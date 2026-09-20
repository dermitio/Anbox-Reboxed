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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-default"
#include "anbox/platform/sdl/platform.h"
#include "anbox/input/device.h"
#include "anbox/input/manager.h"
#include "anbox/graphics/coordinate_transform.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/logger.h"
#include "anbox/platform/sdl/keycode_converter.h"
#include "anbox/platform/sdl/window.h"
#include "anbox/platform/sdl/window_geometry.h"
#include "anbox/platform/sdl/audio_sink.h"
#include "anbox/wm/manager.h"

#include <boost/throw_exception.hpp>

#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#if defined(X11_SUPPORT)
#include <X11/Xlib.h>
#endif
#pragma GCC diagnostic pop

namespace anbox::platform::sdl {
namespace {
constexpr int pointer_axis_max = 65535;

struct DisplayWorkArea {
  int display_index = 0;
  graphics::Rect area = graphics::Rect::Invalid;
  bool usable_bounds = false;
};

graphics::Rect x11_root_bounds() {
#if defined(X11_SUPPORT)
  const char *video_driver = SDL_GetCurrentVideoDriver();
  if (!video_driver || std::string{video_driver} != "x11")
    return graphics::Rect::Invalid;
  Display *display = XOpenDisplay(nullptr);
  if (!display)
    return graphics::Rect::Invalid;

  XWindowAttributes attributes{};
  const auto root = RootWindow(display, DefaultScreen(display));
  const bool valid = XGetWindowAttributes(display, root, &attributes) != 0 &&
                     attributes.width > 0 && attributes.height > 0;
  XCloseDisplay(display);
  if (valid)
    return {0, 0, attributes.width, attributes.height};
#endif
  return graphics::Rect::Invalid;
}

graphics::Rect intersect_rect(const graphics::Rect &rect,
                              const graphics::Rect &bounds) {
  if (rect == graphics::Rect::Invalid || bounds == graphics::Rect::Invalid)
    return graphics::Rect::Invalid;
  const graphics::Rect intersection{
      std::max(rect.left(), bounds.left()),
      std::max(rect.top(), bounds.top()),
      std::min(rect.right(), bounds.right()),
      std::min(rect.bottom(), bounds.bottom())};
  if (intersection.width() <= 0 || intersection.height() <= 0)
    return graphics::Rect::Invalid;
  return intersection;
}

DisplayWorkArea initial_display_work_area(const graphics::Rect &fallback) {
  DisplayWorkArea result;
  int pointer_x = 0;
  int pointer_y = 0;
  SDL_GetGlobalMouseState(&pointer_x, &pointer_y);
  const int display_count = SDL_GetNumVideoDisplays();
  for (int index = 0; index < display_count; ++index) {
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(index, &bounds) != 0)
      continue;
    if (pointer_x >= bounds.x && pointer_x < bounds.x + bounds.w &&
        pointer_y >= bounds.y && pointer_y < bounds.y + bounds.h) {
      result.display_index = index;
      break;
    }
  }

