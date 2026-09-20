#include "anbox/graphics/rendered_viewport.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "viewport lifecycle self-test failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace

int main() {
  int x = -1;
  int y = -1;

  anbox::graphics::RenderedViewport maximized;
  maximized.valid = true;
  maximized.window_width = 800;       // SDL logical pixels
  maximized.window_height = 600;
  maximized.drawable_width = 1600;   // EGL drawable pixels
  maximized.drawable_height = 1200;
  maximized.viewport = {530, 0, 1070, 1200};
  maximized.android_width = 720;
  maximized.android_height = 1600;
  maximized.generation = 10;
  require(maximized.map_absolute(265, 0, x, y) && x == 0 && y == 0,
          "maximize did not honor drawable viewport offset");
  require(maximized.map_absolute(534, 599, x, y) && x == 719 && y == 1599,
          "maximize did not map the rendered bottom-right edge");
  require(!maximized.map_absolute(100, 300, x, y),
          "letterboxed maximize input was accepted");

  auto transition = maximized;
  transition.valid = false;
  transition.generation = 11;
  require(!transition.map_absolute(400, 300, x, y),
          "resize transition did not suppress pointer input");

  anbox::graphics::RenderedViewport restored;
  restored.valid = true;
  restored.window_width = 720;
  restored.window_height = 1600;
  restored.drawable_width = 720;
  restored.drawable_height = 1600;
  restored.viewport = {0, 0, 720, 1600};
  restored.android_width = 720;
  restored.android_height = 1600;
  restored.generation = 12;
  require(restored.map_absolute(719, 1599, x, y) && x == 719 && y == 1599,
          "restore retained stale maximized coordinates");
  require(restored.generation > transition.generation,
          "restore generation did not advance");

  std::cout << "viewport lifecycle self-test: maximize=10 transition=11 "
               "restore=12 letterbox=rejected logical=800x600 "
               "drawable=1600x1200\n";
  return EXIT_SUCCESS;
}
