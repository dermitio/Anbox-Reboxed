/*
 * Copyright (C) 2026 The Anbox Reboxed Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ANBOX_GRAPHICS_GFXSTREAM_BACKEND_H_
#define ANBOX_GRAPHICS_GFXSTREAM_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>

class Renderer;

namespace anbox::network {
class SocketMessenger;
}

namespace gfxstream {
class RenderChannel;
using RenderChannelPtr = std::shared_ptr<RenderChannel>;
}

namespace anbox::graphics {
class GfxstreamBackend {
 public:
  GfxstreamBackend(const std::shared_ptr<::Renderer>& presentation_renderer,
                   int width, int height, bool require_vulkan);
  ~GfxstreamBackend();

  static bool compiled_in();
  gfxstream::RenderChannelPtr create_render_channel();
  void open_color_buffer(std::uint32_t handle);
  void close_color_buffer(std::uint32_t handle);
  void create_process(std::uint64_t id);
  void cleanup_process(std::uint64_t id);

  const std::string& gl_vendor() const { return gl_vendor_; }
  const std::string& gl_renderer() const { return gl_renderer_; }
  const std::string& gl_version() const { return gl_version_; }
  bool vulkan_enabled() const { return vulkan_enabled_; }

 private:
  using ReadPixelsFunction = void (*)(void*, std::uint32_t, std::uint32_t);

  static void post_frame(void* context, std::uint32_t display_id, int width,
                         int height, int y_direction, int format, int type,
                         unsigned char* pixels);

  std::shared_ptr<::Renderer> presentation_renderer_;
  bool initialized_ = false;
  bool vulkan_enabled_ = false;
  bool async_readback_ = false;
  ReadPixelsFunction read_pixels_ = nullptr;
  std::string gl_vendor_;
  std::string gl_renderer_;
  std::string gl_version_;
};
}

#endif