  SDL_Rect display_bounds{};
  if (SDL_GetDisplayBounds(result.display_index, &display_bounds) == 0) {
    graphics::Rect monitor{display_bounds.x, display_bounds.y,
                           display_bounds.x + display_bounds.w,
                           display_bounds.y + display_bounds.h};
    const auto root_bounds = x11_root_bounds();
    const auto visible_monitor = intersect_rect(monitor, root_bounds);
    if (visible_monitor != graphics::Rect::Invalid)
      monitor = visible_monitor;
    SDL_Rect usable_bounds{};
    graphics::Rect usable_area = graphics::Rect::Invalid;
    if (SDL_GetDisplayUsableBounds(result.display_index, &usable_bounds) == 0 &&
        usable_bounds.w > 0 && usable_bounds.h > 0) {
      usable_area = {usable_bounds.x, usable_bounds.y,
                     usable_bounds.x + usable_bounds.w,
                     usable_bounds.y + usable_bounds.h};
      if (root_bounds != graphics::Rect::Invalid)
        usable_area = intersect_rect(usable_area, root_bounds);
      result.usable_bounds = usable_area != monitor;
    }
    result.area = select_window_work_area(usable_area, monitor);
  } else {
    result.area = fallback;
  }
  return result;
}

void forward_dropped_file(const char *path) {
  if (!path || !*path)
    return;
  const auto child = ::fork();
  if (child < 0) {
    WARNING("Failed to fork drag-and-drop bridge helper");
    return;
  }
  if (child == 0) {
    const auto sender = ::fork();
    if (sender < 0)
      ::_exit(126);
    if (sender == 0) {
      ::execl("/proc/self/exe", "anbox-reboxed", "bridge", "send", path,
              static_cast<char *>(nullptr));
      ::_exit(127);
    }
    ::_exit(0);
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}
}  // namespace
Platform::Platform(
    const std::shared_ptr<input::Manager> &input_manager,
    const Configuration &config)
    : input_manager_(input_manager),
      event_thread_running_(false),
      config_(config) {

  // Don't block the screensaver from kicking in. It will be blocked
  // by the desktop shell already and we don't have to do this again.
  // If we would leave this enabled it will prevent systems from
  // suspending correctly.
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
  // Don't disable compositing
  // Available since SDL 2.0.8
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif

#ifdef SDL_HINT_TOUCH_MOUSE_EVENTS
  // Don't emulate mouse events from touch, we're handling touch ourselves.
  // Available since SDL 2.0.10
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif

  auto sdl_init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS;
  if (config_.rootless)
    sdl_init_flags = SDL_INIT_AUDIO | SDL_INIT_EVENTS;
  if (SDL_Init(sdl_init_flags) < 0) {
    const auto message = utils::string_format("Failed to initialize SDL: %s", SDL_GetError());
    BOOST_THROW_EXCEPTION(std::runtime_error(message));
  }
  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

  auto display_frame = graphics::Rect::Invalid;
  if (config_.display_frame == graphics::Rect::Invalid) {
    // We would need to init video to fetch display info
    if (config_.rootless) SDL_VideoInit(NULL);
    for (auto n = 0; n < SDL_GetNumVideoDisplays(); n++) {
      SDL_Rect r;
      if (SDL_GetDisplayBounds(n, &r) != 0) continue;

      graphics::Rect frame{r.x, r.y, r.x + r.w, r.y + r.h};

      if (display_frame == graphics::Rect::Invalid)
        display_frame = frame;
      else
        display_frame.merge(frame);
    }
    if (config_.rootless) SDL_VideoQuit();

    if (display_frame == graphics::Rect::Invalid)
      BOOST_THROW_EXCEPTION(
          std::runtime_error("No valid display configuration found"));
  } else {
    display_frame = config_.display_frame;
    // A fixed Android framebuffer does not imply a fixed host window. The
    // coordinate transform keeps those spaces independent in single-window
    // mode.
    window_size_immutable_ = !config_.single_window;
  }

  graphics::emugl::DisplayInfo::get()->set_resolution(display_frame.width(), display_frame.height());
  display_frame_ = display_frame;

  pointer_ = input_manager->create_device();
  pointer_->set_name("anbox-virtual-pointer");
  pointer_->set_driver_version(1);
  pointer_->set_input_id({BUS_VIRTUAL, 2, 2, 2});
  pointer_->set_physical_location("anbox/virtual/pointer");
  pointer_->set_key_bit(BTN_MOUSE);
  pointer_->set_key_bit(BTN_RIGHT);
  pointer_->set_key_bit(BTN_MIDDLE);
  pointer_->set_key_bit(BTN_SIDE);
  pointer_->set_key_bit(BTN_EXTRA);
  // NOTE: We don't use REL_X/REL_Y in reality but have to specify them here
  // to allow InputFlinger to detect we're a cursor device.
  pointer_->set_rel_bit(REL_X);
  pointer_->set_rel_bit(REL_Y);
  pointer_->set_rel_bit(REL_HWHEEL);
  pointer_->set_rel_bit(REL_WHEEL);
  // The single-window path forwards SDL coordinates as ABS_X/ABS_Y.  Declare
  // the same coordinate range to InputReader; without it Android applies a
  // fallback scale and clicks drift, most noticeably in portrait windows.
  pointer_->set_abs_bit(ABS_X);
  pointer_->set_abs_max(ABS_X, config_.legacy_input
                                   ? display_frame.width() - 1
                                   : pointer_axis_max);
  pointer_->set_abs_bit(ABS_Y);
  pointer_->set_abs_max(ABS_Y, config_.legacy_input
                                   ? display_frame.height() - 1
                                   : pointer_axis_max);
  pointer_->set_prop_bit(INPUT_PROP_POINTER);

  keyboard_ = input_manager->create_device();
  keyboard_->set_name("anbox-virtual-keyboard");
  keyboard_->set_driver_version(1);
  keyboard_->set_input_id({BUS_VIRTUAL, 3, 3, 3});
  keyboard_->set_physical_location("anbox/virtual/keyboard");
  keyboard_->set_key_bit(BTN_MISC);
  // uinput enforces the advertised capability bitmap. The legacy socket
  // bridge did not, which hid the fact that only KEY_OK was declared and made
  // normal host keyboard events disappear when using real event devices.
  for (int code = 1; code <= KEY_MAX; ++code)
    keyboard_->set_key_bit(code);

  touch_ = input_manager->create_device();
  touch_->set_name("anbox-virtual-touch");
  touch_->set_driver_version(1);
  touch_->set_input_id({BUS_VIRTUAL, 4, 4, 4});
  touch_->set_physical_location("anbox/virtual/touch");
  touch_->set_abs_bit(ABS_MT_SLOT);
  touch_->set_abs_max(ABS_MT_SLOT, 10);
  touch_->set_abs_bit(ABS_MT_TOUCH_MAJOR);
  touch_->set_abs_max(ABS_MT_TOUCH_MAJOR, 127);
  touch_->set_abs_bit(ABS_MT_TOUCH_MINOR);
  touch_->set_abs_max(ABS_MT_TOUCH_MINOR, 127);
  touch_->set_abs_bit(ABS_MT_POSITION_X);
  touch_->set_abs_max(ABS_MT_POSITION_X, display_frame.width());
  touch_->set_abs_bit(ABS_MT_POSITION_Y);
  touch_->set_abs_max(ABS_MT_POSITION_Y, display_frame.height());
  touch_->set_abs_bit(ABS_MT_TRACKING_ID);
  touch_->set_abs_max(ABS_MT_TRACKING_ID, MAX_TRACKING_ID);
  touch_->set_prop_bit(INPUT_PROP_DIRECT);

  for (int i = 0; i < MAX_FINGERS; i++)
      touch_slots[i] = -1;

  if (!config_.legacy_input) {
    input_dispatcher_ = std::make_shared<input::Dispatcher>(keyboard_, pointer_);
    reboxed_keyboard_ = std::make_shared<input::Keyboard>(
        input_dispatcher_, config_.forward_host_key_repeat
            ? input::Keyboard::RepeatPolicy::ForwardLinuxRepeat
            : input::Keyboard::RepeatPolicy::IgnoreHost);
    reboxed_pointer_ = std::make_shared<input::Pointer>(input_dispatcher_);
    input_dispatcher_->set_transport_failure_handler(
        [this] { reset_reboxed_input_after_transport_failure(); });
    INFO("Input backend=reboxed queue_capacity=384 release_reserve=256 key_repeat=%s transport=uinput",
         config_.forward_host_key_repeat ? "forward" : "ignore");
  } else {
    INFO("Input backend=legacy transport=socket+uinput");
  }

  event_thread_running_ = true;
  event_thread_ = std::thread(&Platform::process_events, this);
}

Platform::~Platform() {
  event_thread_running_ = false;
  if (event_thread_.joinable())
    event_thread_.join();
  if (!config_.legacy_input) {
    release_reboxed_input("backend shutdown");
    if (input_dispatcher_ &&
        !input_dispatcher_->flush(std::chrono::milliseconds(250)))
      WARNING("Timed out flushing forced input releases during shutdown");
    input_dispatcher_->set_transport_failure_handler({});
    reboxed_keyboard_.reset();
    reboxed_pointer_.reset();
    input_dispatcher_.reset();
  }
}

void Platform::set_renderer(const std::shared_ptr<Renderer> &renderer) {
  renderer_ = renderer;
}

void Platform::set_window_manager(const std::shared_ptr<wm::Manager> &window_manager) {
  window_manager_ = window_manager;
}

void Platform::process_events() {
  while (event_thread_running_) {
    SDL_Event event;
    while (SDL_WaitEventTimeout(&event, 100)) {
      switch (event.type) {
        case SDL_QUIT:
          release_reboxed_input("SDL quit");
          video_has_been_closed_ = true;
          DEBUG("SDL_QUIT");
          break;
        case SDL_WINDOWEVENT:
          for (auto &iter : windows_) {
            if (auto w = iter.second.lock()) {
              if (w->window_id() == event.window.windowID) {
                w->process_event(event);
                break;
              }
            }
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
          if (handle_pointer_lock_shortcut(event))
            break;
          if (keyboard_)
            config_.legacy_input ? process_input_event(event)
                                 : process_reboxed_input_event(event);
          break;
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
          {
            const auto window_id = event.type == SDL_MOUSEMOTION
                                       ? event.motion.windowID
                                       : event.button.windowID;
            const auto window = window_for_sdl_id(window_id);
            if (window && window->process_window_control_event(event))
              break;
          }
          [[fallthrough]];
        case SDL_MOUSEWHEEL:
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION:
          if (config_.legacy_input || event.type == SDL_FINGERDOWN ||
              event.type == SDL_FINGERUP || event.type == SDL_FINGERMOTION)
            process_input_event(event);
          else
            process_reboxed_input_event(event);
          break;
        case SDL_DROPFILE:
          forward_dropped_file(event.drop.file);
          SDL_free(event.drop.file);
          break;
        default:
          break;
      }
    }
  }
}

bool Platform::handle_pointer_lock_shortcut(const SDL_Event &event) {
  if (event.type != SDL_KEYDOWN || event.key.repeat != 0 ||
      event.key.keysym.scancode != SDL_SCANCODE_R)
    return false;

  const auto modifiers = static_cast<SDL_Keymod>(event.key.keysym.mod);
  const auto required = static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_ALT);
  if ((modifiers & required) != required)
    return false;

  set_pointer_locked(!pointer_locked_);
  return true;
}

void Platform::set_pointer_locked(bool locked) {
  auto *window = SDL_GetWindowFromID(focused_sdl_window_id_);
  if (!window) {
    WARNING("Cannot change pointer lock without a focused Anbox Reboxed window");
    return;
  }

  // The revised pointer state machine emits REL_X/REL_Y while locked and
  // absolute coordinates otherwise. Legacy keeps its historical absolute
  // behavior for comparison.
  SDL_SetRelativeMouseMode(locked && !config_.legacy_input ? SDL_TRUE
                                                            : SDL_FALSE);
  SDL_SetWindowGrab(window, locked ? SDL_TRUE : SDL_FALSE);
  pointer_locked_ = SDL_GetWindowGrab(window) == SDL_TRUE;
  INFO("Pointer lock %s (Alt+Ctrl+R to toggle)",
       pointer_locked_ ? "enabled" : "disabled");
}

std::uint16_t Platform::convert_mouse_button(std::uint8_t button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return BTN_LEFT;
    case SDL_BUTTON_RIGHT: return BTN_RIGHT;
    case SDL_BUTTON_MIDDLE: return BTN_MIDDLE;
    case SDL_BUTTON_X1: return BTN_SIDE;
    case SDL_BUTTON_X2: return BTN_EXTRA;
    default: return 0;
  }
}

