#include "anbox/graphics/coordinate_transform.h"
#include "anbox/graphics/rendered_viewport.h"
#include "anbox/input/dispatcher.h"
#include "anbox/input/keyboard.h"
#include "anbox/input/pointer.h"

#include <linux/input.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "input self-test failed: " << message << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

class RecordingSink : public anbox::input::EventSink {
 public:
  bool submit(anbox::input::EventBatch batch) override {
    batches.push_back(std::move(batch));
    return accepting;
  }
  bool accepting = true;
  std::vector<anbox::input::EventBatch> batches;
};

class RecordingDevice : public anbox::input::EventDevice {
 public:
  bool deliver_events(const std::vector<anbox::input::Event> &events,
                      std::uint64_t timestamp, bool legacy) override {
    (void)legacy;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::lock_guard<std::mutex> lock(mutex);
    delivered.push_back(events);
    timestamps.push_back(timestamp);
    return true;
  }
  std::mutex mutex;
  std::vector<std::vector<anbox::input::Event>> delivered;
  std::vector<std::uint64_t> timestamps;
};

void test_keyboard() {
  auto sink = std::make_shared<RecordingSink>();
  anbox::input::Keyboard keyboard(sink);
  const auto t = anbox::input::Dispatcher::monotonic_time_ns();
  require(keyboard.key(KEY_A, true, false, t), "A down rejected");
  require(keyboard.key(KEY_A, true, true, t + 1), "A repeat handling failed");
  require(keyboard.key(KEY_A, false, false, t + 2), "A up rejected");
  require(sink->batches.size() == 2, "one tap was not exactly two batches");
  require(sink->batches[0].events[0].value == 1, "tap missing key-down");
  require(sink->batches[1].events[0].value == 0, "tap missing key-up");
  require(sink->batches[0].events.back().type == EV_SYN &&
              sink->batches[1].events.back().type == EV_SYN,
          "keyboard batch missing SYN_REPORT");
  require(sink->batches[0].source_sequence <
              sink->batches[1].source_sequence,
          "keyboard source sequence did not increase");

  keyboard.key(KEY_B, true, false, t + 3);
  keyboard.key(KEY_C, true, false, t + 4);
  keyboard.release_all("self-test focus loss", t + 5);
  const auto &release = sink->batches.back();
  require(release.kind == anbox::input::BatchKind::ForcedRelease,
          "focus loss did not force a release batch");
  require(release.events.size() == 3, "focus release did not release both keys");
}

void test_pointer_and_mapping() {
  auto sink = std::make_shared<RecordingSink>();
  anbox::input::Pointer pointer(sink);
  const auto t = anbox::input::Dispatcher::monotonic_time_ns();
  pointer.button(BTN_LEFT, true, t);
  pointer.button(BTN_RIGHT, true, t + 1);
  pointer.release_all("self-test minimize", t + 2);
  require(sink->batches.back().events.size() == 3,
          "pointer forced release did not release both buttons");

  anbox::graphics::CoordinateTransform portrait(1024, 768, 720, 1600);
  const auto viewport = portrait.viewport();
  require(viewport.width() == 345 && viewport.height() == 768,
          "portrait viewport does not preserve aspect ratio");
  int x = -1;
  int y = -1;
  require(portrait.map_absolute(viewport.left(), viewport.top(), x, y) &&
              x == 0 && y == 0,
          "portrait top-left mapping failed");
  require(portrait.map_absolute(viewport.right() - 1, viewport.bottom() - 1,
                                x, y) &&
              x == 719 && y == 1599,
          "portrait bottom-right mapping failed");
  require(!portrait.map_absolute(viewport.left() - 1, viewport.top(), x, y),
          "letterbox input was not rejected");

  anbox::graphics::CoordinateTransform rotated(
      1600, 720, 720, 1600,
      anbox::graphics::CoordinateTransform::Rotation::R90);
  require(rotated.map_absolute(0, 0, x, y) && x == 0 && y == 1599,
          "90-degree mapping failed");

  // SDL reports logical window coordinates while EGL may render at a
  // different pixel scale. Input must consume the renderer's exact viewport,
  // not recompute one from the 800x600 logical window.
  anbox::graphics::RenderedViewport maximized;
  maximized.valid = true;
  maximized.window_width = 800;
  maximized.window_height = 600;
  maximized.drawable_width = 1600;
  maximized.drawable_height = 1200;
  maximized.viewport = {530, 0, 1070, 1200};
  maximized.android_width = 720;
  maximized.android_height = 1600;
  maximized.generation = 42;
  require(maximized.map_absolute(265, 0, x, y) && x == 0 && y == 0,
          "maximized HiDPI viewport offset was ignored");
  require(maximized.map_absolute(534, 599, x, y) && x == 719 && y == 1599,
          "mismatched logical/drawable bottom-right mapping failed");
  require(!maximized.map_absolute(200, 300, x, y),
          "maximized letterbox pointer was accepted");

  auto transition = maximized;
  transition.valid = false;
  transition.generation = 43;
  require(!transition.map_absolute(400, 300, x, y),
          "pointer was not suppressed during maximize/restore transition");

  anbox::graphics::RenderedViewport restored;
  restored.valid = true;
  restored.window_width = 720;
  restored.window_height = 1600;
  restored.drawable_width = 720;
  restored.drawable_height = 1600;
  restored.viewport = {0, 0, 720, 1600};
  restored.android_width = 720;
  restored.android_height = 1600;
  restored.generation = 44;
  require(restored.map_absolute(719, 1599, x, y) && x == 719 && y == 1599,
          "restored viewport did not replace maximized mapping");
}

