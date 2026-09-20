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

#include "anbox/cmds/container_manager.h"
#include "anbox/cmds/compatibility.h"
#include "anbox/container/service.h"
#include "anbox/common/loop_device_allocator.h"
#include "anbox/logger.h"
#include "anbox/runtime.h"
#include "anbox/system_configuration.h"

#include "core/posix/signal.h"
#include "core/posix/exec.h"

#include <sys/mount.h>
#include <sched.h>
#include <linux/loop.h>
#include <fcntl.h>
#include <lxc/lxccontainer.h>

#include <array>
#include <fstream>
#include <sstream>
#include <set>

namespace fs = boost::filesystem;

namespace {
constexpr unsigned int unprivileged_user_id{100000};

bool isolate_mount_namespace() {
  if (::unshare(CLONE_NEWNS) < 0) {
    ERROR("Failed to create a private Anbox Reboxed mount namespace: %s",
          strerror(errno));
    return false;
  }

  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
    ERROR("Failed to make the Anbox Reboxed mount namespace private: %s",
          strerror(errno));
    return false;
  }

  INFO("Anbox Reboxed container manager is using a private mount namespace");
  return true;
}

bool is_secure_root_owned_path(const fs::path &path, mode_t expected_type) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == expected_type && st.st_uid == 0 &&
         (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

fs::path selected_android_image(const fs::path &configured_image) {
  const auto native_bridge_root =
      anbox::SystemConfiguration::instance().data_dir() / "state" /
      "native-bridge";
  const auto marker = native_bridge_root / "enabled";
  if (!fs::exists(marker))
    return configured_image;

  if (!is_secure_root_owned_path(marker, S_IFREG))
    throw std::runtime_error(
        "Native Bridge is enabled but its activation marker is insecure");

  std::ifstream marker_stream{marker.string()};
  std::string marker_kind;
  std::getline(marker_stream, marker_kind);
  if (!marker_stream && !marker_stream.eof())
    throw std::runtime_error("Native Bridge activation marker is unreadable");

  if (marker_kind == "builtin-image-v1") {
    if (!is_secure_root_owned_path(configured_image, S_IFREG))
      throw std::runtime_error(
          "Official Native Bridge image is insecure or missing");
    INFO("Using project-built Native Bridge artifacts from Android image %s",
         configured_image);
    return configured_image;
  }

  if (marker_kind != "managed-system-overlay-v1")
    throw std::runtime_error("Native Bridge activation marker is unsupported");

  const auto managed_image = native_bridge_root / "managed-system.img";
  if (!is_secure_root_owned_path(managed_image, S_IFREG))
    throw std::runtime_error(
        "Native Bridge is enabled but its managed Android image is insecure or missing");
  INFO("Using transactional Native Bridge Android image %s (original %s is unchanged)",
       managed_image, configured_image);
  return managed_image;
}

fs::path selected_data_image() {
  const auto managed_root =
      anbox::SystemConfiguration::instance().data_dir() / "state/data-image";
  const auto marker = managed_root / "enabled";
  if (!fs::exists(marker))
    return {};

  const auto image = managed_root / "android-data.img";
  if (!is_secure_root_owned_path(marker, S_IFREG) ||
      !is_secure_root_owned_path(image, S_IFREG)) {
    throw std::runtime_error(
        "Managed Android data image is enabled but insecure or missing");
  }
  INFO("Using transactional Android data image %s; legacy data directory is retained for rollback",
       image);
  return image;
}

std::string decode_mountinfo_path(const std::string &encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '\\' && i + 3 < encoded.size() &&
        encoded[i + 1] >= '0' && encoded[i + 1] <= '7' &&
        encoded[i + 2] >= '0' && encoded[i + 2] <= '7' &&
        encoded[i + 3] >= '0' && encoded[i + 3] <= '7') {
      decoded.push_back(static_cast<char>((encoded[i + 1] - '0') * 64 +
                                          (encoded[i + 2] - '0') * 8 +
                                          encoded[i + 3] - '0'));
      i += 3;
    } else {
      decoded.push_back(encoded[i]);
    }
  }
  return decoded;
}

std::vector<std::string> current_mountpoints() {
  std::vector<std::string> result;
  std::ifstream mountinfo{"/proc/self/mountinfo"};
  std::string line;
  while (std::getline(mountinfo, line)) {
    std::istringstream fields{line};
    std::string value;
    for (int field = 0; field < 5 && fields >> value; ++field) {
      if (field == 4)
        result.push_back(decode_mountinfo_path(value));
    }
  }
  return result;
}

