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

#ifndef ANBOX_GRAPHICS_LAYER_COMPOSER_H_
#define ANBOX_GRAPHICS_LAYER_COMPOSER_H_

#include "anbox/graphics/renderer.h"

#include <memory>
#include <map>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <thread>

namespace anbox::wm {
  class Manager;
  class Window;
}

namespace anbox::graphics {
class LayerComposer {
 public:
  enum class Mode { LegacySynchronous, LatestFrame };
  class Strategy {
   public:
    typedef std::map<std::shared_ptr<wm::Window>, RenderableList> WindowRenderableList;

    virtual ~Strategy() {}
    virtual WindowRenderableList process_layers(const RenderableList &renderables) = 0;
  };

  LayerComposer(const std::shared_ptr<Renderer> renderer,
                const std::shared_ptr<Strategy> &strategy,
                Mode mode = Mode::LegacySynchronous);
  ~LayerComposer();

  void submit_layers(const RenderableList &renderables);
  void request_redraw();

 private:
  struct Frame {
    Strategy::WindowRenderableList windows;
    RenderableList retained_buffers;
    std::uint64_t sequence = 0;
  };

  void present(Frame &frame);
  void draw_windows(const Strategy::WindowRenderableList &windows);
  void run();

  std::shared_ptr<Renderer> renderer_;
  std::shared_ptr<Strategy> strategy_;
  Mode mode_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::unique_ptr<Frame> pending_;
  std::unique_ptr<Frame> last_presented_;
  std::thread thread_;
  bool stopping_ = false;
  bool redraw_requested_ = false;
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> presented_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> redraws_{0};
};
}
#endif
