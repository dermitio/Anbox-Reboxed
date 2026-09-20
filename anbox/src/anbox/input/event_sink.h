#ifndef ANBOX_INPUT_EVENT_SINK_H_
#define ANBOX_INPUT_EVENT_SINK_H_

#include "anbox/input/device.h"

#include <cstdint>
#include <vector>

namespace anbox::input {
enum class Stream { Keyboard, Pointer };
enum class BatchKind { Key, Button, Motion, Wheel, Repeat, ForcedRelease };

struct EventBatch {
  Stream stream = Stream::Keyboard;
  BatchKind kind = BatchKind::Key;
  std::vector<Event> events;
  std::uint64_t source_sequence = 0;
  std::uint64_t transport_sequence = 0;
  std::uint64_t source_time_ns = 0;
  std::uint64_t enqueue_time_ns = 0;
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual bool submit(EventBatch batch) = 0;
};
}  // namespace anbox::input
#endif