bool is_mountpoint(const fs::path &path) {
  const auto target = path.lexically_normal().string();
  const auto mounts = current_mountpoints();
  return std::find(mounts.begin(), mounts.end(), target) != mounts.end();
}

bool safe_umount(const fs::path &path) {
  bool success = true;
  while (is_mountpoint(path)) {
    INFO("Detaching mounted Anbox Reboxed path %s", path);
    if (::umount2(path.c_str(), MNT_DETACH) == 0)
      continue;
    if (errno == EINVAL || errno == ENOENT)
      break;
    ERROR("Failed to detach Anbox Reboxed mount %s: %s", path, strerror(errno));
    success = false;
    break;
  }
  if (success)
    DEBUG("Anbox Reboxed path is unmounted: %s", path);
  return success;
}

bool stop_stale_container() {
  const auto config_dir =
      anbox::SystemConfiguration::instance().container_config_dir();
  auto *container = lxc_container_new("default", config_dir.c_str());
  if (!container)
    return true;

  bool success = true;
  if (container->is_running(container)) {
    WARNING("Stopping stale Anbox Reboxed container before rootfs cleanup");
    success = container->stop(container);
    if (!success)
      ERROR("Failed to stop stale Anbox Reboxed container");
  }
  lxc_container_put(container);
  return success;
}

bool cleanup_anbox_mounts() {
  const fs::path data_dir =
      anbox::SystemConfiguration::instance().data_dir();
  const fs::path combined =
      anbox::SystemConfiguration::instance().combined_rootfs_dir();
  const fs::path rootfs =
      anbox::SystemConfiguration::instance().rootfs_dir();
  const fs::path vendor_rootfs = data_dir / "vendor-rootfs";

  std::set<std::string> candidates;
  for (const auto &mountpoint : current_mountpoints()) {
    const bool under_combined =
        mountpoint == combined.string() ||
        mountpoint.rfind(combined.string() + "/", 0) == 0;
    if (under_combined || mountpoint == vendor_rootfs.string() ||
        mountpoint == rootfs.string()) {
      candidates.insert(mountpoint);
    }
  }

  std::vector<std::string> ordered{candidates.begin(), candidates.end()};
  std::sort(ordered.begin(), ordered.end(),
            [](const std::string &left, const std::string &right) {
              if (left.size() != right.size())
                return left.size() > right.size();
              return left > right;
            });

  bool success = true;
  for (const auto &mountpoint : ordered)
    success = safe_umount(mountpoint) && success;
  return success;
}

bool detach_stale_image_loops(const std::vector<fs::path> &images) {
  std::set<std::string> expected;
  for (const auto &image : images) {
    if (!image.empty())
      expected.insert(fs::absolute(image).lexically_normal().string());
  }

  const fs::path sys_block{"/sys/block"};
  if (!fs::exists(sys_block))
    return true;

  bool success = true;
  for (fs::directory_iterator entry{sys_block}, end; entry != end; ++entry) {
    const auto name = entry->path().filename().string();
    if (name.rfind("loop", 0) != 0)
      continue;

    std::ifstream backing_file{(entry->path() / "loop/backing_file").string()};
    std::string backing;
    if (!std::getline(backing_file, backing) || backing.empty())
      continue;
    backing = fs::absolute(backing).lexically_normal().string();
    if (expected.find(backing) == expected.end())
      continue;

    const auto device = fs::path{"/dev"} / name;
    INFO("Detaching stale Anbox Reboxed image loop %s (%s)", device, backing);
    const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      ERROR("Failed to open stale loop %s: %s", device, strerror(errno));
      success = false;
      continue;
    }
    if (::ioctl(fd, LOOP_CLR_FD) < 0 && errno != ENXIO) {
      ERROR("Failed to detach stale loop %s: %s", device, strerror(errno));
      success = false;
    }
    ::close(fd);
  }
  return success;
}

enum class AndroidImageFormat {
  squashfs,
  ext4,
  erofs,
  f2fs,
  android_sparse,
  unknown,
};

