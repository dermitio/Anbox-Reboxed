/*
 * Copyright (C) 2026 The Anbox Reboxed Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ANBOX_GRAPHICS_GFXSTREAM_MESSAGE_PROCESSOR_H_
#define ANBOX_GRAPHICS_GFXSTREAM_MESSAGE_PROCESSOR_H_

#include "anbox/network/message_processor.h"

#include <atomic>
#include <memory>
#include <thread>

namespace anbox::network {
class SocketMessenger;
}

namespace gfxstream {
class RenderChannel;
using RenderChannelPtr = std::shared_ptr<RenderChannel>;
}

namespace anbox::graphics {
class GfxstreamBackend;

class GfxstreamMessageProcessor : public network::MessageProcessor {
 public:
  GfxstreamMessageProcessor(
      const std::shared_ptr<GfxstreamBackend>& backend,
      const std::shared_ptr<network::SocketMessenger>& messenger);
  ~GfxstreamMessageProcessor() override;

  bool process_data(network::MessageBuffer&& data) override;

 private:
  void drain_responses();

  std::shared_ptr<GfxstreamBackend> backend_;
  std::shared_ptr<network::SocketMessenger> messenger_;
  gfxstream::RenderChannelPtr channel_;
  std::atomic<bool> stopping_{false};
  std::thread response_thread_;
};
}

#endif