std::uint64_t Platform::event_time_ns(const SDL_Event &event) {
  const auto now = input::Dispatcher::monotonic_time_ns();
  if (event.common.timestamp == 0)
    return now;
  const std::uint32_t age_ms =
      static_cast<std::uint32_t>(SDL_GetTicks() - event.common.timestamp);
  const std::uint64_t age_ns = static_cast<std::uint64_t>(age_ms) * 1000000ULL;
  return age_ns < now ? now - age_ns : now;
}

std::shared_ptr<Window> Platform::window_for_sdl_id(
    std::uint32_t window_id) const {
  for (const auto &entry : windows_) {
    if (auto window = entry.second.lock()) {
      if (window->window_id() == window_id)
        return window;
    }
  }
  return nullptr;
}

void Platform::diagnose_rejected_pointer(
    const char *reason, std::uint32_t window_id, int x, int y,
    const graphics::RenderedViewport *viewport) {
  const auto rejected = ++rejected_pointer_events_;
  if (!viewport || !viewport->valid)
    ++invalid_viewport_events_;
  if (rejected <= 5 || rejected % 100 == 0) {
    WARNING("Pointer rejected: reason=%s window_id=%u point=%d,%d rejected=%llu invalid_viewport=%llu generation=%llu window=%dx%d drawable=%dx%d viewport=%d,%d %dx%d",
            reason, window_id, x, y,
            static_cast<unsigned long long>(rejected),
            static_cast<unsigned long long>(invalid_viewport_events_.load()),
            static_cast<unsigned long long>(viewport ? viewport->generation : 0),
            viewport ? viewport->window_width : 0,
            viewport ? viewport->window_height : 0,
            viewport ? viewport->drawable_width : 0,
            viewport ? viewport->drawable_height : 0,
            viewport ? viewport->viewport.left() : 0,
            viewport ? viewport->viewport.top() : 0,
            viewport ? viewport->viewport.width() : 0,
            viewport ? viewport->viewport.height() : 0);
  }
}

