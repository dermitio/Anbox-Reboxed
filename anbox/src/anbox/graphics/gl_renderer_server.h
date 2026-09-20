/*
 * Copyright (C) 2016 Simon Fels <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef ANBOX_GRAPHICS_GL_RENDERER_SERVER_H_
#define ANBOX_GRAPHICS_GL_RENDERER_SERVER_H_

#include <memory>
#include <string>

class Renderer;

namespace anbox::input {
  class Manager;
}

namespace anbox::wm {
  class Manager;
}

namespace anbox::graphics {
class LayerComposer;
class GfxstreamBackend;
class GLRendererServer {
 public:
  struct Config {
    enum class Driver {
      // Use the GL driver provided by the host operating system
      Host,

      // Use a builtin software based GL driver implementation without any support
      // for hardware acceleration.
      Software,
    };
    enum class Backend { Reboxed, Legacy };
    Driver driver;
    bool single_window;
    Backend backend = Backend::Reboxed;
    bool allow_software_fallback = false;
    bool use_gfxstream = false;
    bool require_vulkan = false;
    int android_width = 720;
    int android_height = 1600;
  };

  GLRendererServer(const Config &config, const std::shared_ptr<wm::Manager> &wm);
  ~GLRendererServer();

  std::shared_ptr<Renderer> renderer() const { return renderer_; }
  std::shared_ptr<GfxstreamBackend> gfxstream() const { return gfxstream_; }

 private:
  std::shared_ptr<Renderer> renderer_;
  std::shared_ptr<wm::Manager> wm_;
  std::shared_ptr<LayerComposer> composer_;
  std::shared_ptr<GfxstreamBackend> gfxstream_;
};

}
#endif
