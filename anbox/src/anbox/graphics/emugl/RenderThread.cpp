/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "anbox/graphics/emugl/RenderThread.h"
#include "anbox/graphics/emugl/ReadBuffer.h"
#include "anbox/graphics/emugl/RenderControl.h"
#include "anbox/graphics/emugl/RenderThreadInfo.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/buffered_io_stream.h"
#include "anbox/graphics/emugl/TimeUtils.h"
#include "anbox/logger.h"

#include "external/android-emugl/shared/OpenglCodecCommon/ChecksumCalculatorThreadInfo.h"
#include "external/android-emugl/host/include/OpenGLESDispatch/EGLDispatch.h"
#include "external/android-emugl/host/include/OpenGLESDispatch/GLESv1Dispatch.h"
#include "external/android-emugl/host/include/OpenGLESDispatch/GLESv2Dispatch.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

#define STREAM_BUFFER_SIZE 4 * 1024 * 1024

namespace {
constexpr std::uint32_t kRcGetHostExtensionsString = 10069;
constexpr std::uint32_t kApi35RcSetPuid = 10033;
constexpr std::uint32_t kApi35RcFlushWindowColorBufferAsync = 10031;
constexpr std::uint32_t kApi35RcSetDisplayColorBuffer = 10040;
constexpr std::uint32_t kApi35RcGetDisplayColorBuffer = 10041;
constexpr std::uint32_t kApi35RcCreateDisplayById = 10062;
constexpr std::uint32_t kApi35RcSetDisplayPoseDpi = 10063;
constexpr std::uint32_t kApi35RcCreateColorBufferDma = 10035;
constexpr std::uint32_t kRcBindTexture = 10020;
std::mutex display_color_buffers_mutex;
std::map<std::uint32_t, std::uint32_t> display_color_buffers;

std::uint32_t readProtocolU32(const void *buffer, std::size_t word) {
  std::uint32_t value = 0;
  const auto *bytes = static_cast<const unsigned char *>(buffer);
  std::memcpy(&value, bytes + word * sizeof(value), sizeof(value));
  return value;
}

size_t decodeModernRenderControlCompatibility(void *buffer, size_t length,
                                              IOStream *stream,
                                              Renderer *renderer) {
  if (length < 8)
    return 0;

  const std::uint32_t opcode = readProtocolU32(buffer, 0);
  const std::uint32_t packet_length = readProtocolU32(buffer, 1);
  if (packet_length < 8 || length < packet_length)
    return 0;

  // GLProcessPipe is an Android 15 transport negotiation. Its value is not
  // an ownership identity: resource lifetime is tracked exclusively by the
  // renderer handles below. Consume the matching renderControl packet so the
  // stream remains aligned, but deliberately retain no association.
  if (opcode == kApi35RcSetPuid && packet_length == 16)
    return packet_length;

  // Android 15 assigned opcode 10031 to the void asynchronous window flush.
  // The legacy Anbox decoder assigns the same opcode to rcGetDisplayHeight
  // and consequently writes an unsolicited four-byte response. That response
  // corrupts the next synchronous return value on this pipe (most visibly a
  // later rcCreateWindowSurface handle). Perform the flush and, as required by
  // the Android 15 wire signature, do not write a response.
  if (opcode == kApi35RcFlushWindowColorBufferAsync && packet_length == 12) {
    const std::uint32_t window_surface = readProtocolU32(buffer, 2);
    if (!renderer->flushWindowSurfaceColorBuffer(window_surface))
      WARNING("API 35 rcFlushWindowColorBufferAsync failed for surface %#x",
              window_surface);
    return packet_length;
  }

  if (opcode == kApi35RcCreateDisplayById && packet_length == 12) {
    const std::uint32_t display_id = readProtocolU32(buffer, 2);
    const std::uint32_t result = 0;
    auto *response = static_cast<unsigned char *>(stream->alloc(sizeof(result)));
    std::memcpy(response, &result, sizeof(result));
    stream->flush();
    INFO("Handled API 35 rcCreateDisplayById: display_id=%u result=%u",
         display_id, result);
    return packet_length;
  }

  if (opcode == kApi35RcSetDisplayColorBuffer && packet_length == 16) {
    const std::uint32_t display_id = readProtocolU32(buffer, 2);
    const std::uint32_t color_buffer = readProtocolU32(buffer, 3);
    {
      std::lock_guard<std::mutex> lock(display_color_buffers_mutex);
      display_color_buffers[display_id] = color_buffer;
    }
    const std::int32_t result = 0;
    auto *response = static_cast<unsigned char *>(stream->alloc(sizeof(result)));
    std::memcpy(response, &result, sizeof(result));
    stream->flush();
    INFO("Handled API 35 rcSetDisplayColorBuffer: display_id=%u color_buffer=%u result=%d",
         display_id, color_buffer, result);
    return packet_length;
  }

  if (opcode == kApi35RcGetDisplayColorBuffer && packet_length == 16) {
    const std::uint32_t display_id = readProtocolU32(buffer, 2);
    std::uint32_t color_buffer = 0;
    {
      std::lock_guard<std::mutex> lock(display_color_buffers_mutex);
      const auto found = display_color_buffers.find(display_id);
      if (found != display_color_buffers.end())
        color_buffer = found->second;
    }
    auto *response =
        static_cast<unsigned char *>(stream->alloc(sizeof(color_buffer)));
    std::memcpy(response, &color_buffer, sizeof(color_buffer));
    stream->flush();
    INFO("Handled API 35 rcGetDisplayColorBuffer: display_id=%u color_buffer=%u",
         display_id, color_buffer);
    return packet_length;
  }

  if (opcode == kApi35RcSetDisplayPoseDpi && packet_length == 32) {
    const std::uint32_t result = 0;
    auto *response = static_cast<unsigned char *>(stream->alloc(sizeof(result)));
    std::memcpy(response, &result, sizeof(result));
    stream->flush();
    INFO("Handled API 35 rcSetDisplayPoseDpi: display_id=%u size=%ux%u dpi=%u result=%u",
         readProtocolU32(buffer, 2), readProtocolU32(buffer, 5),
         readProtocolU32(buffer, 6), readProtocolU32(buffer, 7), result);
    return packet_length;
  }

  if (opcode == kApi35RcCreateColorBufferDma && packet_length == 24) {
    const std::uint32_t host_format = readProtocolU32(buffer, 4);
    const std::uint32_t handle =
        renderer->createColorBuffer(readProtocolU32(buffer, 2),
                                    readProtocolU32(buffer, 3), host_format);
    auto *response = static_cast<unsigned char *>(stream->alloc(sizeof(handle)));
    std::memcpy(response, &handle, sizeof(handle));
    stream->flush();
    INFO("Handled API 35 rcCreateColorBufferDMA: size=%ux%u guest_format=%u host_format=%u framework_format=%u handle=%u",
         readProtocolU32(buffer, 2), readProtocolU32(buffer, 3),
         readProtocolU32(buffer, 4), host_format,
         readProtocolU32(buffer, 5), handle);
    return packet_length;
  }

  if (opcode == kRcBindTexture && packet_length == 12) {
    const std::uint32_t color_buffer = readProtocolU32(buffer, 2);
    if (!renderer->bindColorBufferToTexture(color_buffer))
      WARNING("API 35 rcBindTexture failed for color buffer %#x",
              color_buffer);
    return packet_length;
  }

  if (opcode != kRcGetHostExtensionsString)
    return 0;

  const std::uint32_t output_size = readProtocolU32(buffer, 2);
  const std::uint32_t guest_buffer_size = readProtocolU32(buffer, 3);
  const std::size_t response_size = output_size + sizeof(std::int32_t);
  auto *response = static_cast<unsigned char *>(stream->alloc(response_size));
  std::memset(response, 0, output_size);

  // Android 15 RenderEngine requires an ES 3 context. Keep every other
  // optional modern gfxstream feature disabled.
  constexpr char extensions[] = "ANDROID_EMU_gles_max_version_3_0";
  const auto copy_size = std::min<std::size_t>(
      sizeof(extensions), std::min(output_size, guest_buffer_size));
  if (copy_size > 0)
    std::memcpy(response, extensions, copy_size);
  const std::int32_t extension_size =
      guest_buffer_size == 0 ? 0 : static_cast<std::int32_t>(sizeof(extensions));
  std::memcpy(response + output_size, &extension_size,
              sizeof(extension_size));
  stream->flush();
  INFO("Handled API 35 rcGetHostExtensionsString: output=%u guest_buffer=%u response=%zu extension_size=%d",
       output_size, guest_buffer_size, response_size, extension_size);
  return packet_length;
}
}