AndroidImageFormat detect_android_image_format(const fs::path &path) {
  constexpr std::size_t probe_size{4096};
  constexpr std::size_t ext_magic_offset{1024 + 56};
  constexpr std::size_t erofs_magic_offset{1024};
  constexpr std::size_t f2fs_magic_offset{1024};

  std::array<unsigned char, probe_size> data{};
  std::ifstream image{path.string(), std::ios::binary};
  if (!image.is_open())
    return AndroidImageFormat::unknown;

  image.read(reinterpret_cast<char *>(data.data()), data.size());
  const auto bytes_read = static_cast<std::size_t>(image.gcount());

  if (bytes_read >= 4 &&
      data[0] == 0x68 && data[1] == 0x73 &&
      data[2] == 0x71 && data[3] == 0x73)
    return AndroidImageFormat::squashfs;

  if (bytes_read >= 4 &&
      data[0] == 0x3a && data[1] == 0xff &&
      data[2] == 0x26 && data[3] == 0xed)
    return AndroidImageFormat::android_sparse;

  if (bytes_read >= ext_magic_offset + 2 &&
      data[ext_magic_offset] == 0x53 &&
      data[ext_magic_offset + 1] == 0xef)
    return AndroidImageFormat::ext4;

  if (bytes_read >= erofs_magic_offset + 4 &&
      data[erofs_magic_offset] == 0xe2 &&
      data[erofs_magic_offset + 1] == 0xe1 &&
      data[erofs_magic_offset + 2] == 0xf5 &&
      data[erofs_magic_offset + 3] == 0xe0)
    return AndroidImageFormat::erofs;

  if (bytes_read >= f2fs_magic_offset + 4 &&
      data[f2fs_magic_offset] == 0x10 &&
      data[f2fs_magic_offset + 1] == 0x20 &&
      data[f2fs_magic_offset + 2] == 0xf5 &&
      data[f2fs_magic_offset + 3] == 0xf2)
    return AndroidImageFormat::f2fs;

  return AndroidImageFormat::unknown;
}

const char *filesystem_type_for(AndroidImageFormat format) {
  switch (format) {
  case AndroidImageFormat::squashfs:
    return "squashfs";
  case AndroidImageFormat::ext4:
    return "ext4";
  case AndroidImageFormat::erofs:
    return "erofs";
  case AndroidImageFormat::f2fs:
    return "f2fs";
  default:
    return nullptr;
  }
}

std::string elf_machine_name(std::uint16_t machine) {
  switch (machine) {
  case 3:
    return "x86";
  case 40:
    return "ARM";
  case 62:
    return "x86_64";
  case 183:
    return "ARM64";
  default:
    return anbox::utils::string_format("ELF machine %u", machine);
  }
}

std::uint16_t host_elf_machine() {
#if defined(__x86_64__)
  return 62;
#elif defined(__i386__)
  return 3;
#elif defined(__aarch64__)
  return 183;
#elif defined(__arm__)
  return 40;
#else
  return 0;
#endif
}

std::uint16_t read_elf_machine(const fs::path &path) {
  constexpr std::size_t elf_header_size{20};
  std::array<unsigned char, elf_header_size> data{};
  std::ifstream executable{path.string(), std::ios::binary};
  if (!executable.is_open())
    return 0;

  executable.read(reinterpret_cast<char *>(data.data()), data.size());
  if (executable.gcount() != static_cast<std::streamsize>(data.size()) ||
      data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
    return 0;

  // Android system images use little-endian ELF executables.
  if (data[5] != 1)
    return 0;

  return static_cast<std::uint16_t>(data[18]) |
         (static_cast<std::uint16_t>(data[19]) << 8);
}

bool validate_guest_architecture(const fs::path &rootfs) {
  const auto host_machine = host_elf_machine();
  if (host_machine == 0)
    return true;

  for (const auto &relative_path :
       std::array<const char *, 3>{"system/bin/app_process64",
                                   "system/bin/app_process32",
                                   "system/bin/init"}) {
    const auto executable = rootfs / relative_path;
    if (!fs::exists(executable))
      continue;

    const auto guest_machine = read_elf_machine(executable);
    if (guest_machine == 0)
      continue;

    if (guest_machine != host_machine) {
      ERROR("Android image architecture %s is incompatible with host architecture %s; Anbox Reboxed does not emulate CPUs",
            elf_machine_name(guest_machine), elf_machine_name(host_machine));
      return false;
    }

    return true;
  }

  ERROR("Could not determine Android image architecture from system executables");
  return false;
}
}

