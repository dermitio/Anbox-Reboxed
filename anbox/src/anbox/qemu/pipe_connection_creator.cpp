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

#include <iomanip>
#include <sstream>
#include <string>
#include <chrono>

#include "anbox/graphics/opengles_message_processor.h"
#include "anbox/graphics/gfxstream_backend.h"
#include "anbox/graphics/gfxstream_message_processor.h"
#include "anbox/graphics/opengles_socket_connection.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/logger.h"
#include "anbox/network/local_socket_messenger.h"
#include "anbox/qemu/adb_message_processor.h"
#include "anbox/qemu/boot_properties_message_processor.h"
#include "anbox/qemu/bootanimation_message_processor.h"
#include "anbox/qemu/camera_message_processor.h"
#include "anbox/qemu/fingerprint_message_processor.h"
#include "anbox/qemu/gsm_message_processor.h"
#include "anbox/qemu/hwcontrol_message_processor.h"
#include "anbox/qemu/null_message_processor.h"
#include "anbox/qemu/pipe_connection_creator.h"
#include "anbox/qemu/refcount_message_processor.h"
#include "anbox/qemu/sensors_message_processor.h"
#include "anbox/qemu/gps_message_processor.h"

namespace ba = boost::asio;

namespace {
std::atomic<std::uint64_t> next_gl_process_pipe_token{1};

std::string bytes_to_hex(const void *data, std::size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    if (i != 0)
      output << ' ';
    output << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return output.str();
}

std::string client_type_to_string(
    const anbox::qemu::PipeConnectionCreator::client_type &type) {
  switch (type) {
    case anbox::qemu::PipeConnectionCreator::client_type::opengles:
      return "opengles";
    case anbox::qemu::PipeConnectionCreator::client_type::gl_process_pipe:
      return "GLProcessPipe";
    case anbox::qemu::PipeConnectionCreator::client_type::refcount:
      return "refcount";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_boot_properties:
      return "boot-properties";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_hw_control:
      return "hw-control";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_sensors:
      return "sensors";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_camera:
      return "camera";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_fingerprint:
      return "fingerprint";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_gsm:
      return "gsm";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_adb:
      return "adb";
    case anbox::qemu::PipeConnectionCreator::client_type::bootanimation:
      return "boot-animation";
    case anbox::qemu::PipeConnectionCreator::client_type::qemud_gps:
      return "gps";
    case anbox::qemu::PipeConnectionCreator::client_type::invalid:
      break;
    default:
      break;
  }
  return "unknown";
}

class GfxstreamProcessLifetime final
    : public anbox::network::MessageProcessor {
 public:
  GfxstreamProcessLifetime(
      std::shared_ptr<anbox::graphics::GfxstreamBackend> backend,
      std::uint64_t id)
      : backend_(std::move(backend)), id_(id),
        created_at_(std::chrono::steady_clock::now()) {
    DEBUG("GLProcess lifecycle: create token=%llu",
          static_cast<unsigned long long>(id_));
    backend_->create_process(id_);
  }
  ~GfxstreamProcessLifetime() override {
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - created_at_).count();
    DEBUG("GLProcess lifecycle: cleanup token=%llu age_ms=%lld",
          static_cast<unsigned long long>(id_),
          static_cast<long long>(age));
    backend_->cleanup_process(id_);
  }

