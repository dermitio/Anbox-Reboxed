/*
 * Copyright (C) 2017 Simon Fels <morphis@gravedo.de>
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

#include "anbox/common/mount_entry.h"
#include "anbox/common/loop_device.h"
#include "anbox/logger.h"

#include <cerrno>
#include <sys/mount.h>

namespace anbox::common {
std::shared_ptr<MountEntry> MountEntry::create(const boost::filesystem::path &src, const boost::filesystem::path &target,
                                               const std::string &fs_type, unsigned long flags, const std::string &data) {
  auto entry = std::shared_ptr<MountEntry>(new MountEntry(target));
  if (!entry)
    return nullptr;

  const void *mount_data = nullptr;
  if (!data.empty())
    mount_data = reinterpret_cast<const void*>(data.c_str());

  DEBUG("Mounting %s on %s ...", src, target);

  const unsigned long propagation_flags =
      flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE);
  const unsigned long mount_flags =
      flags & ~(MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE);

  if (::mount(src.c_str(), target.c_str(),
              !fs_type.empty() ? fs_type.c_str() : nullptr, mount_flags,
              mount_data) < 0) {
    ERROR("Failed to mount %s: %s", target, strerror(errno));
    return nullptr;
  }

  // Propagation changes are a separate mount operation. Combining MS_PRIVATE
  // with MS_BIND or a filesystem mount leaves the new mount shared on Linux.
  if (propagation_flags != 0 &&
      ::mount(nullptr, target.c_str(), nullptr, propagation_flags, nullptr) <
          0) {
    ERROR("Failed to change mount propagation for %s: %s", target,
          strerror(errno));
    ::umount2(target.c_str(), MNT_DETACH);
    return nullptr;
  }

  entry->active_ = true;

  return entry;
}

std::shared_ptr<MountEntry> MountEntry::create(const std::shared_ptr<LoopDevice> &loop, const boost::filesystem::path &target,
                                               const std::string &fs_type, unsigned long flags, const std::string &data) {
  auto entry = create(loop->path(), target, fs_type, flags, data);
  if (!entry)
    return nullptr;

  entry->loop_ = loop;
  return entry;
}

std::shared_ptr<MountEntry> MountEntry::create(const boost::filesystem::path &target) {
  auto entry = std::shared_ptr<MountEntry>(new MountEntry(target));
  if (!entry)
    return nullptr;

  entry->active_ = true;

  return entry;
}

MountEntry::MountEntry(const boost::filesystem::path &target) :
  active_{false}, target_{target} {}

MountEntry::~MountEntry() {
  if (!active_)
    return;

  DEBUG("Detaching mount %s", target_);
  if (::umount2(target_.c_str(), MNT_DETACH) < 0 && errno != EINVAL &&
      errno != ENOENT) {
    ERROR("Failed to detach mount %s: %s", target_, strerror(errno));
  }
  active_ = false;
}
}
