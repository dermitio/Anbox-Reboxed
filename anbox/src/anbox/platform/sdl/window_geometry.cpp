#include "anbox/platform/sdl/window_geometry.h"

#include <algorithm>
#include <cstdint>

namespace anbox::platform::sdl {
namespace {
bool usable(const graphics::Rect &rect) {
  return rect != graphics::Rect::Invalid && rect.width() > 0 &&
         rect.height() > 0;
}
}  // namespace

graphics::Rect select_window_work_area(const graphics::Rect &usable_area,
                                       const graphics::Rect &fallback) {
  return usable(usable_area) ? usable_area : fallback;
}

graphics::Rect calculate_single_window_frame(
    const graphics::Rect &requested_client_size,
    const graphics::Rect &work_area) {
  if (!usable(work_area))
    return graphics::Rect::Invalid;

  const bool explicit_size = requested_client_size != graphics::Rect::Invalid;
  int height = requested_client_size.height();
  int width = requested_client_size.width();
  if (!explicit_size) {
    height = static_cast<int>((static_cast<std::int64_t>(work_area.height()) * 9) / 10);
    width = static_cast<int>((static_cast<std::int64_t>(height) * 9 + 10) / 20);
    // Automatic geometry must remain wholly inside even very small or narrow
    // work areas. Explicit client dimensions are intentionally never changed.
    width = std::max(1, std::min(width, work_area.width()));
    height = std::max(1, std::min(height, work_area.height()));
  }

  int left = work_area.left() + (work_area.width() - width) / 2;
  int top = work_area.top() + (work_area.height() - height) / 2;
  if (!explicit_size) {
    left = std::max(work_area.left(), left);
    top = std::max(work_area.top(), top);
  }
  return graphics::Rect{left, top, left + width, top + height};
}

FixedSizeConstraints fixed_size_constraints(const graphics::Rect &frame) {
  return {frame.width(), frame.height(), frame.width(), frame.height()};
}

}  // namespace anbox::platform::sdl
