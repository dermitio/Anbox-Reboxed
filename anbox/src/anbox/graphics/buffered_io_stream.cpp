/*
 * Copyright (C) 2016 Simon Fels <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "anbox/graphics/buffered_io_stream.h"
#include "anbox/logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<uint64_t> next_stream_connection_id{1};

bool trace_gl_transport() {
  static const bool enabled = [] {
    const auto* value = std::getenv("ANBOX_GL_TRANSPORT_TRACE");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}
}

namespace anbox::graphics {
BufferedIOStream::BufferedIOStream(
    const std::shared_ptr<anbox::network::SocketMessenger> &messenger,
    size_t buffer_size)
    : IOStream(buffer_size),
      messenger_(messenger),
      in_queue_(1024U),
      out_queue_(16U),
      connection_id_(next_stream_connection_id.fetch_add(1)),
      worker_thread_(&BufferedIOStream::thread_main, this) {
  write_buffer_.resize_noinit(buffer_size);
}

BufferedIOStream::~BufferedIOStream() {
  forceStop();
  if (worker_thread_.joinable()) worker_thread_.join();
}

void *BufferedIOStream::allocBuffer(size_t min_size) {
  std::unique_lock<std::mutex> l(out_lock_);
  if (write_buffer_.size() < min_size) write_buffer_.resize_noinit(min_size);
  return write_buffer_.data();
}

size_t BufferedIOStream::commitBuffer(size_t size) {
  std::unique_lock<std::mutex> l(out_lock_);
  assert(size <= write_buffer_.size());
  const auto response_seq = response_sequence_.fetch_add(1) + 1;
  uint32_t raw = 0;
  if (size >= sizeof(raw))
    std::memcpy(&raw, write_buffer_.data(), sizeof(raw));
  if (trace_gl_transport())
    fprintf(stderr,
            "GLTRANSPORT_RESPONSE conn=%llu req_seq=%llu resp_seq=%llu "
            "opcode=%u request_size=%u response_size=%zu raw=%#x\n",
            static_cast<unsigned long long>(connection_id_),
            static_cast<unsigned long long>(request_sequence_.load()),
            static_cast<unsigned long long>(response_seq),
            current_opcode_.load(), current_request_size_.load(), size, raw);
  if (write_buffer_.isAllocated()) {
    write_buffer_.resize(size);
    out_queue_.push_locked(std::move(write_buffer_), l);
  } else {
    out_queue_.push_locked(
        Buffer{write_buffer_.data(), write_buffer_.data() + size}, l);
  }
  return size;
}

const unsigned char *BufferedIOStream::read(void *buf, size_t *inout_len) {
  std::unique_lock<std::mutex> l(lock_);
  size_t wanted = *inout_len;
  size_t count = 0U;
  auto dst = static_cast<uint8_t *>(buf);
  while (count < wanted) {
    if (read_buffer_left_ > 0) {
      size_t avail = std::min<size_t>(wanted - count, read_buffer_left_);
      memcpy(dst + count,
             read_buffer_.data() + (read_buffer_.size() - read_buffer_left_),
             avail);
      count += avail;
      read_buffer_left_ -= avail;
      continue;
    }

    bool blocking = (count == 0);
    auto result = -EIO;
    if (blocking)
      result = in_queue_.pop_locked(&read_buffer_, l);
    else
      result = in_queue_.try_pop_locked(&read_buffer_);

    if (result == 0) {
      read_buffer_left_ = read_buffer_.size();
      continue;
    }

    if (count > 0) break;

    // If we end up here something went wrong and we couldn't read
    // any valid data.
    return nullptr;
  }

  *inout_len = count;
  return static_cast<const unsigned char *>(buf);
}

void BufferedIOStream::forceStop() {
  std::lock_guard<std::mutex> l(lock_);
  in_queue_.close_locked();
  out_queue_.close_locked();
}

void BufferedIOStream::post_data(Buffer &&data) {
  std::unique_lock<std::mutex> l(lock_);
  in_queue_.push_locked(std::move(data), l);
}

bool BufferedIOStream::needs_data() {
  std::unique_lock<std::mutex> l(lock_);
  return !in_queue_.can_pop_locked();
}

void BufferedIOStream::trace_request(uint32_t opcode, uint32_t request_size) {
  current_opcode_.store(opcode);
  current_request_size_.store(request_size);
  const auto sequence = request_sequence_.fetch_add(1) + 1;
  if (trace_gl_transport())
    fprintf(stderr,
            "GLTRANSPORT_REQUEST conn=%llu req_seq=%llu opcode=%u request_size=%u\n",
            static_cast<unsigned long long>(connection_id_),
            static_cast<unsigned long long>(sequence), opcode, request_size);
}

void BufferedIOStream::thread_main() {
  while (true) {
    std::unique_lock<std::mutex> l(out_lock_);

    Buffer buffer;
    const auto result = out_queue_.pop_locked(&buffer, l);
    if (result != 0 && result != -EAGAIN) break;

    const auto sent_seq = sent_sequence_.fetch_add(1) + 1;
    uint32_t raw = 0;
    if (buffer.size() >= sizeof(raw))
      std::memcpy(&raw, buffer.data(), sizeof(raw));
    if (trace_gl_transport())
      fprintf(stderr,
              "GLTRANSPORT_WRITE_BEGIN conn=%llu sent_seq=%llu size=%zu raw=%#x\n",
              static_cast<unsigned long long>(connection_id_),
              static_cast<unsigned long long>(sent_seq), buffer.size(), raw);
    auto bytes_left = buffer.size();
    while (bytes_left > 0) {
      const auto written = messenger_->send_raw(
          buffer.data() + (buffer.size() - bytes_left), bytes_left);
      if (written < 0) {
        if (trace_gl_transport())
          fprintf(stderr,
                  "GLTRANSPORT_WRITE conn=%llu sent_seq=%llu requested=%zu result=%zd errno=%d\n",
                  static_cast<unsigned long long>(connection_id_),
                  static_cast<unsigned long long>(sent_seq), bytes_left,
                  written, errno);
        if (errno != EINTR && errno != EAGAIN) {
          ERROR("Failed to write data: %s", std::strerror(errno));
          break;
        }
        // Socket is busy, lets try again
      } else {
        if (trace_gl_transport())
          fprintf(stderr,
                  "GLTRANSPORT_WRITE conn=%llu sent_seq=%llu requested=%zu result=%zd errno=0\n",
                  static_cast<unsigned long long>(connection_id_),
                  static_cast<unsigned long long>(sent_seq), bytes_left,
                  written);
        bytes_left -= written;
      }
    }
  }
}
}
