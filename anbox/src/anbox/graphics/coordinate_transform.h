#ifndef ANBOX_GRAPHICS_COORDINATE_TRANSFORM_H_
#define ANBOX_GRAPHICS_COORDINATE_TRANSFORM_H_

#include "anbox/graphics/rect.h"

#include <cstdint>

namespace anbox::graphics {

class CoordinateTransform {
 public:
  enum class Rotation { R0 = 0, R90 = 90, R180 = 180, R270 = 270 };

  CoordinateTransform(int host_width, int host_height, int android_width,
                      int android_height, Rotation rotation = Rotation::R0);
  // Use an already-selected rendered viewport. This is the input-side form:
  // it must not independently repeat the renderer's aspect-fit calculation.
  CoordinateTransform(const Rect &viewport, int android_width,
                      int android_height, Rotation rotation = Rotation::R0);

  Rect viewport() const { return viewport_; }
  bool map_absolute(int host_x, int host_y, int &android_x,
                    int &android_y) const;
  bool map_relative(int host_dx, int host_dy, int &android_dx,
                    int &android_dy) const;
  bool valid() const { return valid_; }

 private:
  int android_width_ = 0;
  int android_height_ = 0;
  Rotation rotation_ = Rotation::R0;
  Rect viewport_ = Rect::Empty;
  bool valid_ = false;
};

}  // namespace anbox::graphics
#endif
