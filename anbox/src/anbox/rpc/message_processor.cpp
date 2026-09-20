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

#include "anbox/rpc/message_processor.h"
#include "anbox/common/variable_length_array.h"
#include "anbox/rpc/constants.h"
#include "anbox/rpc/make_protobuf_object.h"
#include "anbox/rpc/template_message_processor.h"

#include "anbox_rpc.pb.h"

#include <limits>

namespace {
bool size_fits_protobuf_int(size_t size) {
  return size <= static_cast<size_t>(std::numeric_limits<int>::max());
}
}

namespace anbox {
namespace rpc {
const ::std::string &Invocation::method_name() const {
  return invocation_.method_name();
}

const ::std::string &Invocation::parameters() const {
  return invocation_.parameters();
}

google::protobuf::uint32 Invocation::id() const { return invocation_.id(); }

MessageProcessor::MessageProcessor(
    const std::shared_ptr<network::MessageSender> &sender,
    const std::shared_ptr<PendingCallCache> &pending_calls)
    : sender_(sender), pending_calls_(pending_calls) {}

MessageProcessor::~MessageProcessor() {}

bool MessageProcessor::process_data(const std::vector<std::uint8_t> &data) {
  for (const auto &byte : data) buffer_.push_back(byte);

  while (buffer_.size() > 0) {
    if (buffer_.size() < header_size)
      break;

    const auto high = buffer_[0];
    const auto medium = buffer_[1];
    const auto low = buffer_[2];
    size_t const message_size = (high << 16) + (medium << 8) + low;
    const auto message_type = buffer_[3];

    // If we don't have yet all bytes for a new message return and wait
    // until we have all.
    if (buffer_.size() < (message_size + header_size)) break;

    if (message_type == MessageType::invocation) {
      anbox::protobuf::rpc::Invocation raw_invocation;
      if (!size_fits_protobuf_int(message_size) ||
          !raw_invocation.ParseFromArray(buffer_.data() + header_size,
                                         static_cast<int>(message_size)))
        return false;

      dispatch(Invocation(raw_invocation));
    } else if (message_type == MessageType::response) {
      auto result = make_protobuf_object<protobuf::rpc::Result>();
      if (!size_fits_protobuf_int(message_size) ||
          !result->ParseFromArray(buffer_.data() + header_size,
                                  static_cast<int>(message_size)))
        return false;

      if (result->has_id()) {
        bool parsed_response = true;
        pending_calls_->populate_message_for_result(*result,
                                                    [&](google::protobuf::MessageLite *result_message) {
                                                      parsed_response =
                                                          result_message->ParseFromString(result->response());
                                                    });
        if (!parsed_response)
          return false;
        pending_calls_->complete_response(*result);
      }

      for (int n = 0; n < result->events_size(); n++)
        process_event_sequence(result->events(n));
    }

    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + header_size + message_size);
  }

  return true;
}

void MessageProcessor::send_response(::google::protobuf::uint32 id,
                                     google::protobuf::MessageLite *response) {
  VariableLengthArray<serialization_buffer_size> send_response_buffer(
      static_cast<size_t>(response->ByteSizeLong()));

  if (!size_fits_protobuf_int(send_response_buffer.size()) ||
      !response->SerializeToArray(send_response_buffer.data(),
                                  static_cast<int>(send_response_buffer.size())))
    return;

  anbox::protobuf::rpc::Result send_response_result;
  send_response_result.set_id(id);
  send_response_result.set_response(send_response_buffer.data(),
                                    send_response_buffer.size());

  send_response_buffer.resize(send_response_result.ByteSizeLong());
  if (!size_fits_protobuf_int(send_response_buffer.size()) ||
      !send_response_result.SerializeToArray(
          send_response_buffer.data(),
          static_cast<int>(send_response_buffer.size())))
    return;

  const size_t size = send_response_buffer.size();
  const unsigned char header_bytes[header_size] = {
      static_cast<unsigned char>((size >> 16) & 0xff),
      static_cast<unsigned char>((size >> 8) & 0xff),
      static_cast<unsigned char>((size >> 0) & 0xff), MessageType::response,
  };

  std::vector<std::uint8_t> send_buffer(sizeof(header_bytes) + size);
  std::copy(header_bytes, header_bytes + sizeof(header_bytes),
            send_buffer.begin());
  std::copy(send_response_buffer.data(),
            send_response_buffer.data() + send_response_buffer.size(),
            send_buffer.begin() + sizeof(header_bytes));

  sender_->send(reinterpret_cast<const char *>(send_buffer.data()),
                send_buffer.size());
}
}  // namespace anbox
}  // namespace network