anbox::cmds::ContainerManager::ContainerManager()
    : CommandWithFlagsAndAction{
          cli::Name{"container-manager"}, cli::Usage{"container-manager"},
          cli::Description{"Start the container manager service"}, true} {

  flag(cli::make_flag(cli::Name{"android-image"},
                      cli::Description{"Path to the Android rootfs image file if not stored in the data path"},
                      android_img_path_));
  flag(cli::make_flag(cli::Name{"vendor-image"},
                      cli::Description{"Path to an ext4 vendor image; providing it enables GSI vendor mode"},
                      vendor_img_path_));
  flag(cli::make_flag(cli::Name{"data-path"},
                      cli::Description{"Path where the container and its data is stored"},
                      data_path_));
  flag(cli::make_flag(cli::Name{"privileged"},
                      cli::Description{"Run Android container in privileged mode"},
                      privileged_));
  flag(cli::make_flag(cli::Name{"daemon"},
                      cli::Description{"Mark service as being started as systemd daemon"},
                      daemon_));
  flag(cli::make_flag(cli::Name{"use-rootfs-overlay"},
                      cli::Description{"Use an overlay for the Android rootfs"},
                      enable_rootfs_overlay_));
  flag(cli::make_flag(cli::Name{"force-squashfuse"},
                      cli::Description{"Force using squashfuse for mounting the Android rootfs"},
                      enable_squashfuse_));
  flag(cli::make_flag(cli::Name{"container-network-address"},
                      cli::Description{"Assign the specified network address to the Android container"},
                      container_network_address_));
  flag(cli::make_flag(cli::Name{"container-network-gateway"},
                      cli::Description{"Assign the specified network gateway to the Android container"},
                      container_network_gateway_));
  flag(cli::make_flag(cli::Name{"container-network-dns-servers"},
                      cli::Description{"Assign the specified DNS servers to the Android container"},
                      container_network_dns_servers_));

  action([&](const cli::Command::Context&) {
    try {
      if (!daemon_) {
        WARNING("You are running the container manager manually which is most likely not");
        WARNING("what you want. The container manager is normally started by systemd or");
        WARNING("another init system. If you still want to run the container-manager");
        WARNING("you can get rid of this warning by starting with the --daemon option.");
        WARNING("");
      }

      if (geteuid() != 0) {
        ERROR("You are not running the container-manager as root. Generally you don't");
        ERROR("want to run the container-manager manually unless you're a developer");
        ERROR("as it is started by the init system of your operating system.");
        return EXIT_FAILURE;
      }

      auto trap = core::posix::trap_signals_for_process(
          {core::posix::Signal::sig_term, core::posix::Signal::sig_int});
      trap->signal_raised().connect([trap](const core::posix::Signal& signal) {
        INFO("Signal %i received. Good night.", static_cast<int>(signal));
        trap->stop();
      });

      if (!data_path_.empty())
        SystemConfiguration::instance().set_data_path(data_path_);

      if (!fs::exists(data_path_))
        fs::create_directories(data_path_);

      // Root is commonly a shared mount on systemd hosts.  All image and bind
      // mounts, stale-mount cleanup, and LXC startup must happen after leaving
      // the host mount namespace so neither mounts nor unmounts can propagate
      // to unrelated host paths.
      if (!isolate_mount_namespace())
        return EXIT_FAILURE;

      fs::path startup_android_image = android_img_path_;
      if (startup_android_image.empty())
        startup_android_image =
            SystemConfiguration::instance().data_dir() / "android.img";
      startup_android_image = selected_android_image(startup_android_image);
      const auto startup_data_image = selected_data_image();
      fs::path startup_vendor_image = vendor_img_path_;
      if (startup_vendor_image.empty())
        startup_vendor_image =
            SystemConfiguration::instance().data_dir() / "vendor.img";

      if (!stop_stale_container() || !cleanup_anbox_mounts() ||
          !detach_stale_image_loops(
              {startup_android_image, startup_vendor_image,
               startup_data_image}))
        return EXIT_FAILURE;

      if (!setup_mounts())
        return EXIT_FAILURE;

      auto rt = Runtime::create();
      container::Service::Configuration config;
      config.privileged = privileged_;
      config.rootfs_overlay = use_combined_rootfs_;
      config.container_network_address = container_network_address_;
      config.container_network_gateway = container_network_gateway_;

      if (container_network_dns_servers_.length() > 0)
        config.container_network_dns_servers = utils::string_split(container_network_dns_servers_, ',');

      auto service = container::Service::create(rt, config);

      rt->start();
      trap->run();
      rt->stop();

      // Destroy the service first so LXC is stopped before any rootfs mount
      // or loop-device ownership is released.
      service.reset();
      release_mounts();

      return EXIT_SUCCESS;
    } catch (std::exception &err) {
      ERROR("%s", err.what());
      return EXIT_FAILURE;
    }
  });
}

