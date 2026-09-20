/*
 * Copyright (C) 2026 The Anbox Project
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 */

#include "anbox/qemu/refcount_message_processor.h"

#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/logger.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace {
std::string bytes_to_hex(const std::vector<std::uint8_t> &data) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i != 0)
      output << ' ';
    output << std::setw(2) << static_cast<unsigned int>(data[i]);
  }
  return output.str();
}
}

namespace anbox::qemu {
RefCountMessageProcessor::RefCountMessageProcessor(
    const std::shared_ptr<Renderer>& renderer)
    : renderer_(renderer),
      close_color_buffer_([renderer](std::uint32_t handle) {
        renderer->closeColorBuffer(handle);
      }),
      color_buffer_(0),
      created_at_(std::chrono::steady_clock::now()) {}

RefCountMessageProcessor::RefCountMessageProcessor(
    std::function<void(std::uint32_t)> open_color_buffer,
    std::function<void(std::uint32_t)> close_color_buffer)
    : open_color_buffer_(std::move(open_color_buffer)),
      close_color_buffer_(std::move(close_color_buffer)),
      color_buffer_(0),
      created_at_(std::chrono::steady_clock::now()) {}

RefCountMessageProcessor::RefCountMessageProcessor(
    std::function<void(std::uint32_t)> close_color_buffer)
    : close_color_buffer_(std::move(close_color_buffer)), color_buffer_(0),
      created_at_(std::chrono::steady_clock::now()) {}

RefCountMessageProcessor::~RefCountMessageProcessor() {
  if (!color_buffer_)
    return;

  const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - created_at_).count();
  DEBUG("RefCountPipe lifecycle: release handle=%#x age_ms=%lld",
        color_buffer_, static_cast<long long>(age));
  close_color_buffer_(color_buffer_);
}

bool RefCountMessageProcessor::process_data(
    const std::vector<std::uint8_t> &data) {
  if (color_buffer_) {
    WARNING("RefCountPipe received %zu unexpected bytes after handle=%#x",
            data.size(), color_buffer_);
    return false;
  }

  pending_.insert(pending_.end(), data.begin(), data.end());
  if (pending_.size() < sizeof(color_buffer_))
    return true;
  if (pending_.size() != sizeof(color_buffer_)) {
    WARNING("RefCountPipe invalid handle message size=%zu bytes=[%s]",
            pending_.size(), bytes_to_hex(pending_).c_str());
    return false;
  }

  std::memcpy(&color_buffer_, pending_.data(), sizeof(color_buffer_));
  pending_.clear();
  if (!color_buffer_) {
    WARNING("RefCountPipe received invalid zero color buffer handle");
    return false;
  }

  // The Android O+ gfxstream host creates color buffers with no implicit
  // reference.  A refcount pipe file descriptor is the guest's owning
  // reference, so acquire it as soon as its handle is received and release it
  // when this processor is destroyed.  The legacy renderer retains its
  // historical implicit create reference and therefore leaves this callback
  // unset.
  if (open_color_buffer_)
    open_color_buffer_(color_buffer_);

  DEBUG("RefCountPipe lifecycle: acquire handle=%#x", color_buffer_);
  return true;
}
}
