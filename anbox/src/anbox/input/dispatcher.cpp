#include "anbox/input/dispatcher.h"

#include "anbox/input/device.h"
#include "anbox/logger.h"

#include <algorithm>

namespace anbox::input {
Dispatcher::Dispatcher(std::shared_ptr<EventDevice> keyboard,
                       std::shared_ptr<EventDevice> pointer,
                       std::size_t capacity,
                       std::size_t release_reserve)
    : keyboard_(std::move(keyboard)),
      pointer_(std::move(pointer)),
      capacity_(std::max<std::size_t>(1, capacity)),
      release_reserve_(std::min(release_reserve, capacity_)) {
  worker_ = std::thread(&Dispatcher::run, this);
}

Dispatcher::~Dispatcher() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  if (worker_.joinable())
    worker_.join();
  const auto stats = diagnostics();
  INFO("Input diagnostics: submitted=%llu delivered=%llu dropped=%llu coalesced=%llu transport_failures=%llu max_depth=%llu max_latency_us=%llu capacity=%zu",
       static_cast<unsigned long long>(stats.submitted),
       static_cast<unsigned long long>(stats.delivered),
       static_cast<unsigned long long>(stats.dropped),
       static_cast<unsigned long long>(stats.coalesced),
       static_cast<unsigned long long>(stats.transport_failures),
       static_cast<unsigned long long>(stats.maximum_depth),
       static_cast<unsigned long long>(stats.maximum_latency_us), capacity_);
}

std::uint64_t Dispatcher::monotonic_time_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Dispatcher::is_release(const EventBatch &batch) const {
  if (batch.kind == BatchKind::ForcedRelease)
    return true;
  return std::any_of(batch.events.begin(), batch.events.end(),
                     [](const Event &event) {
                       return event.type == EV_KEY && event.value == 0;
                     });
}

bool Dispatcher::make_room_locked(const EventBatch &incoming) {
  const bool release = is_release(incoming);
  const std::size_t normal_limit = capacity_ - release_reserve_;
  const std::size_t limit = release ? capacity_ : normal_limit;
  if (queue_.size() < limit)
    return true;

  auto droppable = std::find_if(queue_.begin(), queue_.end(),
                                [](const EventBatch &batch) {
    return batch.kind == BatchKind::Motion || batch.kind == BatchKind::Repeat;
  });
  if (droppable != queue_.end()) {
    queue_.erase(droppable);
    ++diagnostics_.dropped;
    return true;
  }
  return false;
}

bool Dispatcher::submit(EventBatch batch) {
  if (batch.events.empty())
    return true;
  if (batch.source_time_ns == 0)
    batch.source_time_ns = monotonic_time_ns();
  batch.enqueue_time_ns = monotonic_time_ns();

  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || transport_failed_)
    return false;
  batch.transport_sequence = ++next_sequence_;
  ++diagnostics_.submitted;

  // Motion is state, not history. Only replace a trailing motion batch so a
  // button/wheel/key boundary can never be crossed by coalescing.
  if (batch.kind == BatchKind::Motion && !queue_.empty() &&
      queue_.back().kind == BatchKind::Motion &&
      queue_.back().stream == batch.stream) {
    queue_.back() = std::move(batch);
    ++diagnostics_.coalesced;
    return true;
  }

  if (!make_room_locked(batch)) {
    ++diagnostics_.dropped;
    WARNING("Input queue full: dropped source_seq=%llu transport_seq=%llu kind=%d depth=%zu",
            static_cast<unsigned long long>(batch.source_sequence),
            static_cast<unsigned long long>(batch.transport_sequence),
            static_cast<int>(batch.kind), queue_.size());
    return false;
  }
  queue_.push_back(std::move(batch));
  diagnostics_.maximum_depth =
      std::max<std::uint64_t>(diagnostics_.maximum_depth, queue_.size());
  wake_.notify_one();
  return true;
}

void Dispatcher::run() {
  for (;;) {
    EventBatch batch;
    std::size_t depth = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_)
          return;
        continue;
      }
      batch = std::move(queue_.front());
      queue_.pop_front();
      delivering_ = true;
      depth = queue_.size();
    }

    const auto now = monotonic_time_ns();
    const std::uint64_t queue_latency_us =
        static_cast<std::uint64_t>((now - batch.source_time_ns) / 1000ULL);
    const bool stale_state = queue_latency_us > 100000ULL &&
        (batch.kind == BatchKind::Motion || batch.kind == BatchKind::Wheel ||
         batch.kind == BatchKind::Repeat);
    if (stale_state) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        delivering_ = false;
        ++diagnostics_.dropped;
        if (queue_.empty())
          drained_.notify_all();
      }
      WARNING("Dropped stale input state: source_seq=%llu transport_seq=%llu kind=%d latency_us=%llu",
              static_cast<unsigned long long>(batch.source_sequence),
              static_cast<unsigned long long>(batch.transport_sequence),
              static_cast<int>(batch.kind),
              static_cast<unsigned long long>(queue_latency_us));
      continue;
    }
    auto device = batch.stream == Stream::Keyboard ? keyboard_ : pointer_;
    const bool delivered = device && device->deliver_events(
        batch.events, batch.source_time_ns, false);
    const std::uint64_t delivery_latency_us = static_cast<std::uint64_t>(
        (monotonic_time_ns() - batch.source_time_ns) / 1000ULL);
    std::function<void()> failure_handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      delivering_ = false;
      if (delivered) {
        ++diagnostics_.delivered;
        diagnostics_.maximum_latency_us = std::max(
            diagnostics_.maximum_latency_us, delivery_latency_us);
      } else {
        ++diagnostics_.transport_failures;
        transport_failed_ = true;
        diagnostics_.dropped += queue_.size();
        queue_.clear();
        failure_handler = transport_failure_handler_;
      }
      if (queue_.empty())
        drained_.notify_all();
    }
    log_diagnostics(batch, queue_latency_us, delivery_latency_us, depth);
    if (failure_handler)
      failure_handler();
  }
}

void Dispatcher::log_diagnostics(const EventBatch &batch,
                                 std::uint64_t queue_latency_us,
                                 std::uint64_t delivery_latency_us,
                                 std::size_t depth) {
  if (batch.kind == BatchKind::ForcedRelease ||
      batch.kind == BatchKind::Repeat ||
      (batch.transport_sequence % 256ULL) == 1ULL) {
    INFO("Input boundary: source_seq=%llu transport_seq=%llu stream=%s kind=%d queue_latency_us=%llu delivery_latency_us=%llu queue_depth=%zu",
         static_cast<unsigned long long>(batch.source_sequence),
         static_cast<unsigned long long>(batch.transport_sequence),
         batch.stream == Stream::Keyboard ? "keyboard" : "pointer",
         static_cast<int>(batch.kind),
         static_cast<unsigned long long>(queue_latency_us),
         static_cast<unsigned long long>(delivery_latency_us), depth);
  }
}

bool Dispatcher::flush(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return drained_.wait_for(lock, timeout,
                           [this] { return queue_.empty() && !delivering_; });
}

Dispatcher::Diagnostics Dispatcher::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}

void Dispatcher::set_transport_failure_handler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  transport_failure_handler_ = std::move(handler);
}
}  // namespace anbox::input