anbox::cmds::ContainerManager::~ContainerManager() {}

void anbox::cmds::ContainerManager::release_mounts() {
  // MountEntry objects own loop devices. Detach every nested mount first so
  // clearing the entries cannot issue LOOP_CLR_FD while a bind mount remains.
  if (!cleanup_anbox_mounts())
    WARNING("One or more Anbox Reboxed mounts could not be detached cleanly");
  mounts_.clear();
}

bool anbox::cmds::ContainerManager::setup_mounts() {
  use_combined_rootfs_ = false;

  fs::path android_img_path = android_img_path_;
  if (android_img_path.empty())
    android_img_path = SystemConfiguration::instance().data_dir() / "android.img";
  android_img_path = selected_android_image(android_img_path);

  if (!fs::exists(android_img_path)) {
    ERROR("Android image does not exist at path %s", android_img_path);
    return false;
  }

  const auto android_rootfs_dir = SystemConfiguration::instance().rootfs_dir();
  if (utils::is_mounted(android_rootfs_dir)) {
    ERROR("Androd rootfs is already mounted!?");
    return false;
  }

  if (!fs::exists(android_rootfs_dir))
    fs::create_directory(android_rootfs_dir);

  const auto image_format = detect_android_image_format(android_img_path);
  if (image_format == AndroidImageFormat::android_sparse) {
    ERROR("Android sparse images cannot be mounted directly. Convert %s to a raw image with simg2img first",
          android_img_path);
    return false;
  }

  const auto filesystem_type = filesystem_type_for(image_format);
  if (!filesystem_type) {
    ERROR("Unsupported Android image format at path %s", android_img_path);
    return false;
  }

  // SquashFS can fall back to squashfuse when loop devices are unavailable.
  // Raw GSI filesystems (ext4, EROFS, and F2FS) require a loop device.
  if (!fs::exists("/dev/loop-control") && !enable_squashfuse_) {
    if (image_format != AndroidImageFormat::squashfs) {
      ERROR("/dev/loop-control is required to mount the %s GSI image",
            filesystem_type);
      return false;
    }

    WARNING("/dev/loop-control not found. Falling back to squashfuse.");
    enable_squashfuse_ = true;
  }

  if (enable_squashfuse_ && image_format != AndroidImageFormat::squashfs) {
    ERROR("--force-squashfuse is only valid for SquashFS images");
    return false;
  }

  if (!enable_squashfuse_) {
    std::shared_ptr<common::LoopDevice> loop_device;

    try {
      loop_device = common::LoopDeviceAllocator::new_device();
    } catch (const std::exception& e) {
      ERROR("Could not create loopback device: %s", e.what());
      return false;
    } catch (...) {
      ERROR("Could not create loopback device");
      return false;
    }

    if (!loop_device->attach_file(android_img_path)) {
      ERROR("Failed to attach Android rootfs image to loopback device");
      return false;
    }

    auto m = common::MountEntry::create(loop_device, android_rootfs_dir,
                                        filesystem_type,
                                        MS_RDONLY | MS_PRIVATE);
    if (!m) {
      ERROR("Failed to mount %s Android rootfs", filesystem_type);
      return false;
    }
    mounts_.push_back(m);
  } else if (fs::exists("/dev/fuse") && !utils::find_program_on_path("squashfuse").empty()) {
    std::vector<std::string> args = {
      "-t", "fuse.squashfuse",
      // Allow other users than root to access the rootfs
      "-o", "allow_other",
      android_img_path.string(),
      android_rootfs_dir,
    };

    // Easiest is here to go with the standard mount program as that
    // will handle everything for us which is relevant to get the
    // squashfs via squashfuse properly mount without having to
    // reimplement all the details. Once the mount call comes back
    // without an error we can expect the image to be mounted.
    auto child = core::posix::exec("/bin/mount", args, {}, core::posix::StandardStream::empty, []() {});
    const auto result = child.wait_for(core::posix::wait::Flags::untraced);
    if (result.status != core::posix::wait::Result::Status::exited ||
        result.detail.if_exited.status != core::posix::exit::Status::success) {
      ERROR("Failed to mount squashfs Android image");
      return false;
    }

    auto m = common::MountEntry::create(android_rootfs_dir);
    if (!m) {
      ERROR("Failed to create mount entry for Android rootfs");
      return false;
    }
    mounts_.push_back(m);
  } else {
    ERROR("No loop device or FUSE support found. Can't setup Android rootfs!");
    return false;
  }

  if (!validate_guest_architecture(android_rootfs_dir)) {
    release_mounts();
    return false;
  }

  fs::path vendor_img_path = vendor_img_path_;
  if (vendor_img_path.empty()) {
    const auto default_vendor_img =
        SystemConfiguration::instance().data_dir() / "vendor.img";
    if (fs::exists(default_vendor_img))
      vendor_img_path = default_vendor_img;
  }

  fs::path vendor_rootfs_dir;
  if (!vendor_img_path.empty()) {
    if (!fs::exists(vendor_img_path)) {
      ERROR("Vendor image does not exist at path %s", vendor_img_path);
      release_mounts();
      return false;
    }

    if (detect_android_image_format(vendor_img_path) !=
        AndroidImageFormat::ext4) {
      ERROR("GSI vendor image must be an ext4 filesystem: %s",
            vendor_img_path);
      release_mounts();
      return false;
    }

    vendor_rootfs_dir =
        SystemConfiguration::instance().data_dir() / "vendor-rootfs";
    if (utils::is_mounted(vendor_rootfs_dir.string())) {
      ERROR("Vendor rootfs is already mounted");
      release_mounts();
      return false;
    }
    if (!fs::exists(vendor_rootfs_dir))
      fs::create_directories(vendor_rootfs_dir);

    std::shared_ptr<common::LoopDevice> vendor_loop_device;
    try {
      vendor_loop_device = common::LoopDeviceAllocator::new_device();
    } catch (const std::exception& e) {
      ERROR("Could not create vendor loopback device: %s", e.what());
      release_mounts();
      return false;
    } catch (...) {
      ERROR("Could not create vendor loopback device");
      release_mounts();
      return false;
    }

    if (!vendor_loop_device->attach_file(vendor_img_path)) {
      ERROR("Failed to attach vendor image to loopback device");
      release_mounts();
      return false;
    }

    auto vendor_mount = common::MountEntry::create(
        vendor_loop_device, vendor_rootfs_dir, "ext4",
        MS_RDONLY | MS_PRIVATE);
    if (!vendor_mount) {
      ERROR("Failed to mount ext4 vendor image");
      release_mounts();
      return false;
    }
    mounts_.push_back(vendor_mount);
  }

  auto final_android_rootfs_dir = android_rootfs_dir;
  if (enable_rootfs_overlay_ && !vendor_rootfs_dir.empty()) {
    WARNING(
        "Disabling rootfs overlay for GSI vendor mode. The GSI system image "
        "remains read-only and writable Android state is provided by the "
        "dedicated /data and /cache mounts.");
    enable_rootfs_overlay_ = false;
  }
  if (!vendor_rootfs_dir.empty()) {
    if (!setup_gsi_rootfs_shell(android_rootfs_dir))
      return false;

    final_android_rootfs_dir =
        SystemConfiguration::instance().combined_rootfs_dir();
    use_combined_rootfs_ = true;
  } else if (enable_rootfs_overlay_) {
    if (!setup_rootfs_overlay())
      return false;

    final_android_rootfs_dir = SystemConfiguration::instance().combined_rootfs_dir();
    use_combined_rootfs_ = true;
  }

  if (!vendor_rootfs_dir.empty()) {
    const auto vendor_target =
        fs::path(final_android_rootfs_dir) / "vendor";
    if (!fs::exists(vendor_target)) {
      ERROR("Android rootfs does not provide a /vendor mount point");
      release_mounts();
      return false;
    }

    auto vendor_bind = common::MountEntry::create(
        vendor_rootfs_dir, vendor_target, "",
        MS_BIND | MS_PRIVATE);
    if (!vendor_bind) {
      ERROR("Failed to bind GSI vendor image at /vendor");
      release_mounts();
      return false;
    }
    mounts_.push_back(vendor_bind);
    INFO("Mounted GSI vendor image %s at /vendor", vendor_img_path);

    // Generate the allowlisted Android 15 GLES encoder overlay while the
    // immutable vendor image is available read-only. Unknown builds remove
    // stale overlays and continue unmodified rather than risking a patch.
    const auto encoder_source =
        vendor_rootfs_dir / "lib64" / "libGLESv2_enc.so";
    try {
      const auto result = cmds::run_gles_compatibility_tool(
          {"auto", "--data-dir",
           SystemConfiguration::instance().data_dir().string(),
           "--source", encoder_source.string()});
      if (result != EXIT_SUCCESS)
        WARNING("Guest GLES compatibility overlay was not applied");
    } catch (const std::exception& error) {
      WARNING("Could not run guest GLES compatibility tool: %s", error.what());
    }

    // Android 15's ranchu mapper still selects the DMA-only update callback.
    // Install an exact-hash, read-only overlay that selects the ordinary
    // serialized update callback. Both transports mount it because Anbox does
    // not expose a real goldfish address-space mapping to the guest.
    const auto mapper_source = vendor_rootfs_dir / "lib64" / "hw" /
        "android.hardware.graphics.mapper@3.0-impl-ranchu.so";
    try {
      const auto result = cmds::run_gfxstream_compatibility_tool(
          {"auto", "--data-dir",
           SystemConfiguration::instance().data_dir().string(),
           "--source", mapper_source.string()});
      if (result != EXIT_SUCCESS)
        WARNING("Guest gfxstream mapper compatibility overlay was not applied");
    } catch (const std::exception& error) {
      WARNING("Could not run guest gfxstream compatibility tool: %s",
              error.what());
    }
  }

  const auto data_image = selected_data_image();
  if (!data_image.empty()) {
    const auto data_mountpoint =
        SystemConfiguration::instance().data_dir() / "data";
    if (!fs::exists(data_mountpoint))
      fs::create_directories(data_mountpoint);

    std::shared_ptr<common::LoopDevice> data_loop;
    try {
      data_loop = common::LoopDeviceAllocator::new_device();
    } catch (const std::exception &error) {
      ERROR("Could not allocate managed data loop device: %s", error.what());
      release_mounts();
      return false;
    }
    if (!data_loop->attach_file(data_image, false)) {
      ERROR("Failed to attach managed Android data image read-write");
      release_mounts();
      return false;
    }
    auto data_mount = common::MountEntry::create(
        data_loop, data_mountpoint, "ext4",
        MS_NOSUID | MS_NODEV | MS_NOATIME | MS_PRIVATE);
    if (!data_mount) {
      ERROR("Failed to mount managed Android data image");
      release_mounts();
      return false;
    }
    mounts_.push_back(data_mount);
  }


  for (const auto &dir_name : std::vector<std::string>{"cache", "data"}) {
    auto target_dir_path = fs::path(final_android_rootfs_dir) / dir_name;
    auto src_dir_path = SystemConfiguration::instance().data_dir() / dir_name;

    if (!fs::exists(src_dir_path)) {
      if (!fs::create_directory(src_dir_path)) {
        ERROR("Failed to create Android %s directory", dir_name);
        release_mounts();
        return false;
      }
      if (::chown(src_dir_path.c_str(), unprivileged_user_id, unprivileged_user_id) != 0) {
        ERROR("Failed to allow access for unprivileged user on %s directory of the rootfs", dir_name);
        release_mounts();
        return false;
      }
    }

    auto m = common::MountEntry::create(src_dir_path, target_dir_path, "",
                                        MS_BIND | MS_PRIVATE);
    if (!m) {
      ERROR("Failed to mount Android %s directory", dir_name);
      release_mounts();
      return false;
    }
    mounts_.push_back(m);
  }

  // Unmounting needs to happen in reverse order
  std::reverse(mounts_.begin(), mounts_.end());

  return true;
}