bool Platform::map_pointer_android(std::uint32_t window_id, int host_x,
                                   int host_y, int &android_x,
                                   int &android_y) {
  const auto window = window_for_sdl_id(window_id);
  if (!window || !renderer_) {
    diagnose_rejected_pointer("window or renderer unavailable", window_id,
                              host_x, host_y, nullptr);
    return false;
  }
  graphics::RenderedViewport viewport;
  if (!renderer_->renderedViewport(window->native_handle(), &viewport)) {
    diagnose_rejected_pointer("resize transition has no current viewport",
                              window_id, host_x, host_y, &viewport);
    return false;
  }
  if (!viewport.map_absolute(host_x, host_y, android_x, android_y)) {
    diagnose_rejected_pointer("outside rendered Android viewport", window_id,
                              host_x, host_y, &viewport);
    return false;
  }

  const auto display_info = graphics::emugl::DisplayInfo::get();
  if (!config_.single_window) {
    android_x += window->frame().left();
    android_y += window->frame().top();
  }
  return true;
}

bool Platform::map_pointer_absolute(std::uint32_t window_id, int host_x,
                                    int host_y, int &android_x,
                                    int &android_y) {
  if (!map_pointer_android(window_id, host_x, host_y, android_x, android_y))
    return false;
  const auto display_info = graphics::emugl::DisplayInfo::get();
  // A stable normalized uinput axis lets Android InputReader rescale the
  // pointer when the logical display resolution changes without recreating
  // the host device or replaying old state.
  const int display_width = std::max(1, static_cast<int>(display_info->width()));
  const int display_height = std::max(1, static_cast<int>(display_info->height()));
  android_x = display_width <= 1 ? 0 : static_cast<int>(
      static_cast<long long>(android_x) * pointer_axis_max /
      (display_width - 1));
  android_y = display_height <= 1 ? 0 : static_cast<int>(
      static_cast<long long>(android_y) * pointer_axis_max /
      (display_height - 1));
  android_x = std::clamp(android_x, 0, pointer_axis_max);
  android_y = std::clamp(android_y, 0, pointer_axis_max);
  return true;
}

