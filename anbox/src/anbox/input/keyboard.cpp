#include "anbox/input/keyboard.h"

#include "anbox/logger.h"

#include <linux/input.h>

namespace anbox::input {
Keyboard::Keyboard(std::shared_ptr<EventSink> sink, RepeatPolicy repeat_policy)
    : sink_(std::move(sink)), repeat_policy_(repeat_policy) {}

Keyboard::~Keyboard() {
  const auto stats = diagnostics();
  INFO("Keyboard diagnostics: host_events=%llu repeats=%llu duplicates=%llu forced_releases=%llu",
       static_cast<unsigned long long>(stats.host_events),
       static_cast<unsigned long long>(stats.repeats),
       static_cast<unsigned long long>(stats.duplicates),
       static_cast<unsigned long long>(stats.forced_releases));
}

bool Keyboard::key(std::uint16_t linux_code, bool pressed, bool host_repeat,
                   std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++diagnostics_.host_events;
  const auto source_sequence = ++next_source_sequence_;
  if (linux_code == KEY_RESERVED)
    return false;

  if (host_repeat) {
    ++diagnostics_.repeats;
    if (!pressed || host_held_.find(linux_code) == host_held_.end() ||
        repeat_policy_ == RepeatPolicy::IgnoreHost)
      return true;
    EventBatch repeat{Stream::Keyboard, BatchKind::Repeat,
                      {{EV_KEY, linux_code, 2}, {EV_SYN, SYN_REPORT, 0}},
                      source_sequence, 0, source_time_ns, 0};
    return sink_->submit(std::move(repeat));
  }

  if (pressed) {
    if (!host_held_.insert(linux_code).second) {
      ++diagnostics_.duplicates;
      return true;
    }
    EventBatch down{Stream::Keyboard, BatchKind::Key,
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
  EventBatch up{Stream::Keyboard, BatchKind::Key,
                {{EV_KEY, linux_code, 0}, {EV_SYN, SYN_REPORT, 0}},
                source_sequence, 0, source_time_ns, 0};
  return sink_->submit(std::move(up));
}

void Keyboard::release_all(const std::string &reason,
                           std::uint64_t source_time_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  host_held_.clear();
  if (injected_held_.empty())
    return;
  EventBatch batch;
  batch.stream = Stream::Keyboard;
  batch.kind = BatchKind::ForcedRelease;
  batch.source_sequence = ++next_source_sequence_;
  batch.source_time_ns = source_time_ns;
  for (const auto code : injected_held_)
    batch.events.push_back({EV_KEY, code, 0});
  batch.events.push_back({EV_SYN, SYN_REPORT, 0});
  diagnostics_.forced_releases += injected_held_.size();
  INFO("Keyboard forced release: reason='%s' keys=%zu source_seq=%llu",
       reason.c_str(), injected_held_.size(),
       static_cast<unsigned long long>(batch.source_sequence));
  injected_held_.clear();
  sink_->submit(std::move(batch));
}

void Keyboard::reset_after_transport_failure() {
  std::lock_guard<std::mutex> lock(mutex_);
  diagnostics_.forced_releases += injected_held_.size();
  host_held_.clear();
  injected_held_.clear();
}

Keyboard::Diagnostics Keyboard::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}
}  // namespace anbox::input
