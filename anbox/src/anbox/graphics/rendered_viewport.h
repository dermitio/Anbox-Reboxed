#ifndef ANBOX_GRAPHICS_RENDERED_VIEWPORT_H_
#define ANBOX_GRAPHICS_RENDERED_VIEWPORT_H_

#include "anbox/graphics/coordinate_transform.h"

#include <cstdint>

namespace anbox::graphics {

// Immutable-by-convention publication from the renderer to input. Window
// dimensions are SDL logical coordinates; drawable and viewport dimensions
// are the actual EGL pixel coordinates used by glViewport.
struct RenderedViewport {
  bool valid = false;
  int window_width = 0;
  int window_height = 0;
  int drawable_width = 0;
  int drawable_height = 0;
  Rect viewport = Rect::Empty;
  int android_width = 0;
  int android_height = 0;
  CoordinateTransform::Rotation rotation = CoordinateTransform::Rotation::R0;
  std::uint64_t generation = 0;

  bool map_absolute(int window_x, int window_y, int &android_x,
                    int &android_y) const;
  bool map_relative(int window_dx, int window_dy, int &android_dx,
                    int &android_dy) const;
};

}  // namespace anbox::graphics
#endif