bool Platform::map_pointer_relative(std::uint32_t window_id, int host_dx,
                                    int host_dy, int &android_dx,
                                    int &android_dy) {
  const auto window = window_for_sdl_id(window_id);
  if (!window || !renderer_) {
    diagnose_rejected_pointer("relative window or renderer unavailable",
                              window_id, host_dx, host_dy, nullptr);
    return false;
  }
  graphics::RenderedViewport viewport;
  if (!renderer_->renderedViewport(window->native_handle(), &viewport)) {
    diagnose_rejected_pointer("relative resize transition has no viewport",
                              window_id, host_dx, host_dy, &viewport);
    return false;
  }
  return viewport.map_relative(host_dx, host_dy, android_dx, android_dy);
}

void Platform::process_reboxed_input_event(const SDL_Event &event) {
  const auto timestamp = event_time_ns(event);
  std::uint32_t event_window_id = 0;
  switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: event_window_id = event.key.windowID; break;
    case SDL_MOUSEMOTION: event_window_id = event.motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: event_window_id = event.button.windowID; break;
    case SDL_MOUSEWHEEL: event_window_id = event.wheel.windowID; break;
    default: break;
  }
  if (event_window_id == 0 || event_window_id != focused_sdl_window_id_)
    return;
  switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      const auto code = KeycodeConverter::convert(event.key.keysym.scancode);
      reboxed_keyboard_->key(code, event.type == SDL_KEYDOWN,
                             event.key.repeat != 0, timestamp);
      break;
    }
    case SDL_MOUSEMOTION: {
      if (!config_.no_touch_emulation) {
        process_input_event(event);
        break;
      }
      int x = 0;
      int y = 0;
      if (pointer_locked_) {
        if (map_pointer_relative(event.motion.windowID, event.motion.xrel,
                                 event.motion.yrel, x, y))
          reboxed_pointer_->relative(x, y, timestamp);
      } else if (map_pointer_absolute(event.motion.windowID, event.motion.x,
                                      event.motion.y, x, y)) {
        reboxed_pointer_->absolute(x, y, timestamp);
      }
      break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
      if (!config_.no_touch_emulation) {
        process_input_event(event);
        break;
      }
      const auto code = convert_mouse_button(event.button.button);
      if (code == 0)
        break;
      const bool pressed = event.type == SDL_MOUSEBUTTONDOWN;
      int x = 0;
      int y = 0;
      if (pressed && !pointer_locked_) {
        if (!map_pointer_absolute(event.button.windowID, event.button.x,
                                  event.button.y, x, y))
          break;
        reboxed_pointer_->absolute(x, y, timestamp);
      }
      reboxed_pointer_->button(code, pressed, timestamp);
      break;
    }
    case SDL_MOUSEWHEEL: {
      if (!pointer_locked_) {
        int host_x = 0;
        int host_y = 0;
        SDL_GetMouseState(&host_x, &host_y);
        int mapped_x = 0;
        int mapped_y = 0;
        if (!map_pointer_absolute(event.wheel.windowID, host_x, host_y,
                                  mapped_x, mapped_y))
          break;
        reboxed_pointer_->absolute(mapped_x, mapped_y, timestamp);
      } else {
        const auto window = window_for_sdl_id(event.wheel.windowID);
        graphics::RenderedViewport viewport;
        if (!window || !renderer_ ||
            !renderer_->renderedViewport(window->native_handle(), &viewport)) {
          diagnose_rejected_pointer("wheel during invalid resize transition",
                                    event.wheel.windowID, 0, 0, &viewport);
          break;
        }
      }
      int vertical = event.wheel.y;
      int horizontal = event.wheel.x;
      if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        vertical = -vertical;
        horizontal = -horizontal;
      }
      reboxed_pointer_->wheel(vertical, horizontal, timestamp);
      break;
    }
    default: break;
  }
}

void Platform::release_reboxed_input(const std::string &reason) {
  if (config_.legacy_input)
    return;
  const auto timestamp = input::Dispatcher::monotonic_time_ns();
  if (reboxed_keyboard_)
    reboxed_keyboard_->release_all(reason, timestamp);
  if (reboxed_pointer_)
    reboxed_pointer_->release_all(reason, timestamp);

  // Preserve the existing single-touch emulation feature without folding it
  // into the rewritten mouse state machine.
  if (!config_.no_touch_emulation && touch_ &&
      find_touch_slot(emulated_touch_id_) >= 0) {
    std::vector<input::Event> releases;
    push_finger_up(emulated_touch_id_, releases);
    if (!releases.empty())
      touch_->send_events(releases);
  }
}

void Platform::reset_reboxed_input_after_transport_failure() {
  ERROR("Input transport failed; clearing held state and pending input to prevent stale replay");
  if (reboxed_keyboard_)
    reboxed_keyboard_->reset_after_transport_failure();
  if (reboxed_pointer_)
    reboxed_pointer_->reset_after_transport_failure();
}

