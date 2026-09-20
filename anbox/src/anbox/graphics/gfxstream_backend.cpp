/*
 * Copyright (C) 2026 The Anbox Reboxed Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "anbox/graphics/gfxstream_backend.h"

#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/logger.h"

#ifdef ANBOX_HAVE_GFXSTREAM
#include <gfxstream/virtio-gpu-gfxstream-renderer.h>
#include <render-utils/Renderer.h>

#include <cstdlib>
#include <dlfcn.h>
#include <stdexcept>

extern "C" const gfxstream::RendererPtr& android_getOpenglesRenderer();
extern "C" void android_setPostCallback(
    void (*callback)(void*, std::uint32_t, int, int, int, int, int,
                     unsigned char*),
    void* context, bool use_bgra_readback, std::uint32_t display_id);
extern "C" bool android_asyncReadbackSupported();
extern "C" void (*android_getReadPixelsFunc())(
    void*, std::uint32_t, std::uint32_t);
extern "C" void android_getOpenglesHardwareStrings(char** vendor,
                                                    char** renderer,
                                                    char** version);
#endif

namespace anbox::graphics {
namespace {
#ifdef ANBOX_HAVE_GFXSTREAM
void fence_callback(void*, stream_renderer_fence*) {}

void debug_callback(void*, stream_renderer_debug* message) {
  if (!message || !message->message)
    return;
  if (message->debug_type == STREAM_RENDERER_DEBUG_ERROR)
    ERROR("gfxstream: %s", message->message);
  else if (message->debug_type == STREAM_RENDERER_DEBUG_WARN)
    WARNING("gfxstream: %s", message->message);
  else
    DEBUG("gfxstream: %s", message->message);
}
#endif
}

bool GfxstreamBackend::compiled_in() {
#ifdef ANBOX_HAVE_GFXSTREAM
  return true;
#else
  return false;
#endif
}

GfxstreamBackend::GfxstreamBackend(
    const std::shared_ptr<::Renderer>& presentation_renderer, int width,
    int height, bool require_vulkan)
    : presentation_renderer_(presentation_renderer) {
#ifdef ANBOX_HAVE_GFXSTREAM
  // stream_renderer_init is shared with virtio-gpu frontends and enables
  // VirtioGpuNativeSync by default. Anbox Reboxed carries gfxstream commands
  // over qemu-pipe and deliberately exposes no guest DRM render node. If the
  // feature remains advertised, guest EGL calls VIRTGPU_EXECBUFFER on an
  // invalid (-1) render-node fd for every frame. Disable the capability so
  // the guest uses its defined glFinish synchronization fallback instead.
  constexpr std::uint64_t renderer_features_parameter = 11;
  // NoDelayCloseColorBuffer assumes virtio-gpu resource teardown is the
  // authoritative last reference. Our qemu-pipe transport instead receives
  // process and refcount-pipe teardown independently from SurfaceFlinger.
  // Immediate destruction therefore races late composition and produces
  // intermittent missing icons, black browser surfaces, and
  // "Failed to find ColorBuffer" lookups. Retain gfxstream's delayed-close
  // grace period until all users have naturally moved past the buffer.
  // qemu-pipe transports encoded bytes only.  It exposes neither a goldfish
  // address-space allocator nor virtio-gpu shared resources, so advertising
  // DMA/direct-memory extensions makes guest gralloc allocate ColorBuffers
  // through a path whose backing memory the host cannot read.  Keep gralloc
  // on its ordinary serialized ColorBuffer create/update path.
  constexpr char renderer_features[] =
      "VirtioGpuNativeSync:disabled,NoDelayCloseColorBuffer:disabled,"
      "GlDma:disabled,GlDma2:disabled,GlDirectMem:disabled,"
      "HasSharedSlotsHostMemoryAllocator:disabled";
  const std::uint64_t renderer_flags =
      STREAM_RENDERER_FLAGS_USE_EGL_BIT |
      STREAM_RENDERER_FLAGS_USE_GLES_BIT |
      STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT |
      (require_vulkan ? STREAM_RENDERER_FLAGS_USE_VK_BIT : 0);
  stream_renderer_param parameters[] = {
      {STREAM_RENDERER_PARAM_USER_DATA, 0},
      {STREAM_RENDERER_PARAM_RENDERER_FLAGS, renderer_flags},
      {STREAM_RENDERER_PARAM_FENCE_CALLBACK,
       reinterpret_cast<std::uint64_t>(&fence_callback)},
      {STREAM_RENDERER_PARAM_DEBUG_CALLBACK,
       reinterpret_cast<std::uint64_t>(&debug_callback)},
      {STREAM_RENDERER_PARAM_WIN0_WIDTH, static_cast<std::uint64_t>(width)},
      {STREAM_RENDERER_PARAM_WIN0_HEIGHT, static_cast<std::uint64_t>(height)},
      {renderer_features_parameter,
       reinterpret_cast<std::uint64_t>(renderer_features)},
  };
  if (stream_renderer_init(parameters,
                           sizeof(parameters) / sizeof(parameters[0])) != 0)
    throw std::runtime_error("Failed to initialize modern gfxstream backend");
  initialized_ = true;
  vulkan_enabled_ = require_vulkan;

  // The qemu-pipe refcount service is the authoritative owner of guest
  // ColorBuffers.  Without this, gfxstream also releases the same buffers
  // while tearing down EGL windows and graphics processes.  That double
  // accounting can recycle a still-live handle and surfaces as corrupted app
  // icons, black layers, and "Failed to find ColorBuffer" diagnostics.
  const auto& gfxstream_renderer = android_getOpenglesRenderer();
  if (gfxstream_renderer && gfxstream_renderer->getVirtioGpuOps() &&
      gfxstream_renderer->getVirtioGpuOps()
          ->set_guest_managed_color_buffer_lifetime) {
    gfxstream_renderer->getVirtioGpuOps()
        ->set_guest_managed_color_buffer_lifetime(true);
  } else {
    stream_renderer_teardown();
    initialized_ = false;
    throw std::runtime_error(
        "gfxstream cannot enable guest-managed ColorBuffer lifetime");
  }

  char* vendor = nullptr;
  char* renderer = nullptr;
  char* version = nullptr;
  android_getOpenglesHardwareStrings(&vendor, &renderer, &version);
  if (vendor) gl_vendor_ = vendor;
  if (renderer) gl_renderer_ = renderer;
  if (version) gl_version_ = version;
  std::free(vendor);
  std::free(renderer);
  std::free(version);

  async_readback_ = android_asyncReadbackSupported();
  read_pixels_ = async_readback_ ? android_getReadPixelsFunc() : nullptr;
  if (async_readback_ && !read_pixels_) {
    stream_renderer_teardown();
    initialized_ = false;
    throw std::runtime_error(
        "gfxstream advertises asynchronous readback without a read-pixels function");
  }
  presentation_renderer_->enableExternalFrameSource(width, height);
  android_setPostCallback(&GfxstreamBackend::post_frame, this, false, 0);
  Dl_info runtime_info{};
  const auto runtime_path =
      dladdr(reinterpret_cast<void*>(&stream_renderer_init), &runtime_info) &&
              runtime_info.dli_fname
          ? runtime_info.dli_fname
          : "unknown";
#ifdef ANBOX_GFXSTREAM_PROTOCOL_REVISION
  constexpr const char* protocol_revision =
      ANBOX_GFXSTREAM_PROTOCOL_REVISION;
#else
  constexpr const char* protocol_revision = "unknown";
#endif
  INFO("Graphics transport=gfxstream protocol=%s runtime='%s' GLES='%s' renderer='%s' vendor='%s' Vulkan=%s readback=%s native_sync=glFinish colorbuffer_close=delayed frame_queue=latest-only",
       protocol_revision, runtime_path,
       gl_version_.c_str(), gl_renderer_.c_str(), gl_vendor_.c_str(),
       vulkan_enabled_ ? "enabled" : "disabled",
       async_readback_ ? "async" : "synchronous");
#else
  (void)width;
  (void)height;
  (void)require_vulkan;
  throw std::runtime_error(
      "This build does not include the modern gfxstream backend");
#endif
}

GfxstreamBackend::~GfxstreamBackend() {
#ifdef ANBOX_HAVE_GFXSTREAM
  if (!initialized_)
    return;
  android_setPostCallback(nullptr, nullptr, false, 0);
  presentation_renderer_->disableExternalFrameSource();
  stream_renderer_teardown();
#endif
}

gfxstream::RenderChannelPtr GfxstreamBackend::create_render_channel() {
#ifdef ANBOX_HAVE_GFXSTREAM
  const auto& renderer = android_getOpenglesRenderer();
  return renderer ? renderer->createRenderChannel() : nullptr;
#else
  return {};
#endif
}

void GfxstreamBackend::open_color_buffer(std::uint32_t handle) {
#ifdef ANBOX_HAVE_GFXSTREAM
  const auto& renderer = android_getOpenglesRenderer();
  if (renderer && renderer->getVirtioGpuOps() &&
      renderer->getVirtioGpuOps()->open_color_buffer)
    renderer->getVirtioGpuOps()->open_color_buffer(handle);
#else
  (void)handle;
#endif
}

void GfxstreamBackend::close_color_buffer(std::uint32_t handle) {
#ifdef ANBOX_HAVE_GFXSTREAM
  const auto& renderer = android_getOpenglesRenderer();
  if (renderer && renderer->getVirtioGpuOps() &&
      renderer->getVirtioGpuOps()->close_color_buffer)
    renderer->getVirtioGpuOps()->close_color_buffer(handle);
#else
  (void)handle;
#endif
}

void GfxstreamBackend::create_process(std::uint64_t id) {
#ifdef ANBOX_HAVE_GFXSTREAM
  const auto& renderer = android_getOpenglesRenderer();
  if (renderer)
    renderer->onGuestGraphicsProcessCreate(id);
#else
  (void)id;
#endif
}

void GfxstreamBackend::cleanup_process(std::uint64_t id) {
#ifdef ANBOX_HAVE_GFXSTREAM
  const auto& renderer = android_getOpenglesRenderer();
  if (renderer)
    renderer->cleanupProcGLObjects(id);
#else
  (void)id;
#endif
}

void GfxstreamBackend::post_frame(void* context, std::uint32_t display_id,
                                  int width, int height, int y_direction,
                                  int format, int type,
                                  unsigned char* pixels) {
  if (display_id != 0 || !context)
    return;
  auto* backend = static_cast<GfxstreamBackend*>(context);
  constexpr std::uint64_t max_frame_bytes = 64U * 1024U * 1024U;
  if (!pixels || width <= 0 || height <= 0)
    return;
  const auto bytes = static_cast<std::uint64_t>(width) *
                     static_cast<std::uint64_t>(height) * 4U;
  if (bytes > max_frame_bytes || bytes > UINT32_MAX) {
    WARNING("gfxstream readback rejected: size=%dx%d bytes=%llu",
            width, height, static_cast<unsigned long long>(bytes));
    return;
  }
  // With gfxstream's asynchronous PBO pipeline the post callback only marks a
  // completed buffer as available. The supplied staging pointer is not filled
  // until the consumer explicitly drains that buffer through ReadPixelsFunc.
  if (backend->async_readback_)
    backend->read_pixels_(pixels, static_cast<std::uint32_t>(bytes), display_id);
  backend->presentation_renderer_->postExternalFrame(
      width, height, y_direction, format, type, pixels);
}
}
