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

#include "DisplayManager.h"

namespace anbox::graphics::emugl {
std::shared_ptr<DisplayInfo> DisplayInfo::get() {
  static auto info = std::make_shared<DisplayInfo>();
  return info;
}

void DisplayInfo::set_resolution(std::uint32_t width, std::uint32_t height) {
  width_ = width;
  height_ = height;
}

void DisplayInfo::set_density(std::uint32_t dpi) { density_ = dpi; }

std::uint32_t DisplayInfo::width() const { return width_; }
std::uint32_t DisplayInfo::height() const { return height_; }
std::uint32_t DisplayInfo::density() const { return density_; }
}
