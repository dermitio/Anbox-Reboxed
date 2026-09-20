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

#include "anbox/input/device.h"
#include "anbox/logger.h"
#include "anbox/network/delegate_connection_creator.h"
#include "anbox/network/delegate_message_processor.h"
#include "anbox/network/local_socket_messenger.h"
#include "anbox/qemu/null_message_processor.h"

#include <time.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <poll.h>

namespace anbox::input {
std::shared_ptr<Device> Device::create(
    const std::string &path, const std::shared_ptr<Runtime> &runtime) {
  auto sp = std::make_shared<Device>();

  auto delegate_connector = std::make_shared<
      network::DelegateConnectionCreator<boost::asio::local::stream_protocol>>(
      [sp](std::shared_ptr<boost::asio::local::stream_protocol::socket> const
               &socket) { sp->new_client(socket); });

  sp->connector_ = std::make_shared<network::PublishedSocketConnector>(
      path, runtime, delegate_connector);

  // The socket is created with user permissions (e.g. rwx------),
  // which prevents the container from accessing it. Make sure it is writable.
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

  return sp;
}

Device::Device()
    : next_connection_id_(0),
      connections_(
          std::make_shared<network::Connections<network::SocketConnection>>()) {
  ::memset(&info_, 0, sizeof(info_));
}

Device::~Device() {
  destroy_uinput_device();
}

void Device::destroy_uinput_device() {
  if (uinput_fd_ >= 0) {
    ioctl(uinput_fd_, UI_DEV_DESTROY);
    close(uinput_fd_);
    uinput_fd_ = -1;
    uinput_event_path_.clear();
  }
}

bool Device::create_uinput_device() {
  if (uinput_fd_ >= 0) return true;
  const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;

  auto fail = [&]() { close(fd); return false; };
  if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) return fail();
  const struct { int ev; const std::uint8_t *bits; int max; int request; } sets[] = {
      {EV_KEY, info_.key_bitmask, KEY_MAX, UI_SET_KEYBIT},
      {EV_ABS, info_.abs_bitmask, ABS_MAX, UI_SET_ABSBIT},
      {EV_REL, info_.rel_bitmask, REL_MAX, UI_SET_RELBIT},
      {EV_SW, info_.sw_bitmask, SW_MAX, UI_SET_SWBIT},
      {EV_LED, info_.led_bitmask, LED_MAX, UI_SET_LEDBIT},
      {EV_FF, info_.ff_bitmask, FF_MAX, UI_SET_FFBIT},
  };
  for (const auto &set : sets) {
    bool any = false;
    for (int bit = 0; bit <= set.max; ++bit) {
      if (!(set.bits[bit / 8] & (1u << (bit % 8)))) continue;
      if (!any && ioctl(fd, UI_SET_EVBIT, set.ev) < 0) return fail();
      any = true;
      if (ioctl(fd, set.request, bit) < 0) return fail();
    }
  }
  for (int bit = 0; bit <= INPUT_PROP_MAX; ++bit)
    if ((info_.prop_bitmask[bit / 8] & (1u << (bit % 8))) &&
        ioctl(fd, UI_SET_PROPBIT, bit) < 0) return fail();
  if (info_.physical_location[0] != '\0' &&
      ioctl(fd, UI_SET_PHYS, info_.physical_location) < 0)
    return fail();

  struct uinput_user_dev dev{};
  std::snprintf(dev.name, sizeof(dev.name), "%s", info_.name);
  dev.id = info_.id;
  for (int bit = 0; bit < ABS_CNT; ++bit) {
    dev.absmin[bit] = info_.abs_min[bit];
    dev.absmax[bit] = info_.abs_max[bit];
  }
  if (write(fd, &dev, sizeof(dev)) != sizeof(dev) || ioctl(fd, UI_DEV_CREATE) < 0)
    return fail();

  char sysname[64]{};
  if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
    ioctl(fd, UI_DEV_DESTROY);
    return fail();
  }
  for (int attempt = 0; attempt < 50 && uinput_event_path_.empty(); ++attempt) {
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/input", error)) {
      const auto name = entry.path().filename().string();
      if (name.rfind("event", 0) != 0) continue;
      std::ifstream device_name(entry.path() / "device/name");
      std::string value;
      std::getline(device_name, value);
      if (value != info_.name) continue;
      uinput_event_path_ = "/dev/input/" + name;
      break;
    }
    if (uinput_event_path_.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (uinput_event_path_.empty()) {
    ioctl(fd, UI_DEV_DESTROY);
    return fail();
  }

  // Fail closed unless udev has positively marked this exact event device as
  // ignored by libinput and as not being a host input device. This check is
  // completed before the fd is published to Android or any event is sent.
  struct stat event_stat {};
  if (stat(uinput_event_path_.c_str(), &event_stat) < 0) {
    ioctl(fd, UI_DEV_DESTROY);
    return fail();
  }
  const auto udev_record = std::string{"/run/udev/data/c"} +
                           std::to_string(major(event_stat.st_rdev)) + ":" +
                           std::to_string(minor(event_stat.st_rdev));
  bool libinput_ignored = false;
  bool id_input_disabled = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::ifstream properties(udev_record);
    std::string property;
    while (std::getline(properties, property)) {
      libinput_ignored |= property == "E:LIBINPUT_IGNORE_DEVICE=1";
      id_input_disabled |= property == "E:ID_INPUT=0";
    }
    if (libinput_ignored && id_input_disabled) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!libinput_ignored || !id_input_disabled) {
    uinput_event_path_.clear();
    ioctl(fd, UI_DEV_DESTROY);
    return fail();
  }
  uinput_fd_ = fd;
  return true;
}

