/*
 * Copyright (C) 2026 The Anbox Project
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 */

#ifndef ANBOX_QEMU_REFCOUNT_MESSAGE_PROCESSOR_H_
#define ANBOX_QEMU_REFCOUNT_MESSAGE_PROCESSOR_H_

#include "anbox/network/message_processor.h"

#include <memory>
#include <functional>
#include <chrono>

class Renderer;

namespace anbox::qemu {
class RefCountMessageProcessor : public network::MessageProcessor {
 public:
  explicit RefCountMessageProcessor(const std::shared_ptr<Renderer>& renderer);
  RefCountMessageProcessor(
      std::function<void(std::uint32_t)> open_color_buffer,
      std::function<void(std::uint32_t)> close_color_buffer);
  explicit RefCountMessageProcessor(
      std::function<void(std::uint32_t)> close_color_buffer);
  ~RefCountMessageProcessor();

  bool process_data(const std::vector<std::uint8_t> &data) override;

 private:
  std::shared_ptr<Renderer> renderer_;
  std::function<void(std::uint32_t)> open_color_buffer_;
  std::function<void(std::uint32_t)> close_color_buffer_;
  std::vector<std::uint8_t> pending_;
  std::uint32_t color_buffer_;
  std::chrono::steady_clock::time_point created_at_;
};
}
#endif
