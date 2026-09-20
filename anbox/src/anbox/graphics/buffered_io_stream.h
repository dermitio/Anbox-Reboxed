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

#ifndef ANBOX_GRAPHICS_BUFFERED_IO_STREAM_H_
#define ANBOX_GRAPHICS_BUFFERED_IO_STREAM_H_

#include "external/android-emugl/host/include/libOpenglRender/IOStream.h"

#include "anbox/graphics/buffer_queue.h"
#include "anbox/network/socket_messenger.h"

#include <memory>
#include <thread>
#include <atomic>

namespace anbox::graphics {
class BufferedIOStream : public IOStream {
 public:
  static const size_t default_buffer_size{384};

  explicit BufferedIOStream(
      const std::shared_ptr<anbox::network::SocketMessenger> &messenger,
      size_t buffer_size = default_buffer_size);

  virtual ~BufferedIOStream();

  void *allocBuffer(size_t min_size) override;
  size_t commitBuffer(size_t size) override;
  const unsigned char *read(void *buf, size_t *inout_len) override;
  void forceStop() override;
  void post_data(Buffer &&data);

  bool needs_data();
  void trace_request(uint32_t opcode, uint32_t request_size);
  uint64_t connection_id() const { return connection_id_; }

 private:
  void thread_main();

  std::shared_ptr<anbox::network::SocketMessenger> messenger_;
  std::mutex lock_;
  std::mutex out_lock_;
  Buffer write_buffer_;
  Buffer read_buffer_;
  size_t read_buffer_left_ = 0;
  BufferQueue in_queue_;
  BufferQueue out_queue_;
  const uint64_t connection_id_;
  std::thread worker_thread_;
  std::atomic<uint64_t> request_sequence_{0};
  std::atomic<uint64_t> response_sequence_{0};
  std::atomic<uint64_t> sent_sequence_{0};
  std::atomic<uint32_t> current_opcode_{0};
  std::atomic<uint32_t> current_request_size_{0};
};
}
#endif