 private:
  std::shared_ptr<anbox::graphics::GfxstreamBackend> backend_;
  std::uint64_t id_;
  std::chrono::steady_clock::time_point created_at_;
};
}
namespace anbox::qemu {
PipeConnectionCreator::PipeConnectionCreator(
    std::shared_ptr<Renderer> renderer,
    std::shared_ptr<graphics::GfxstreamBackend> gfxstream,
    std::shared_ptr<Runtime> rt,
    std::shared_ptr<anbox::application::SensorsState> sensors_state,
    std::shared_ptr<anbox::application::GpsInfoBroker> gpsInfoBroker)
    : renderer_(renderer),
      gfxstream_(std::move(gfxstream)),
      runtime_(rt),
      sensors_state_(sensors_state),
      gps_info_broker_(gpsInfoBroker),
      next_connection_id_(0),
      connections_(
          std::make_shared<network::Connections<network::SocketConnection>>()) {
}

PipeConnectionCreator::~PipeConnectionCreator() noexcept {
  connections_->clear();
}

void PipeConnectionCreator::create_connection_for(
    std::shared_ptr<boost::asio::local::stream_protocol::socket> const
        &socket) {
  DEBUG("Accepted guest qemu pipe connection for /dev/qemu_pipe");
  auto const messenger = std::make_shared<network::LocalSocketMessenger>(socket);
  const auto type = identify_client(messenger);

  std::uint64_t process_token = 0;
  if (type == client_type::gl_process_pipe) {
    std::int32_t confirmation = 0;
    const auto error = messenger->receive_msg(
        ba::buffer(&confirmation, sizeof(confirmation)));
    if (error)
      BOOST_THROW_EXCEPTION(std::runtime_error(
          "Failed to read GLProcessPipe confirmation: " + error.message()));
    if (confirmation != 100)
      BOOST_THROW_EXCEPTION(std::runtime_error(
          "Unexpected GLProcessPipe confirmation value"));

    // Required protocol acknowledgement only. This token is never stored in
    // Renderer and never participates in resource ownership or EOF cleanup.
    process_token =
        next_gl_process_pipe_token.fetch_add(1, std::memory_order_relaxed);
    messenger->send(reinterpret_cast<const char *>(&process_token),
                    sizeof(process_token));
  }

  auto const processor = type == client_type::gl_process_pipe && gfxstream_
      ? std::static_pointer_cast<network::MessageProcessor>(
            std::make_shared<GfxstreamProcessLifetime>(gfxstream_,
                                                       process_token))
      : create_processor(type, messenger);
  if (!processor)
    BOOST_THROW_EXCEPTION(std::runtime_error("Unhandled client type"));

  std::shared_ptr<network::SocketConnection> connection;
  if (type == client_type::opengles)
    connection = std::make_shared<graphics::OpenGlesSocketConnection>(
        messenger, messenger, next_id(), connections_, processor);
  else
    connection = std::make_shared<network::SocketConnection>(
        messenger, messenger, next_id(), connections_, processor);

  connection->set_name(client_type_to_string(type));
  connections_->add(connection);
  connection->read_next_message();
}

PipeConnectionCreator::client_type PipeConnectionCreator::identify_client(
    std::shared_ptr<network::SocketMessenger> const &messenger) {
  // The client will identify itself as first thing by writing a string
  // in the format 'pipe:<name>[:<arguments>]\0' to the channel.
  std::vector<char> buffer;
  for (;;) {
    unsigned char byte[1] = {0};
    const auto err = messenger->receive_msg(ba::buffer(byte, 1));
    if (err) {
      WARNING("Failed while reading qemu pipe service name: %s",
              err.message().c_str());
      break;
    }
    buffer.push_back(byte[0]);
    if (byte[0] == 0x0) break;
  }

  if (buffer.empty()) {
    WARNING("Guest qemu pipe connection supplied an empty service name");
    return client_type::invalid;
  }

  const auto string_size = buffer.back() == '\0' ? buffer.size() - 1
                                                   : buffer.size();
  const std::string identifier_and_args{buffer.data(), string_size};
  const auto service_hex = bytes_to_hex(buffer.data(), buffer.size());
  DEBUG("Guest qemu pipe service bytes=[%s] name='%s'",
        service_hex.c_str(), identifier_and_args.c_str());

  if (utils::string_starts_with(identifier_and_args, "pipe:opengles"))
    return client_type::opengles;
  else if (identifier_and_args == "pipe:GLProcessPipe" ||
           identifier_and_args == "GLProcessPipe")
    return client_type::gl_process_pipe;
  else if (identifier_and_args == "pipe:refcount" ||
           identifier_and_args == "refcount")
    return client_type::refcount;
  // Even if 'boot-properties' is an argument to the service 'qemud' here we
  // take this as a own service instance as that is what it is.
  else if (utils::string_starts_with(identifier_and_args,
                                     "pipe:qemud:boot-properties"))
    return client_type::qemud_boot_properties;
  else if (utils::string_starts_with(identifier_and_args,
                                     "pipe:qemud:hw-control"))
    return client_type::qemud_hw_control;
  else if (utils::string_starts_with(identifier_and_args, "pipe:qemud:sensors"))
    return client_type::qemud_sensors;
  else if (utils::string_starts_with(identifier_and_args, "pipe:qemud:camera"))
    return client_type::qemud_camera;
  else if (utils::string_starts_with(identifier_and_args,
                                     "pipe:qemud:fingerprintlisten"))
    return client_type::qemud_fingerprint;
  else if (utils::string_starts_with(identifier_and_args, "pipe:qemud:gsm"))
    return client_type::qemud_gsm;
  else if (utils::string_starts_with(identifier_and_args,
                                     "pipe:anbox:bootanimation"))
    return client_type::bootanimation;
  else if (utils::string_starts_with(identifier_and_args, "pipe:qemud:adb"))
    return client_type::qemud_adb;
  else if (utils::string_starts_with(identifier_and_args, "pipe:qemud:gps"))
    return client_type::qemud_gps;

  WARNING("Unsupported guest qemu pipe service '%s'",
          identifier_and_args.c_str());
  return client_type::invalid;
}

std::shared_ptr<network::MessageProcessor>
PipeConnectionCreator::create_processor(
    const client_type &type,
    const std::shared_ptr<network::SocketMessenger> &messenger) {
  if (type == client_type::opengles) {
    if (gfxstream_)
      return std::make_shared<graphics::GfxstreamMessageProcessor>(
          gfxstream_, messenger);
    return std::make_shared<graphics::OpenGlesMessageProcessor>(renderer_,
                                                                messenger);
  }
  else if (type == client_type::refcount)
    return gfxstream_
        ? std::make_shared<qemu::RefCountMessageProcessor>(
              [backend = gfxstream_](std::uint32_t handle) {
                backend->open_color_buffer(handle);
              },
              [backend = gfxstream_](std::uint32_t handle) {
                backend->close_color_buffer(handle);
              })
        : std::make_shared<qemu::RefCountMessageProcessor>(renderer_);
  else if (type == client_type::qemud_boot_properties)
    return std::make_shared<qemu::BootPropertiesMessageProcessor>(messenger);
  else if (type == client_type::qemud_hw_control)
    return std::make_shared<qemu::HwControlMessageProcessor>(messenger);
  else if (type == client_type::qemud_sensors)
    return std::make_shared<qemu::SensorsMessageProcessor>(messenger, sensors_state_);
  else if (type == client_type::qemud_camera)
    return std::make_shared<qemu::CameraMessageProcessor>(messenger);
  else if (type == client_type::qemud_fingerprint)
    return std::make_shared<qemu::FingerprintMessageProcessor>(messenger);
  else if (type == client_type::qemud_gsm)
    return std::make_shared<qemu::GsmMessageProcessor>(messenger);
  else if (type == client_type::qemud_adb)
    return std::make_shared<qemu::AdbMessageProcessor>(runtime_, messenger);
  else if (type == client_type::qemud_gps)
    return std::make_shared<qemu::GpsMessageProcessor>(messenger, gps_info_broker_);

  return std::make_shared<qemu::NullMessageProcessor>();
}

int PipeConnectionCreator::next_id() {
  return next_connection_id_.fetch_add(1);
}
}
