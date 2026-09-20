#include "anbox/cmds/display_status.h"

#include <SDL.h>
#include <cstdlib>
#include <iostream>

anbox::cmds::DisplayStatus::DisplayStatus()
    : CommandWithFlagsAndAction{cli::Name{"display-status"},
                                cli::Usage{"display-status"},
                                cli::Description{"Show native Anbox Reboxed window orientation support"}} {
  action([](const cli::Command::Context &) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
      std::cerr << "display-available: false\nerror: " << SDL_GetError() << std::endl;
      return EXIT_FAILURE;
    }
    SDL_DisplayMode mode{};
    const auto ok = SDL_GetCurrentDisplayMode(0, &mode) == 0;
    std::cout << "display-available: " << (ok ? "true" : "false") << '\n';
    if (ok) {
      std::cout << "desktop-size: " << mode.w << 'x' << mode.h << '\n'
                << "native-framebuffer-orientation: true\n"
                << "pixel-rotation-required: false\n"
                << "input-coordinate-mapping: identity\n"
                << "portrait-supported: " << (mode.h >= 720 ? "true" : "false") << '\n';
    } else {
      std::cout << "error: " << SDL_GetError() << '\n';
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  });
}