void test_dispatcher() {
  auto keyboard = std::make_shared<RecordingDevice>();
  auto pointer = std::make_shared<RecordingDevice>();
  anbox::input::Dispatcher dispatcher(keyboard, pointer, 8, 2);
  const auto t = anbox::input::Dispatcher::monotonic_time_ns();
  for (int n = 0; n < 32; ++n) {
    dispatcher.submit({anbox::input::Stream::Pointer,
                       anbox::input::BatchKind::Motion,
                       {{EV_ABS, ABS_X, n}, {EV_SYN, SYN_REPORT, 0}},
                       static_cast<std::uint64_t>(n + 1), 0, t, 0});
  }
  dispatcher.submit({anbox::input::Stream::Keyboard,
                     anbox::input::BatchKind::Key,
                     {{EV_KEY, KEY_A, 1}, {EV_SYN, SYN_REPORT, 0}},
                     33, 0, t, 0});
  dispatcher.submit({anbox::input::Stream::Keyboard,
                     anbox::input::BatchKind::ForcedRelease,
                     {{EV_KEY, KEY_A, 0}, {EV_SYN, SYN_REPORT, 0}},
                     34, 0, t, 0});
  dispatcher.submit({anbox::input::Stream::Pointer,
                     anbox::input::BatchKind::Wheel,
                     {{EV_REL, REL_WHEEL, 1}, {EV_SYN, SYN_REPORT, 0}},
                     35, 0, t - 200000000ULL, 0});
  require(dispatcher.flush(std::chrono::seconds(2)),
          "dispatcher did not drain");
  const auto stats = dispatcher.diagnostics();
  require(stats.maximum_depth <= 8, "dispatcher exceeded bounded capacity");
  require(stats.coalesced > 0, "motion was not coalesced");
  require(stats.dropped > 0, "stale pointer state was replayed");
  require(stats.delivered >= 3, "ordered key boundaries were not delivered");
  require(stats.maximum_latency_us > 0, "latency was not measured");
  std::lock_guard<std::mutex> lock(keyboard->mutex);
  require(keyboard->delivered.size() == 2,
          "keyboard down/up ordering was not preserved");
  require(keyboard->delivered[0][0].value == 1 &&
              keyboard->delivered[1][0].value == 0,
          "keyboard delivery order is incorrect");
  std::cout << "input self-test: delivered=" << stats.delivered
            << " coalesced=" << stats.coalesced
            << " dropped=" << stats.dropped
            << " max_depth=" << stats.maximum_depth
            << " max_latency_us=" << stats.maximum_latency_us << std::endl;
}
}  // namespace

int main() {
  test_keyboard();
  test_pointer_and_mapping();
  test_dispatcher();
  return EXIT_SUCCESS;
}
