#ifndef ANBOX_INPUT_DISPATCHER_H_
#define ANBOX_INPUT_DISPATCHER_H_

#include "anbox/input/event_sink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace anbox::input {
class EventDevice;

class Dispatcher : public EventSink {
 public:
  struct Diagnostics {
    std::uint64_t submitted = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t transport_failures = 0;
    std::uint64_t maximum_depth = 0;
    std::uint64_t maximum_latency_us = 0;
  };

  Dispatcher(std::shared_ptr<EventDevice> keyboard,
             std::shared_ptr<EventDevice> pointer,
             std::size_t capacity = 384,
             std::size_t release_reserve = 256);
  ~Dispatcher() override;

  bool submit(EventBatch batch) override;
  bool flush(std::chrono::milliseconds timeout);
  Diagnostics diagnostics() const;
  void set_transport_failure_handler(std::function<void()> handler);

  static std::uint64_t monotonic_time_ns();

 private:
  bool is_release(const EventBatch &batch) const;
  bool make_room_locked(const EventBatch &incoming);
  void run();
  void log_diagnostics(const EventBatch &batch,
                       std::uint64_t queue_latency_us,
                       std::uint64_t delivery_latency_us,
                       std::size_t depth);

  std::shared_ptr<EventDevice> keyboard_;
  std::shared_ptr<EventDevice> pointer_;
  const std::size_t capacity_;
  const std::size_t release_reserve_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable drained_;
  std::deque<EventBatch> queue_;
  std::thread worker_;
  bool stopping_ = false;
  bool transport_failed_ = false;
  bool delivering_ = false;
  std::uint64_t next_sequence_ = 0;
  Diagnostics diagnostics_;
  std::function<void()> transport_failure_handler_;
};
}  // namespace anbox::input
#endif
