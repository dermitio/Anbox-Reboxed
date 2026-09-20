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

#ifndef ANBOX_GRAPHICS_EMUGL_DISPLAY_INFO_H_
#define ANBOX_GRAPHICS_EMUGL_DISPLAY_INFO_H_

#include <cstdint>
#include <memory>

namespace anbox::graphics::emugl {
class DisplayInfo {
 public:
  DisplayInfo() = default;

  static std::shared_ptr<DisplayInfo> get();

  void set_resolution(std::uint32_t width, std::uint32_t height);
  void set_density(std::uint32_t dpi);

  std::uint32_t width() const;
  std::uint32_t height() const;
  std::uint32_t density() const;

  // Source compatibility for the old emulator API. These names were
  // misleading: "vertical" contained width and "horizontal" contained
  // height. New code must use width()/height().
  std::uint32_t vertical_resolution() const { return width(); }
  std::uint32_t horizontal_resolution() const { return height(); }

 private:
  std::uint32_t width_ = 720;
  std::uint32_t height_ = 1600;
  std::uint32_t density_ = 320;
};
}
#endif
