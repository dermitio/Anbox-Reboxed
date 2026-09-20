#ifndef ANBOX_INPUT_KEYBOARD_H_
#define ANBOX_INPUT_KEYBOARD_H_

#include "anbox/input/event_sink.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace anbox::input {
class Keyboard {
 public:
  enum class RepeatPolicy { IgnoreHost, ForwardLinuxRepeat };
  struct Diagnostics {
    std::uint64_t host_events = 0;
    std::uint64_t repeats = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t forced_releases = 0;
  };

  Keyboard(std::shared_ptr<EventSink> sink,
           RepeatPolicy repeat_policy = RepeatPolicy::IgnoreHost);
  ~Keyboard();

  bool key(std::uint16_t linux_code, bool pressed, bool host_repeat,
           std::uint64_t source_time_ns);
  void release_all(const std::string &reason, std::uint64_t source_time_ns);
  void reset_after_transport_failure();
  Diagnostics diagnostics() const;

 private:
  std::shared_ptr<EventSink> sink_;
  RepeatPolicy repeat_policy_;
  mutable std::mutex mutex_;
  std::set<std::uint16_t> host_held_;
  std::set<std::uint16_t> injected_held_;
  std::uint64_t next_source_sequence_ = 0;
  Diagnostics diagnostics_;
};
}  // namespace anbox::input
#endif
