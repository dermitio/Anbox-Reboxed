#include "anbox/graphics/rendered_viewport.h"

#include <algorithm>
#include <cmath>

namespace anbox::graphics {
namespace {
Rect logical_viewport(const RenderedViewport &snapshot) {
  if (snapshot.window_width <= 0 || snapshot.window_height <= 0 ||
      snapshot.drawable_width <= 0 || snapshot.drawable_height <= 0)
    return Rect::Empty;
  const auto project_start = [](int value, int logical, int drawable) {
    if (logical <= 1 || drawable <= 1)
      return 0;
    return static_cast<int>(std::ceil(
        static_cast<double>(value) * (logical - 1) / (drawable - 1)));
  };
  const auto project_end = [](int value, int logical, int drawable) {
    if (logical <= 1 || drawable <= 1)
      return logical;
    return static_cast<int>(std::floor(
               static_cast<double>(value - 1) * (logical - 1) /
               (drawable - 1))) + 1;
  };
  return {std::clamp(project_start(snapshot.viewport.left(),
                                   snapshot.window_width,
                                   snapshot.drawable_width),
                     0, snapshot.window_width),
          std::clamp(project_start(snapshot.viewport.top(),
                                   snapshot.window_height,
                                   snapshot.drawable_height),
                     0, snapshot.window_height),
          std::clamp(project_end(snapshot.viewport.right(),
                                 snapshot.window_width,
                                 snapshot.drawable_width),
                     0, snapshot.window_width),
          std::clamp(project_end(snapshot.viewport.bottom(),
                                 snapshot.window_height,
                                 snapshot.drawable_height),
                     0, snapshot.window_height)};
}
}  // namespace

bool RenderedViewport::map_absolute(int window_x, int window_y,
                                    int &android_x, int &android_y) const {
  if (!valid || window_width <= 0 || window_height <= 0 ||
      drawable_width <= 0 || drawable_height <= 0 || window_x < 0 ||
      window_y < 0 || window_x >= window_width || window_y >= window_height)
    return false;
  return CoordinateTransform(logical_viewport(*this), android_width,
                             android_height, rotation)
      .map_absolute(window_x, window_y, android_x, android_y);
}

bool RenderedViewport::map_relative(int window_dx, int window_dy,
                                    int &android_dx, int &android_dy) const {
  if (!valid || window_width <= 0 || window_height <= 0 ||
      drawable_width <= 0 || drawable_height <= 0)
    return false;
  return CoordinateTransform(logical_viewport(*this), android_width,
                             android_height, rotation)
      .map_relative(window_dx, window_dy, android_dx, android_dy);
}
}  // namespace anbox::graphics
