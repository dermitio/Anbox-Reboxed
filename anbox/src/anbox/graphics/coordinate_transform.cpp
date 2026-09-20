#include "anbox/graphics/coordinate_transform.h"

#include <algorithm>
#include <cmath>

namespace anbox::graphics {
CoordinateTransform::CoordinateTransform(int host_width, int host_height,
                                         int android_width,
                                         int android_height,
                                         Rotation rotation)
    : android_width_(android_width),
      android_height_(android_height),
      rotation_(rotation) {
  if (host_width <= 0 || host_height <= 0 || android_width <= 0 ||
      android_height <= 0)
    return;

  const bool quarter_turn = rotation == Rotation::R90 ||
                            rotation == Rotation::R270;
  const int source_width = quarter_turn ? android_height : android_width;
  const int source_height = quarter_turn ? android_width : android_height;
  int width = host_width;
  int height = std::max(
      1, static_cast<int>(static_cast<long long>(host_width) * source_height /
                          source_width));
  if (height > host_height) {
    height = host_height;
    width = std::max(
        1, static_cast<int>(static_cast<long long>(host_height) * source_width /
                            source_height));
  }
  const int left = (host_width - width) / 2;
  const int top = (host_height - height) / 2;
  viewport_ = Rect{left, top, left + width, top + height};
  valid_ = true;
}

CoordinateTransform::CoordinateTransform(const Rect &viewport,
                                         int android_width,
                                         int android_height,
                                         Rotation rotation)
    : android_width_(android_width),
      android_height_(android_height),
      rotation_(rotation),
      viewport_(viewport) {
  valid_ = viewport.width() > 0 && viewport.height() > 0 &&
           android_width > 0 && android_height > 0;
}

bool CoordinateTransform::map_absolute(int host_x, int host_y, int &android_x,
                                       int &android_y) const {
  if (!valid_ || host_x < viewport_.left() || host_x >= viewport_.right() ||
      host_y < viewport_.top() || host_y >= viewport_.bottom())
    return false;

  const double u = viewport_.width() <= 1
      ? 0.0
      : static_cast<double>(host_x - viewport_.left()) /
            static_cast<double>(viewport_.width() - 1);
  const double v = viewport_.height() <= 1
      ? 0.0
      : static_cast<double>(host_y - viewport_.top()) /
            static_cast<double>(viewport_.height() - 1);
  double x = u;
  double y = v;
  switch (rotation_) {
    case Rotation::R0: break;
    case Rotation::R90: x = v; y = 1.0 - u; break;
    case Rotation::R180: x = 1.0 - u; y = 1.0 - v; break;
    case Rotation::R270: x = 1.0 - v; y = u; break;
    default: return false;
  }
  android_x = std::clamp(static_cast<int>(std::lround(
                             x * (android_width_ - 1))),
                         0, android_width_ - 1);
  android_y = std::clamp(static_cast<int>(std::lround(
                             y * (android_height_ - 1))),
                         0, android_height_ - 1);
  return true;
}

bool CoordinateTransform::map_relative(int host_dx, int host_dy,
                                       int &android_dx, int &android_dy) const {
  if (!valid_)
    return false;
  const double sx = static_cast<double>(android_width_) /
                    std::max(1, viewport_.width());
  const double sy = static_cast<double>(android_height_) /
                    std::max(1, viewport_.height());
  switch (rotation_) {
    case Rotation::R0:
      android_dx = std::lround(host_dx * sx);
      android_dy = std::lround(host_dy * sy);
      break;
    case Rotation::R90:
      android_dx = std::lround(host_dy * sx);
      android_dy = std::lround(-host_dx * sy);
      break;
    case Rotation::R180:
      android_dx = std::lround(-host_dx * sx);
      android_dy = std::lround(-host_dy * sy);
      break;
    case Rotation::R270:
      android_dx = std::lround(-host_dy * sx);
      android_dy = std::lround(host_dx * sy);
      break;
    default: return false;
  }
  return true;
}
}  // namespace anbox::graphics