bool anbox::cmds::ContainerManager::setup_gsi_rootfs_shell(
    const fs::path &source_rootfs) {
  const auto shell_rootfs =
      fs::path(SystemConfiguration::instance().combined_rootfs_dir());
  if (utils::is_mounted(shell_rootfs.string())) {
    ERROR("GSI rootfs shell is already mounted");
    release_mounts();
    return false;
  }

  if (!fs::exists(shell_rootfs))
    fs::create_directories(shell_rootfs);

  // The shell is generated state. Keep the immutable GSI mounted read-only,
  // but provide a writable root mount for Android's mount namespace setup.
  for (fs::directory_iterator entry{shell_rootfs}, end; entry != end; ++entry) {
    try {
      fs::remove_all(entry->path());
    } catch (const fs::filesystem_error &error) {
      if (error.code().value() != EBUSY)
        throw;
      WARNING("Removal found busy GSI path %s; detaching and retrying",
              entry->path());
      if (!safe_umount(entry->path()))
        throw;
      fs::remove_all(entry->path());
    }
  }

  for (fs::directory_iterator entry{source_rootfs}, end; entry != end; ++entry) {
    const auto source = entry->path();
    const auto target = shell_rootfs / source.filename();
    struct stat st {};
    if (::lstat(source.c_str(), &st) != 0) {
      ERROR("Failed to inspect GSI root entry %s: %s", source,
            strerror(errno));
      release_mounts();
      return false;
    }

    if (S_ISLNK(st.st_mode)) {
      fs::create_symlink(fs::read_symlink(source), target);
    } else if (S_ISDIR(st.st_mode)) {
      fs::create_directory(target);
    } else if (S_ISREG(st.st_mode)) {
      fs::copy_file(source, target);
    } else {
      WARNING("Skipping unsupported GSI root entry %s", source);
      continue;
    }

    if (::lchown(target.c_str(), st.st_uid, st.st_gid) != 0) {
      ERROR("Failed to assign GSI root entry %s: %s", target,
            strerror(errno));
      release_mounts();
      return false;
    }
    if (!S_ISLNK(st.st_mode) && ::chmod(target.c_str(), st.st_mode & 07777) != 0) {
      ERROR("Failed to set GSI root entry permissions on %s: %s", target,
            strerror(errno));
      release_mounts();
      return false;
    }
  }

  const auto source_system = source_rootfs / "system";
  const auto target_system = shell_rootfs / "system";
  auto system_mount = common::MountEntry::create(
      source_system, target_system, "", MS_BIND | MS_PRIVATE);
  if (!system_mount) {
    ERROR("Failed to expose immutable GSI /system in rootfs shell");
    release_mounts();
    return false;
  }
  mounts_.push_back(system_mount);

  // First-stage init normally establishes these mountpoints. Anbox Reboxed enters the
  // GSI at second stage, so make them distinct mounts before Android changes
  // their propagation modes.
  for (const auto *name : {"apex", "linkerconfig"}) {
    const auto path = shell_rootfs / name;
    auto mountpoint = common::MountEntry::create(
        path, path, "", MS_BIND | MS_PRIVATE);
    if (!mountpoint) {
      ERROR("Failed to create GSI /%s mountpoint", name);
      release_mounts();
      return false;
    }
    mounts_.push_back(mountpoint);
  }

  INFO("Prepared writable GSI root shell around immutable /system");
  return true;
}

