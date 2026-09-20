#ifndef ANBOX_INPUT_POINTER_H_
#define ANBOX_INPUT_POINTER_H_

#include "anbox/input/event_sink.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace anbox::input {
class Pointer {
 public:
  struct Diagnostics {
    std::uint64_t motions = 0;
    std::uint64_t buttons = 0;
    std::uint64_t wheels = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t forced_releases = 0;
  };

  explicit Pointer(std::shared_ptr<EventSink> sink);
  ~Pointer();
  bool absolute(int x, int y, std::uint64_t source_time_ns);
  bool relative(int dx, int dy, std::uint64_t source_time_ns);
  bool button(std::uint16_t linux_code, bool pressed,
              std::uint64_t source_time_ns);
  bool wheel(int vertical, int horizontal, std::uint64_t source_time_ns);
  void release_all(const std::string &reason, std::uint64_t source_time_ns);
  void reset_after_transport_failure();
  Diagnostics diagnostics() const;

 private:
  std::shared_ptr<EventSink> sink_;
  mutable std::mutex mutex_;
  std::set<std::uint16_t> host_held_;
  std::set<std::uint16_t> injected_held_;
  std::uint64_t next_source_sequence_ = 0;
  Diagnostics diagnostics_;
};
}  // namespace anbox::input
#endif
