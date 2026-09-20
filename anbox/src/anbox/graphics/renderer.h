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

#ifndef ANBOX_GRAPHICS_RENDERER_H_
#define ANBOX_GRAPHICS_RENDERER_H_

#include "anbox/graphics/emugl/Renderable.h"

#include <EGL/egl.h>
#include <functional>

namespace anbox::graphics {
class Renderer {
 public:
  virtual ~Renderer() {}

  virtual bool draw(EGLNativeWindowType native_window,
                    const anbox::graphics::Rect& window_frame,
                    const RenderableList& renderables) = 0;

  // Freeze the mutable guest ColorBuffers named by a queued frame. The
  // renderer may replace their handles with immutable host-owned snapshots.
  // Backends without asynchronous presentation may keep the defaults.
  virtual bool retain_frame(RenderableList&) { return true; }
  virtual void release_frame(const RenderableList&) {}
  // The host-window backend uses this to request a repaint of the newest
  // retained frame after compositor lifecycle changes.
  virtual void set_redraw_requester(std::function<void()> requester) {
    (void)requester;
  }
};
}
#endif
