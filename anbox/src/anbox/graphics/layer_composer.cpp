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

#include "anbox/graphics/layer_composer.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/logger.h"
#include "anbox/wm/manager.h"


namespace anbox::graphics {
LayerComposer::LayerComposer(const std::shared_ptr<Renderer> renderer,
                             const std::shared_ptr<Strategy> &strategy,
                             Mode mode)
    : renderer_(renderer), strategy_(strategy), mode_(mode) {
  if (mode_ == Mode::LatestFrame)
    thread_ = std::thread(&LayerComposer::run, this);
  if (mode_ == Mode::LatestFrame)
    renderer_->set_redraw_requester([this] { request_redraw(); });
}

LayerComposer::~LayerComposer() {
  if (mode_ == Mode::LegacySynchronous)
    return;
  renderer_->set_redraw_requester({});
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  if (thread_.joinable())
    thread_.join();
  if (pending_)
    renderer_->release_frame(pending_->retained_buffers);
  if (last_presented_)
    renderer_->release_frame(last_presented_->retained_buffers);
  INFO("Renderer queue stopped: submitted=%llu presented=%llu dropped=%llu redraws=%llu capacity=1+last",
       static_cast<unsigned long long>(submitted_.load()),
       static_cast<unsigned long long>(presented_.load()),
       static_cast<unsigned long long>(dropped_.load()),
       static_cast<unsigned long long>(redraws_.load()));
}

void LayerComposer::draw_windows(
    const Strategy::WindowRenderableList &windows) {
  for (const auto &w : windows) {
    if (!w.first)
      continue;
    renderer_->draw(w.first->native_handle(),
                    Rect{0, 0, w.first->frame().width(), w.first->frame().height()},
                    w.second);
  }
}

void LayerComposer::present(Frame &frame) {
  draw_windows(frame.windows);
  renderer_->release_frame(frame.retained_buffers);
  ++presented_;
}

void LayerComposer::request_redraw() {
  if (mode_ != Mode::LatestFrame)
    return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    redraw_requested_ = true;
  }
  wake_.notify_one();
}

void LayerComposer::run() {
  for (;;) {
    std::unique_ptr<Frame> frame;
    Strategy::WindowRenderableList redraw_windows;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] {
        return stopping_ || pending_ || redraw_requested_;
      });
      if (stopping_)
        return;
      if (pending_) {
        frame = std::move(pending_);
        redraw_requested_ = false;
      } else {
        redraw_requested_ = false;
        if (last_presented_)
          redraw_windows = last_presented_->windows;
        else
          // Modern gfxstream posts independently of the legacy rcPostLayer
          // callback. Resolve the current host window even before a legacy
          // frame has ever been submitted.
          redraw_windows = strategy_->process_layers({});
      }
    }
    if (!redraw_windows.empty()) {
      draw_windows(redraw_windows);
      ++redraws_;
      continue;
    }
    if (!frame)
      continue;
    draw_windows(frame->windows);
    std::unique_ptr<Frame> old_frame;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      old_frame = std::move(last_presented_);
      last_presented_ = std::move(frame);
    }
    if (old_frame)
      renderer_->release_frame(old_frame->retained_buffers);
    ++presented_;
  }
}

void LayerComposer::submit_layers(const RenderableList &renderables) {
  auto win_layers = strategy_->process_layers(renderables);
  if (mode_ == Mode::LegacySynchronous) {
    Frame frame{std::move(win_layers), {}, ++submitted_};
    present(frame);
    return;
  }

  RenderableList retained;
  std::vector<RenderableList*> frozen_lists;
  for (auto &window : win_layers) {
    if (!window.first)
      continue;
    if (!renderer_->retain_frame(window.second)) {
      for (auto* frozen : frozen_lists)
        renderer_->release_frame(*frozen);
      WARNING("Renderer rejected frame %llu because a ColorBuffer could not be frozen",
              static_cast<unsigned long long>(submitted_.load() + 1));
      return;
    }
    frozen_lists.push_back(&window.second);
    retained.insert(retained.end(), window.second.begin(), window.second.end());
  }

  auto frame = std::make_unique<Frame>();
  frame->windows = std::move(win_layers);
  frame->retained_buffers = std::move(retained);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frame->sequence = ++submitted_;
    if (pending_) {
      renderer_->release_frame(pending_->retained_buffers);
      ++dropped_;
    }
    pending_ = std::move(frame);
  }
  wake_.notify_one();
}
}