void Device::send_events(const std::vector<Event> &events) {
  struct timespec spec {};
  clock_gettime(CLOCK_MONOTONIC, &spec);
  const auto timestamp = static_cast<std::uint64_t>(spec.tv_sec) *
                             1000000000ULL +
                         static_cast<std::uint64_t>(spec.tv_nsec);
  if (!deliver_events(events, timestamp, true))
    WARNING("Legacy input delivery failed");
}

bool Device::deliver_events(const std::vector<Event> &events,
                            std::uint64_t monotonic_timestamp_ns,
                            bool include_legacy_connections) {
  struct CompatEvent {
    // NOTE: A bit dirty but as we're running currently a 64 bit container
    // struct input_event has a different size. We rebuild the struct here
    // to reach the correct size.
    std::uint64_t sec;
    std::uint64_t usec;
    std::uint16_t type;
    std::uint16_t code;
    std::uint32_t value;
  };

  if (events.empty())
    return true;
  std::vector<CompatEvent> data(events.size());
  std::size_t n = 0;
  for (const auto &event : events) {
    data[n].sec = monotonic_timestamp_ns / 1000000000ULL;
    data[n].usec = (monotonic_timestamp_ns % 1000000000ULL) / 1000ULL;
    data[n].type = event.type;
    data[n].code = event.code;
    data[n].value = event.value;
    n++;
  }

  bool ok = true;
  if (include_legacy_connections) {
    for (unsigned connection = 0; connection < connections_->size();
         connection++) {
      try {
        connections_->at(connection)->send(
            reinterpret_cast<const char *>(data.data()),
            data.size() * sizeof(CompatEvent));
      } catch (const std::exception &error) {
        ERROR("Input socket transport failed: %s", error.what());
        ok = false;
      }
    }
  }

  if (uinput_fd_ < 0)
    return include_legacy_connections ? ok : false;

  const auto *bytes = reinterpret_cast<const char *>(data.data());
  const std::size_t length = data.size() * sizeof(CompatEvent);
  std::size_t written = 0;
  while (written < length) {
    const ssize_t result = write(uinput_fd_, bytes + written, length - written);
    if (result > 0) {
      if ((result % static_cast<ssize_t>(sizeof(CompatEvent))) != 0) {
        ERROR("uinput returned a partial event record (%zd bytes)", result);
        destroy_uinput_device();
        return false;
      }
      written += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd descriptor {uinput_fd_, POLLOUT, 0};
      if (poll(&descriptor, 1, 5) > 0)
        continue;
    }
    ERROR("uinput delivery failed after %zu/%zu bytes: %s", written, length,
          strerror(errno));
    // Device removal makes Android InputReader release all state even when a
    // final key-up/button-up could not be written to the failed transport.
    destroy_uinput_device();
    return false;
  }
  return ok;
}

void Device::set_name(const std::string &name) {
  snprintf(info_.name, 80, "%s", name.c_str());
}

void Device::set_driver_version(const int &version) {
  info_.driver_version = version;
}

void Device::set_input_id(const struct input_id &id) {
  info_.id.bustype = id.bustype;
  info_.id.product = id.product;
  info_.id.vendor = id.vendor;
  info_.id.version = id.version;
}

void Device::set_physical_location(const std::string &physical_location) {
  snprintf(info_.physical_location, 80, "%s", physical_location.c_str());
}

void Device::set_key_bit(const std::uint64_t &bit) {
  set_bit(info_.key_bitmask, bit);
}

void Device::set_abs_bit(const std::uint64_t &bit) {
  set_bit(info_.abs_bitmask, bit);
}

void Device::set_rel_bit(const std::uint64_t &bit) {
  set_bit(info_.rel_bitmask, bit);
}

void Device::set_sw_bit(const std::uint64_t &bit) {
  set_bit(info_.sw_bitmask, bit);
}

void Device::set_led_bit(const std::uint64_t &bit) {
  set_bit(info_.led_bitmask, bit);
}

void Device::set_ff_bit(const std::uint64_t &bit) {
  set_bit(info_.ff_bitmask, bit);
}

void Device::set_prop_bit(const std::uint64_t &bit) {
  set_bit(info_.prop_bitmask, bit);
}

void Device::set_abs_min(const std::uint64_t &bit, const std::uint32_t &value) {
  info_.abs_min[bit] = value;
}

void Device::set_abs_max(const std::uint64_t &bit, const std::uint32_t &value) {
  info_.abs_max[bit] = value;
}

void Device::set_bit(std::uint8_t *array, const std::uint64_t &bit) {
  array[bit / 8] |= (1 << (bit % 8));
}

void Device::set_unique_id(const std::string &unique_id) {
  snprintf(info_.unique_id, 80, "%s", unique_id.c_str());
}

std::string Device::socket_path() const { return connector_->socket_file(); }

int Device::next_id() { return next_connection_id_++; }

void Device::new_client(
    std::shared_ptr<boost::asio::local::stream_protocol::socket> const
        &socket) {
  auto const messenger =
      std::make_shared<network::LocalSocketMessenger>(socket);
  auto const &connection = std::make_shared<network::SocketConnection>(
      messenger, messenger, next_id(), connections_,
      std::make_shared<qemu::NullMessageProcessor>());
  connection->set_name("input-device");
  connections_->add(connection);

  // Send all necessary information about our device so that the remote
  // side can properly configure itself for this input device
  connection->send(reinterpret_cast<char const *>(&info_), sizeof(info_));
}
}
