#ifndef ANBOX_PLATFORM_SDL_WINDOW_GEOMETRY_H_
#define ANBOX_PLATFORM_SDL_WINDOW_GEOMETRY_H_

#include "anbox/graphics/rect.h"

namespace anbox::platform::sdl {

struct FixedSizeConstraints {
  int minimum_width = 0;
  int minimum_height = 0;
  int maximum_width = 0;
  int maximum_height = 0;
};

graphics::Rect select_window_work_area(const graphics::Rect &usable,
                                       const graphics::Rect &fallback);
graphics::Rect calculate_single_window_frame(
    const graphics::Rect &requested_client_size,
    const graphics::Rect &work_area);
FixedSizeConstraints fixed_size_constraints(const graphics::Rect &frame);

}  // namespace anbox::platform::sdl

#endif
