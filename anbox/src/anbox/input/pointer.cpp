#include "anbox/input/pointer.h"

#include "anbox/logger.h"

#include <linux/input.h>

namespace anbox::input {
Pointer::Pointer(std::shared_ptr<EventSink> sink) : sink_(std::move(sink)) {}

Pointer::~Pointer() {
  const auto stats = diagnostics();
  INFO("Pointer diagnostics: motions=%llu buttons=%llu wheels=%llu duplicates=%llu forced_releases=%llu",
       static_cast<unsigned long long>(stats.motions),
       static_cast<unsigned long long>(stats.buttons),
       static_cast<unsigned long long>(stats.wheels),
       static_cast<unsigned long long>(stats.duplicates),
       static_cast<unsigned long long>(stats.forced_releases));
}

bool Pointer::absolute(int x, int y, std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++diagnostics_.motions;
  EventBatch batch{Stream::Pointer, BatchKind::Motion,
                   {{EV_ABS, ABS_X, x}, {EV_ABS, ABS_Y, y},
                    {EV_SYN, SYN_REPORT, 0}},
                   ++next_source_sequence_, 0, source_time_ns, 0};
  return sink_->submit(std::move(batch));
}

bool Pointer::relative(int dx, int dy, std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (dx == 0 && dy == 0)
    return true;
  ++diagnostics_.motions;
  EventBatch batch;
  batch.stream = Stream::Pointer;
  batch.kind = BatchKind::Motion;
  batch.source_sequence = ++next_source_sequence_;
  batch.source_time_ns = source_time_ns;
  if (dx != 0) batch.events.push_back({EV_REL, REL_X, dx});
  if (dy != 0) batch.events.push_back({EV_REL, REL_Y, dy});
  batch.events.push_back({EV_SYN, SYN_REPORT, 0});
  return sink_->submit(std::move(batch));
}

bool Pointer::button(std::uint16_t linux_code, bool pressed,
                     std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++diagnostics_.buttons;
  const auto source_sequence = ++next_source_sequence_;
  if (pressed) {
    if (!host_held_.insert(linux_code).second) {
      ++diagnostics_.duplicates;
      return true;
    }
    EventBatch down{Stream::Pointer, BatchKind::Button,
                    {{EV_KEY, linux_code, 1}, {EV_SYN, SYN_REPORT, 0}},
                    source_sequence, 0, source_time_ns, 0};
    if (sink_->submit(std::move(down))) {
      injected_held_.insert(linux_code);
      return true;
    }
    return false;
  }
  if (host_held_.erase(linux_code) == 0) {
    ++diagnostics_.duplicates;
    return true;
  }
  if (injected_held_.erase(linux_code) == 0)
    return true;
  EventBatch up{Stream::Pointer, BatchKind::Button,
                {{EV_KEY, linux_code, 0}, {EV_SYN, SYN_REPORT, 0}},
                source_sequence, 0, source_time_ns, 0};
  return sink_->submit(std::move(up));
}

bool Pointer::wheel(int vertical, int horizontal,
                    std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (vertical == 0 && horizontal == 0)
    return true;
  ++diagnostics_.wheels;
  EventBatch batch;
  batch.stream = Stream::Pointer;
  batch.kind = BatchKind::Wheel;
  batch.source_sequence = ++next_source_sequence_;
  batch.source_time_ns = source_time_ns;
  if (vertical != 0) batch.events.push_back({EV_REL, REL_WHEEL, vertical});
  if (horizontal != 0) batch.events.push_back({EV_REL, REL_HWHEEL, horizontal});
  batch.events.push_back({EV_SYN, SYN_REPORT, 0});
  return sink_->submit(std::move(batch));
}

void Pointer::release_all(const std::string &reason,
                          std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  host_held_.clear();
  if (injected_held_.empty())
    return;
  EventBatch batch;
  batch.stream = Stream::Pointer;
  batch.kind = BatchKind::ForcedRelease;
  batch.source_sequence = ++next_source_sequence_;
  batch.source_time_ns = source_time_ns;
  for (const auto code : injected_held_)
    batch.events.push_back({EV_KEY, code, 0});
  batch.events.push_back({EV_SYN, SYN_REPORT, 0});
  diagnostics_.forced_releases += injected_held_.size();
  INFO("Pointer forced release: reason='%s' buttons=%zu source_seq=%llu",
       reason.c_str(), injected_held_.size(),
       static_cast<unsigned long long>(batch.source_sequence));
  injected_held_.clear();
  sink_->submit(std::move(batch));
}

void Pointer::reset_after_transport_failure() {
  std::lock_guard<std::mutex> lock(mutex_);
  diagnostics_.forced_releases += injected_held_.size();
  host_held_.clear();
  injected_held_.clear();
}

Pointer::Diagnostics Pointer::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}
}  // namespace anbox::input