void Platform::process_input_event(const SDL_Event &event) {
  std::vector<input::Event> mouse_events;
  std::vector<input::Event> keyboard_events;
  std::vector<input::Event> touch_events;

  std::int32_t x = 0;
  std::int32_t y = 0;

  switch (event.type) {
    // Mouse
    case SDL_MOUSEBUTTONDOWN:
      x = event.button.x;
      y = event.button.y;
      if (!map_pointer_android(event.button.windowID, x, y, x, y))
        break;
      if (config_.no_touch_emulation) {
        mouse_events.push_back({EV_ABS, ABS_X, x});
        mouse_events.push_back({EV_ABS, ABS_Y, y});
        mouse_events.push_back({EV_KEY, BTN_LEFT, 1});
      } else {
        push_finger_down(x, y, emulated_touch_id_, touch_events);
      }
      break;
    case SDL_MOUSEBUTTONUP:
      if (config_.no_touch_emulation) {
        mouse_events.push_back({EV_KEY, BTN_LEFT, 0});
      } else {
        push_finger_up(emulated_touch_id_, touch_events);
      }
      break;
    case SDL_MOUSEMOTION:
      x = event.motion.x;
      y = event.motion.y;
      if (!map_pointer_android(event.motion.windowID, x, y, x, y))
        break;

      if (config_.no_touch_emulation) {
        // Android 15's InputReader uses these absolute coordinates for the
        // virtual cursor.  Do not also send REL_X/REL_Y: that makes it apply
        // a second, accumulated movement and produces direction-dependent
        // drift (especially vertical drift after a long drag).
        mouse_events.push_back({EV_ABS, ABS_X, x});
        mouse_events.push_back({EV_ABS, ABS_Y, y});
      } else {
        push_finger_motion(x, y, emulated_touch_id_, touch_events);
      }
      break;
    case SDL_MOUSEWHEEL:
      if (!config_.no_touch_emulation) {
        SDL_GetMouseState(&x, &y);
        if (!map_pointer_android(event.wheel.windowID, x, y, x, y))
          break;

        mouse_events.push_back({EV_ABS, ABS_X, x});
        mouse_events.push_back({EV_ABS, ABS_Y, y});
      }
      mouse_events.push_back(
          {EV_REL, REL_WHEEL, static_cast<std::int32_t>(event.wheel.y)});
      break;
    // Keyboard
    case SDL_KEYDOWN: {
      const auto code = KeycodeConverter::convert(event.key.keysym.scancode);
      if (code == KEY_RESERVED) break;
      keyboard_events.push_back({EV_KEY, code, 1});
      break;
    }
    case SDL_KEYUP: {
      const auto code = KeycodeConverter::convert(event.key.keysym.scancode);
      if (code == KEY_RESERVED) break;
      keyboard_events.push_back({EV_KEY, code, 0});
      break;
    }
    // Touch screen
    case SDL_FINGERDOWN: {
      if (!calculate_touch_coordinates(event, x, y))
        break;
      push_finger_down(x, y, event.tfinger.fingerId, touch_events);

      break;
    }
    case SDL_FINGERUP: {
      push_finger_up(event.tfinger.fingerId, touch_events);
      break;
    }
	case SDL_FINGERMOTION: {

      if (!calculate_touch_coordinates(event, x, y))
        break;
      push_finger_motion(x, y, event.tfinger.fingerId, touch_events);
      break;
    }
    default:
      break;
  }

  if (mouse_events.size() > 0) {
    mouse_events.push_back({EV_SYN, SYN_REPORT, 0});      
    pointer_->send_events(mouse_events);
  }

  if (keyboard_events.size() > 0)
    keyboard_->send_events(keyboard_events);

  if (touch_events.size() > 0)
    touch_->send_events(touch_events);
}

int Platform::find_touch_slot(int id){
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (touch_slots[i] == id)
          return i;
    }
    return -1;
}

void Platform::push_slot(std::vector<input::Event> &touch_events, int slot){
    if (last_slot != slot) {
        touch_events.push_back({EV_ABS, ABS_MT_SLOT, slot});
        last_slot = slot;
    }
}

void Platform::push_finger_down(int x, int y, int finger_id, std::vector<input::Event> &touch_events){
    int slot = find_touch_slot(-1);
    if (slot == -1) {
        DEBUG("no free slot!");
        return;
    }
    touch_slots[slot] = finger_id;
    push_slot(touch_events, slot);
    touch_events.push_back({EV_ABS, ABS_MT_TRACKING_ID, static_cast<std::int32_t>(finger_id % MAX_TRACKING_ID + 1)});
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_X, x});
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_Y, y});
    touch_events.push_back({EV_SYN, SYN_REPORT, 0});
}

void Platform::push_finger_up(int finger_id, std::vector<input::Event> &touch_events){
    int slot = find_touch_slot(finger_id);
    if (slot == -1) 
      return;
    push_slot(touch_events, slot);
    touch_events.push_back({EV_ABS, ABS_MT_TRACKING_ID, -1});
    touch_events.push_back({EV_SYN, SYN_REPORT, 0});
    touch_slots[slot] = -1;
}

void Platform::push_finger_motion(int x, int y, int finger_id, std::vector<input::Event> &touch_events){
    int slot = find_touch_slot(finger_id);
    if (slot == -1) 
      return;
    push_slot(touch_events, slot);
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_X, x});
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_Y, y});
    touch_events.push_back({EV_SYN, SYN_REPORT, 0});
}