RenderThread::RenderThread(const std::shared_ptr<Renderer> &renderer, IOStream *stream, std::recursive_mutex &m)
    : emugl::Thread(), renderer_(renderer), m_lock(m), m_stream(stream) {}

RenderThread::~RenderThread() {
  forceStop();
}

RenderThread *RenderThread::create(const std::shared_ptr<Renderer> &renderer, IOStream *stream, std::recursive_mutex &m) {
  return new RenderThread(renderer, stream, m);
}

void RenderThread::forceStop() { m_stream->forceStop(); }

intptr_t RenderThread::main() {
  RenderThreadInfo threadInfo;
  ChecksumCalculatorThreadInfo threadChecksumInfo;

  threadInfo.m_glDec.initGL(gles1_dispatch_get_proc_func, NULL);
  threadInfo.m_gl2Dec.initGL(gles2_dispatch_get_proc_func, NULL);
  initRenderControlContext(&threadInfo.m_rcDec);

  ReadBuffer readBuf(STREAM_BUFFER_SIZE);

  while (true) {
    int stat = readBuf.getData(m_stream);
    if (stat <= 0)
      break;

    bool progress;
    do {
      progress = false;

      std::unique_lock<std::recursive_mutex> l(m_lock);

      if (readBuf.validData() < 8)
        break;
      const std::uint32_t opcode = readProtocolU32(readBuf.buf(), 0);
      const size_t packet_size = readProtocolU32(readBuf.buf(), 1);
      if (packet_size < 8 || packet_size > readBuf.validData())
        break;
      if (auto* traced = dynamic_cast<anbox::graphics::BufferedIOStream*>(m_stream))
        traced->trace_request(opcode, packet_size);

      size_t last = decodeModernRenderControlCompatibility(
          readBuf.buf(), packet_size, m_stream, renderer_.get());
      if (last > 0) {
        progress = true;
        readBuf.consume(last);
        continue;
      }

      last = threadInfo.m_glDec.decode(readBuf.buf(), packet_size, m_stream);
      if (last > 0) {
        progress = true;
        readBuf.consume(last);
        continue;
      }

      last = threadInfo.m_gl2Dec.decode(readBuf.buf(), packet_size, m_stream);
      if (last > 0) {
        progress = true;
        readBuf.consume(last);
        continue;
      }

      last = threadInfo.m_rcDec.decode(readBuf.buf(), packet_size, m_stream);
      if (last > 0) {
        readBuf.consume(last);
        progress = true;
      }

    } while (progress);

  }

  // Release references to the current thread's context/surfaces if any
  renderer_->bindContext(0, 0, 0);
  if (threadInfo.currContext || threadInfo.currDrawSurf || threadInfo.currReadSurf)
    ERROR("RenderThread exiting with current context/surfaces");

  threadInfo.m_gl2Dec.freeShader();
  threadInfo.m_gl2Dec.freeProgram();
  renderer_->deferCurrentThreadResources();

  return 0;
}
