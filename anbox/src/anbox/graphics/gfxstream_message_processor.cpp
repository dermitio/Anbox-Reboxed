/*
 * Copyright (C) 2026 The Anbox Reboxed Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "anbox/graphics/gfxstream_message_processor.h"

#include "anbox/graphics/gfxstream_backend.h"
#include "anbox/logger.h"
#include "anbox/network/socket_messenger.h"

#ifdef ANBOX_HAVE_GFXSTREAM
#include <render-utils/RenderChannel.h>
#endif

#include <cstring>
#include <cerrno>
#include <stdexcept>

namespace anbox::graphics {
GfxstreamMessageProcessor::GfxstreamMessageProcessor(
    const std::shared_ptr<GfxstreamBackend>& backend,
    const std::shared_ptr<network::SocketMessenger>& messenger)
    : backend_(backend), messenger_(messenger),
      channel_(backend_->create_render_channel()) {
#ifdef ANBOX_HAVE_GFXSTREAM
  if (!channel_)
    throw std::runtime_error("Failed to create gfxstream render channel");
  response_thread_ = std::thread(
      &GfxstreamMessageProcessor::drain_responses, this);
#else
  throw std::runtime_error("gfxstream support is not compiled in");
#endif
}

GfxstreamMessageProcessor::~GfxstreamMessageProcessor() {
  stopping_.store(true);
#ifdef ANBOX_HAVE_GFXSTREAM
  if (channel_)
    channel_->stop();
#endif
  if (response_thread_.joinable())
    response_thread_.join();
}

bool GfxstreamMessageProcessor::process_data(network::MessageBuffer&& data) {
#ifdef ANBOX_HAVE_GFXSTREAM
  gfxstream::RenderChannel::Buffer outgoing;
  outgoing.resize_noinit(data.size());
  std::memcpy(outgoing.data(), data.data(), data.size());
  for (;;) {
    const auto result = channel_->tryWrite(std::move(outgoing));
    if (result == gfxstream::RenderChannel::IoResult::Ok)
      return true;
    if (result == gfxstream::RenderChannel::IoResult::Error)
      return false;
    channel_->waitUntilWritable();
  }
#else
  (void)data;
  return false;
#endif
}

void GfxstreamMessageProcessor::drain_responses() {
#ifdef ANBOX_HAVE_GFXSTREAM
  while (!stopping_.load()) {
    channel_->waitUntilReadable();
    if (stopping_.load())
      break;
    for (;;) {
      gfxstream::RenderChannel::Buffer incoming;
      const auto result = channel_->tryRead(&incoming);
      if (result == gfxstream::RenderChannel::IoResult::TryAgain)
        break;
      if (result == gfxstream::RenderChannel::IoResult::Error)
        return;
      std::size_t offset = 0;
      while (offset < incoming.size() && !stopping_.load()) {
        const auto written = messenger_->send_raw(
            incoming.data() + offset, incoming.size() - offset);
        if (written > 0) {
          offset += static_cast<std::size_t>(written);
          continue;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN))
          continue;
        ERROR("gfxstream response transport stopped after %zu/%zu bytes: %s",
              offset, incoming.size(),
              written == 0 ? "peer closed the socket" : std::strerror(errno));
        channel_->stop();
        return;
      }
    }
  }
#endif
}
}
