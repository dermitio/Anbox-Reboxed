/*
 * Copyright (C) 2018 Canonical Ltd.
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

#include "anbox/common/binder_device_allocator.h"
#include "anbox/common/binder_device.h"
#include "anbox/common/binderfs.h"
#include "anbox/defer_action.h"
#include "anbox/logger.h"
#include "anbox/utils.h"

#include <boost/filesystem.hpp>

#include <cerrno>
#include <fcntl.h>
#include <linux/loop.h>
#include <sys/ioctl.h>

#include <system_error>

namespace fs = boost::filesystem;

namespace {
const constexpr char* binderfs_base_path{BINDERFS_PATH};
const constexpr char* binderfs_control_path{BINDERFS_PATH "/binder-control"};
const constexpr char* default_binder_device_name{"binder"};
const constexpr uint32_t max_binder_device_allocation_attempts{1024};
} // namespace

namespace anbox::common {
bool BinderDeviceAllocator::is_supported() {
  return fs::exists(binderfs_control_path);
}

std::unique_ptr<BinderDevice> BinderDeviceAllocator::new_device() {
  static uint32_t next_id = 0;

  const auto ctl_fd = ::open(binderfs_control_path, O_RDWR);
  if (ctl_fd < 0) {
    ERROR("Failed to access binder control: %s", std::strerror(errno));
    return nullptr;
  }

  DeferAction close_ctl_fd{[&]() { ::close(ctl_fd); }};

  for (uint32_t attempt = 0; attempt < max_binder_device_allocation_attempts; ++attempt) {
    binderfs_device dev;
    std::memset(&dev, 0, sizeof(binderfs_device));

    const auto device_name =
        utils::string_format("%s%d", default_binder_device_name, next_id++);
    if (device_name.length() > BINDERFS_MAX_NAME) {
      ERROR("Invalid binder device name: %s", device_name);
      return nullptr;
    }

    std::memcpy(dev.name, device_name.c_str(), device_name.length());

    if (::ioctl(ctl_fd, BINDER_CTL_ADD, &dev) < 0) {
      // A container manager killed during teardown can leave its BinderFS
      // nodes behind. Names are process-local counters, so skip stale nodes
      // instead of making all subsequent container starts fail.
      if (errno == EEXIST)
        continue;
      ERROR("Failed to allocate new binder node: %s", std::strerror(errno));
      return nullptr;
    }

    const auto path = utils::string_format("%s/%s", binderfs_base_path, device_name);
    if (!fs::exists(path)) {
      ERROR("Allocated binder device %s is missing", path);
      return nullptr;
    }

    return BinderDevice::create(path);
  }

  ERROR("Failed to allocate a unique binder node after %u attempts",
        max_binder_device_allocation_attempts);
  return nullptr;
}
}
