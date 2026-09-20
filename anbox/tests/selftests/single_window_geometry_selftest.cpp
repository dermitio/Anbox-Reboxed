#include "anbox/graphics/coordinate_transform.h"
#include "anbox/graphics/emugl/TextureResize.h"
#include "anbox/platform/sdl/window_geometry.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "single-window geometry self-test failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace

int main() {
  using anbox::graphics::Rect;
  using namespace anbox::platform::sdl;

  const Rect work_area{100, 40, 1300, 1040};
  const auto explicit_frame =
      calculate_single_window_frame(Rect{640, 900}, work_area);
  require(explicit_frame.width() == 640 && explicit_frame.height() == 900,
          "explicit client size changed");
  require(explicit_frame.left() == 380 && explicit_frame.top() == 90,
          "explicit client was not centered");

  const auto automatic =
      calculate_single_window_frame(Rect::Invalid, work_area);
  require(automatic.height() == 900 && automatic.width() == 405,
          "automatic size is not 90%-height portrait 20:9");
  require(automatic.left() == 497 && automatic.top() == 90,
          "automatic client was not centered in the work area");

  const Rect screen{0, 0, 1920, 1080};
  require(select_window_work_area(Rect::Invalid, screen) == screen,
          "no-WM screen fallback failed");
  require(select_window_work_area(work_area, screen) == work_area,
          "usable work area was not preferred");

  const Rect narrow{20, 30, 320, 1030};
  const auto clamped = calculate_single_window_frame(Rect::Invalid, narrow);
  require(clamped.width() == 300 && clamped.left() == 20,
          "automatic width was not clamped to the visible work area");

  const auto constraints = fixed_size_constraints(automatic);
  require(constraints.minimum_width == automatic.width() &&
              constraints.maximum_width == automatic.width() &&
              constraints.minimum_height == automatic.height() &&
              constraints.maximum_height == automatic.height(),
          "fixed-size min/max constraints differ");

  // A matching 20:9 Android buffer and drawable must occupy the entire host
  // client, including when logical and drawable pixel dimensions differ.
  const anbox::graphics::CoordinateTransform hidpi_portrait(
      810, 1800, 720, 1600);
  require(hidpi_portrait.viewport() == Rect{0, 0, 810, 1800},
          "matching portrait buffer was letterboxed");

  const anbox::graphics::CoordinateTransform rotated(
      1600, 720, 720, 1600,
      anbox::graphics::CoordinateTransform::Rotation::R90);
  require(rotated.viewport() == Rect{0, 0, 1600, 720},
          "90-degree transform did not swap source dimensions");
  int android_x = -1;
  int android_y = -1;
  require(rotated.map_absolute(0, 0, android_x, android_y) &&
              android_x == 0 && android_y == 1599,
          "90-degree corner mapping is incorrect");

  const anbox::graphics::CoordinateTransform upside_down(
      Rect{0, 0, 720, 1600}, 720, 1600,
      anbox::graphics::CoordinateTransform::Rotation::R180);
  require(upside_down.map_absolute(0, 0, android_x, android_y) &&
              android_x == 719 && android_y == 1599,
          "180-degree corner mapping is incorrect");

  require(TextureResize::calculateFactor(720, 1600, 427, 948) == 1,
          "20:9 presentation was unnecessarily downsampled");
  require(TextureResize::calculateFactor(720, 1600, 320, 711) == 2,
          "two-pass resize was not selected for a smaller drawable");
  require(TextureResize::calculateFactor(720, 1600, 180, 400) == 4,
          "repeated buffer resize selected the wrong factor");

  std::cout << "single-window geometry self-test: explicit=640x900 "
               "automatic=405x900 fallback=1920x1080 fixed=true "
               "hidpi=true rotations=true resize-factors=true\n";
  return EXIT_SUCCESS;
}