bool anbox::cmds::ContainerManager::setup_rootfs_overlay() {
  const auto combined_rootfs_path = SystemConfiguration::instance().combined_rootfs_dir();
  if (!fs::exists(combined_rootfs_path))
    fs::create_directories(combined_rootfs_path);

  const auto overlay_path = SystemConfiguration::instance().overlay_dir();
  if (!fs::exists(overlay_path))
    fs::create_directories(overlay_path);

  const auto upper_path = fs::path(overlay_path) / "upper";
  const auto work_path = fs::path(overlay_path) / "work";
  if (!fs::exists(upper_path))
    fs::create_directories(upper_path);
  if (!fs::exists(work_path))
    fs::create_directories(work_path);

  const auto rootfs_path = SystemConfiguration::instance().rootfs_dir();
  const auto overlay_config =
      utils::string_format("lowerdir=%s,upperdir=%s,workdir=%s",
                           rootfs_path, upper_path.string(),
                           work_path.string());
  auto m = common::MountEntry::create("overlay", combined_rootfs_path,
                                      "overlay", 0,
                                      overlay_config.c_str());
  if (!m) {
    ERROR("Failed to setup rootfs overlay");
    release_mounts();
    return false;
  }
  mounts_.push_back(m);

  DEBUG("Successfully setup rootfs overlay");
  return true;
}