bool Platform::adjust_coordinates(std::int32_t &x, std::int32_t &y) {
  SDL_Window *window = nullptr;

  if (!config_.single_window) {
    window = SDL_GetWindowFromID(focused_sdl_window_id_);
    return adjust_coordinates(window, x, y);
  } else {
    // When running the whole Android system in a single window we don't
    // need to reacalculate and the pointer position as they are already
    // relative to our window.
    return true;
  }
}

bool Platform::adjust_coordinates(SDL_Window *window, std::int32_t &x, std::int32_t &y) {
  std::int32_t rel_x = 0;
  std::int32_t rel_y = 0;

  if (!window) {
    return false;
  }
  // As we get only absolute coordindates relative to our window we have to
  // calculate the correct position based on the current focused window
  SDL_GetWindowPosition(window, &rel_x, &rel_y);
  x += rel_x;
  y += rel_y;
  return true;
}

bool Platform::calculate_touch_coordinates(const SDL_Event &event,
                                           std::int32_t &x,
                                           std::int32_t &y) {
  SDL_Window *window = nullptr;

  window = SDL_GetWindowFromID(focused_sdl_window_id_);
  // before SDL 2.0.7 on X11 tfinger coordinates are not normalized
  if (!SDL_VERSION_ATLEAST(2,0,7) && (event.tfinger.x > 1 || event.tfinger.y > 1)) {
    x = event.tfinger.x;
    y = event.tfinger.y;
  } else {
    if (window) {
      // SDL finger coordinates are normalized to the physical touch display,
      // not to an individual X11 window.  Treating them as window-relative
      // confines usable input to whichever desktop region overlaps the
      // portrait window.  Convert display-normalized coordinates to desktop
      // pixels first, then subtract the SDL window origin.
      SDL_Rect display_bounds{};
      const auto display_index = SDL_GetWindowDisplayIndex(window);
      std::int32_t window_x = 0;
      std::int32_t window_y = 0;
      SDL_GetWindowPosition(window, &window_x, &window_y);
      if (display_index >= 0 && SDL_GetDisplayBounds(display_index, &display_bounds) == 0) {
        x = static_cast<std::int32_t>(event.tfinger.x * display_bounds.w) +
            display_bounds.x - window_x;
        y = static_cast<std::int32_t>(event.tfinger.y * display_bounds.h) +
            display_bounds.y - window_y;
      } else {
        // Conservative fallback for SDL backends that do not expose a
        // display index: retain the historical window-relative calculation.
        SDL_GetWindowSize(window, &x, &y);
        x *= event.tfinger.x;
        y *= event.tfinger.y;
      }
    } else {
      x = display_frame_.width() * event.tfinger.x;
      y = display_frame_.height() * event.tfinger.y;
    }
  }

  if (config_.single_window) {
    // When running the whole Android system in a single window we don't
    // need to reacalculate and the pointer position as they are already
    // relative to our window.
    return true;
  } else {
    return adjust_coordinates(window, x, y);
  }
}

Window::Id Platform::next_window_id() {
  static Window::Id next_id = 0;
  return next_id++;
}

std::shared_ptr<wm::Window> Platform::create_window(
    const anbox::wm::Task::Id &task, const anbox::graphics::Rect &frame, const std::string &title) {
  if (!renderer_) {
    ERROR("Can't create window without a renderer set");
    return nullptr;
  }

  // Force video init again after sdl has closed
  if (config_.rootless && video_has_been_closed_) {
    DEBUG("forcing video init");
    SDL_VideoInit(NULL);
    video_has_been_closed_ = false;
  }

  auto initial_frame = frame;
  if (config_.single_window) {
    const auto display = initial_display_work_area(display_frame_);
    initial_frame = calculate_single_window_frame(frame, display.area);
    if (initial_frame == graphics::Rect::Invalid) {
      ERROR("Could not determine a valid work area for the single host window");
      return nullptr;
    }
    INFO("Single-window geometry: mode=%s display=%d work_area=%d,%d %dx%d source=%s client=%dx%d position=%d,%d",
         frame == graphics::Rect::Invalid ? "automatic-20:9" : "explicit",
         display.display_index, display.area.left(), display.area.top(),
         display.area.width(), display.area.height(),
         display.usable_bounds ? "usable" : "monitor-fallback",
         initial_frame.width(), initial_frame.height(), initial_frame.left(),
         initial_frame.top());
  }

  auto id = next_window_id();
  const bool resizable = !config_.single_window && !window_size_immutable_;
  auto w = std::make_shared<Window>(renderer_, id, task, shared_from_this(),
                                   initial_frame, title, resizable,
                                   !config_.server_side_decoration);
  focused_sdl_window_id_ = w->window_id();
  windows_.insert({id, w});
  int window_width = 0;
  int window_height = 0;
  w->window_size(window_width, window_height);
  renderer_->updateHostWindow(w->native_handle(), window_width, window_height,
                              "created");
  return w;
}

void Platform::window_deleted(const Window::Id &id) {
  auto w = windows_.find(id);
  if (w == windows_.end()) {
    WARNING("Got window removed event for unknown window (id %d)", id);
    return;
  }
  if (auto window = w->second.lock()) {
    release_reboxed_input("window close");
    if (window->window_id() == focused_sdl_window_id_)
      set_pointer_locked(false);
    window_manager_->remove_task(window->task());
  }
  windows_.erase(w);

  // In single-window mode this is the whole Android desktop, not merely an
  // app task.  Leaving the session manager alive after its sole window closes
  // makes the next launcher invocation find a stale process with nothing to
  // show.  Route the close through the existing signal trap so it stops the
  // container and renderer in the normal order.
  if (config_.single_window)
    ::raise(SIGTERM);
}

void Platform::window_wants_focus(const Window::Id &id) {
  auto w = windows_.find(id);
  if (w == windows_.end()) return;

  if (auto window = w->second.lock()) {
    focused_sdl_window_id_ = window->window_id();
    window_manager_->set_focused_task(window->task());
  }
}

void Platform::window_lost_focus(const Window::Id &id) {
  auto window = windows_.find(id);
  if (window == windows_.end())
    return;
  if (auto locked = window->second.lock()) {
    if (locked->window_id() == focused_sdl_window_id_) {
      release_reboxed_input("focus loss");
      set_pointer_locked(false);
      focused_sdl_window_id_ = 0;
    }
  }
}

void Platform::window_hidden(const Window::Id &id) {
  invalidate_window_viewport(id, "hidden or minimized");
  release_reboxed_input("window hidden or minimized");
  if (pointer_locked_)
    set_pointer_locked(false);
  const auto entry = windows_.find(id);
  if (entry != windows_.end()) {
    if (auto window = entry->second.lock()) {
      if (window->window_id() == focused_sdl_window_id_)
        focused_sdl_window_id_ = 0;
    }
  }
}

void Platform::window_moved(const Window::Id &id, const std::int32_t &x,
                                  const std::int32_t &y) {
  auto w = windows_.find(id);
  if (w == windows_.end()) return;

  if (auto window = w->second.lock()) {
    // A move can cross monitors with a different scale. SDL logical size may
    // stay constant while the EGL drawable changes.
    int width = 0;
    int height = 0;
    window->window_size(width, height);
    renderer_->updateHostWindow(window->native_handle(), width, height,
                                "moved or monitor scale changed");
    // The single Android display always uses framebuffer-local coordinates.
    // Feeding its desktop position back into SurfaceFlinger shifts the
    // viewport whenever the host window is moved and clips launcher icons and
    // the navigation bar at the right/bottom edges.
    if (config_.single_window)
      return;
    auto new_frame = window->frame();
    new_frame.translate(x, y);
    window->update_frame(new_frame);
    window_manager_->resize_task(window->task(), new_frame, 3);
  }
}

void Platform::window_resized(const Window::Id &id,
                                    const std::int32_t &width,
                                    const std::int32_t &height) {
  auto w = windows_.find(id);
  if (w == windows_.end()) return;

  if (auto window = w->second.lock()) {
    renderer_->updateHostWindow(
        window->native_handle(), width, height,
        window->fullscreen() ? "fullscreen size changed"
                             : "compositor size changed");
    auto new_frame = window->frame();
    if (config_.single_window)
      new_frame.translate(0, 0);
    new_frame.resize(width, height);
    // We need to update the window frame in advance here as otherwise we may
    // get a movement event before we got an update of the actual layer
    // representing this window and then we're back to the original size of
    // the task.
    window->update_frame(new_frame);
    window_manager_->resize_task(window->task(), new_frame, 3);
  }
}

void Platform::invalidate_window_viewport(const Window::Id &id,
                                          const char *reason) {
  const auto entry = windows_.find(id);
  if (entry == windows_.end() || !renderer_)
    return;
  if (auto window = entry->second.lock()) {
    int width = 0;
    int height = 0;
    window->window_size(width, height);
    renderer_->updateHostWindow(window->native_handle(), width, height,
                                reason);
  }
}

void Platform::window_lifecycle_changed(const Window::Id &id,
                                        const char *reason) {
  invalidate_window_viewport(id, reason);
}

void Platform::set_clipboard_data(const ClipboardData &data) {
  if (data.text.empty())
    return;
  SDL_SetClipboardText(data.text.c_str());
}

Platform::ClipboardData Platform::get_clipboard_data() {
  if (!SDL_HasClipboardText())
    return ClipboardData{};

  auto text = SDL_GetClipboardText();
  if (!text)
    return ClipboardData{};

  auto data = ClipboardData{text};
  SDL_free(text);
  return data;
}

std::shared_ptr<audio::Sink> Platform::create_audio_sink() {
  return std::make_shared<AudioSink>();
}

std::shared_ptr<audio::Source> Platform::create_audio_source() {
  ERROR("Not implemented");
  return nullptr;
}

bool Platform::supports_multi_window() const {
  return true;
}
}
