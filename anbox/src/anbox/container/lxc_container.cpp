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

#include "anbox/android/ip_config_builder.h"
#include "anbox/common/binder_device_allocator.h"
#include "anbox/common/binder_device.h"
#include "anbox/container/lxc_container.h"
#include "anbox/system_configuration.h"
#include "anbox/memory_manager.h"
#include "anbox/logger.h"
#include "anbox/utils.h"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/throw_exception.hpp>

#include <linux/loop.h>
#include <sys/capability.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <unistd.h>

namespace fs = boost::filesystem;

namespace {
constexpr unsigned int unprivileged_uid{100000};
constexpr unsigned int android_system_uid{1000};
constexpr const char *default_container_ip_address{"192.168.250.2"};
constexpr const std::uint32_t default_container_ip_prefix_length{24};
constexpr const char *default_host_ip_address{"192.168.250.1"};
constexpr const char *default_dns_server{"8.8.8.8"};
constexpr int num_needed_binders{3};
constexpr int first_apex_loop{2};
// Android 15 requires 42 idle APEX loops. The host can already occupy loop0
// through loop3 for the system/vendor images and setup, so expose loop2 through
// loop45 inclusive to leave 42 idle devices inside the container.
constexpr int last_apex_loop{45};

struct SignatureSpoofOverlay {
  bool enabled{false};
  fs::path services_jar;
  fs::path system_app_dir;
  fs::path init_rc;
  std::string generation;
};

bool is_secure_root_owned_path(const fs::path &path, mode_t expected_type) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == expected_type && st.st_uid == 0 &&
         (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

SignatureSpoofOverlay selected_signature_spoof_overlay() {
  SignatureSpoofOverlay overlay;
  const auto root = anbox::SystemConfiguration::instance().data_dir() /
                    "state/signature-spoof";
  const auto marker = root / "enabled";
  if (!fs::exists(marker))
    return overlay;
  if (!is_secure_root_owned_path(marker, S_IFREG))
    throw std::runtime_error("Signature-spoof marker is not a secure root-owned file");

  std::ifstream input{marker.string()};
  if (!input.is_open() || !std::getline(input, overlay.generation))
    throw std::runtime_error("Failed to read the signature-spoof generation marker");
  const std::string prefix{"android-15-reboxed-v1-"};
  if (overlay.generation.size() != prefix.size() + 16 + 1 + 8 ||
      overlay.generation.compare(0, prefix.size(), prefix) != 0 ||
      overlay.generation[prefix.size() + 16] != '-') {
    throw std::runtime_error("Invalid signature-spoof generation marker");
  }
  for (std::size_t index = prefix.size(); index < overlay.generation.size(); ++index) {
    if (index == prefix.size() + 16)
      continue;
    const char value = overlay.generation[index];
    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
      throw std::runtime_error("Invalid signature-spoof generation marker");
  }

  const auto generation = root / "generations" / overlay.generation;
  overlay.services_jar = generation / "services.jar";
  overlay.system_app_dir = generation / "system-app";
  overlay.init_rc = generation / "reboxed-signature-spoof.rc";
  if (!is_secure_root_owned_path(generation, S_IFDIR) ||
      !is_secure_root_owned_path(overlay.services_jar, S_IFREG) ||
      !is_secure_root_owned_path(overlay.system_app_dir, S_IFDIR) ||
      !is_secure_root_owned_path(
          overlay.system_app_dir / "ReboxedSignatureSpoofPermission.apk", S_IFREG) ||
      !is_secure_root_owned_path(overlay.init_rc, S_IFREG)) {
    throw std::runtime_error(
        "Enabled signature-spoof generation is insecure or incomplete");
  }
  overlay.enabled = true;
  return overlay;
}

void migrate_legacy_credential_encrypted_app_data() {
  const auto data_root =
      anbox::SystemConfiguration::instance().data_dir() / "data";
  const auto legacy_root = data_root / "data";
  const auto canonical_root = data_root / "user/0";

  if (!fs::is_directory(legacy_root))
    return;

  fs::create_directories(canonical_root);
  for (fs::directory_iterator it{legacy_root}, end; it != end; ++it) {
    const auto destination = canonical_root / it->path().filename();
    if (fs::exists(destination))
      continue;

    // Both paths live below the same persistent /data backing store. Rename
    // preserves uid/gid, modes, xattrs, timestamps, and all app contents.
    // Existing canonical directories are deliberately never merged or
    // overwritten.
    INFO("Migrating legacy Android app data '%s' to '%s'",
         it->path().string(), destination.string());
    fs::rename(it->path(), destination);
  }
}

constexpr const char *compat_apexd_init{
    "# Container-only replacement for Android's stock apexd service file.\n"
    "# Explicit labels avoid policy transition lookup when SELinux is disabled.\n"
    "service apexd /system/bin/apexd\n"
    "    interface aidl apexservice\n"
    "    class core\n"
    "    user root\n"
    "    group system\n"
    "    oneshot\n"
    "    disabled\n"
    "    reboot_on_failure reboot,apexd-failed\n"
    "    capabilities CHOWN DAC_OVERRIDE DAC_READ_SEARCH FOWNER SYS_ADMIN\n"
    "    seclabel u:r:init:s0\n"
    "\n"
    "service apexd-bootstrap /system/bin/apexd --bootstrap\n"
    "    user root\n"
    "    group system\n"
    "    oneshot\n"
    "    disabled\n"
    "    reboot_on_failure reboot,bootloader,bootstrap-apexd-failed\n"
    "    capabilities SYS_ADMIN\n"
    "    seclabel u:r:init:s0\n"
    "\n"
    "service apexd-snapshotde /system/bin/apexd --snapshotde\n"
    "    user root\n"
    "    group system\n"
    "    oneshot\n"
    "    disabled\n"
    "    capabilities CHOWN DAC_OVERRIDE DAC_READ_SEARCH FOWNER\n"
    "    seclabel u:r:init:s0\n"};
constexpr const char *compat_bpf_init{
    "# Run the real Android 15 loader before netd. The executable is an exact\n"
    "# APEX copy with only its LTS-kernel policy predicate relaxed.\n"
    "on load_bpf_programs\n"
    "    exec_start bpfloader\n"
    "\n"
    "service bpfloader /system/bin/netutils-wrapper-1.0\n"
    "    capabilities CHOWN DAC_OVERRIDE SYS_ADMIN NET_ADMIN\n"
    "    group root graphics network_stack net_admin net_bw_acct net_bw_stats net_raw system\n"
    "    user root\n"
    "    file /dev/kmsg w\n"
    "    rlimit memlock 1073741824 1073741824\n"
    "    oneshot\n"
    "    reboot_on_failure reboot,bpfloader-failed\n"
    "    seclabel u:r:init:s0\n"};
constexpr const char *compat_derive_classpath_init{
    "# Container-only early definition for the sdkext APEX service.\n"
    "# The wrapper runs the platform tool, then injects Anbox Reboxed bootclasspath\n"
    "# compatibility stubs before init imports /data/system/environ/classpath.\n"
    "service derive_classpath /vendor/bin/anbox_derive_classpath\n"
    "    user system\n"
    "    group system reserved_disk\n"
    "    oneshot\n"
    "    disabled\n"
    "    seclabel u:r:init:s0\n"};
constexpr const char *compat_extservices_apex_init{
    "# Container-only APEX compatibility for Android 15 GSI.\n"
    "# apexd leaves com.android.extservices inactive in this container, but\n"
    "# PackageManager requires the real android.ext.services package.\n"
    "on post-fs-data\n"
    "    mkdir /apex/com.android.extservices 0755 root root\n"
    "    mount none /system/apex/com.android.extservices.apex /apex/com.android.extservices bind ro\n"};
constexpr const char *compat_graphics_init{
    "# Container-only early definitions for the API 35 ranchu graphics stack.\n"
    "# Vendor init files are imported after class core starts in second-stage mode.\n"
    "on early-init\n"
    "    start vendor.hwservicemanager\n"
    "\n"
    "on post-fs-data\n"
    "    mkdir /data/anbox 0755 system system\n"
    "    mkdir /data/data 0711 system system\n"
    "    mkdir /data/user/0 0711 system system\n"
    "    mount none /data/user/0 /data/data bind\n"
    "    mkdir /data/user_de/0 0711 system system\n"
    "    mkdir /data/user/0/com.android.providers.telephony 0700 radio radio\n"
    "    mkdir /data/user/0/com.android.providers.telephony/databases 0700 radio radio\n"
    "    mkdir /data/user_de/0/com.android.providers.telephony 0700 radio radio\n"
    "    mkdir /data/user_de/0/com.android.providers.telephony/databases 0700 radio radio\n"
    "    mkdir /data/system_ce/0 0770 system system\n"
    "    mkdir /data/system_de/0 0770 system system\n"
    "    mkdir /data/misc_ce/0 0771 system misc\n"
    "    mkdir /data/misc_de/0 0771 system misc\n"
    "    mkdir /data/misc/profiles 0771 system system\n"
    "    mkdir /data/misc/profiles/cur 0771 system system\n"
    "    mkdir /data/misc/profiles/cur/0 0771 system system\n"
    "    mkdir /data/misc/profiles/ref 0771 system system\n"
    "    chmod 0750 /mnt/user\n"
    "    chown root media_rw /mnt/user\n"
    "    chmod 0710 /mnt/user/0\n"
    "    chown shell everybody /mnt/user/0\n"
    "    chmod 0700 /mnt/user/0/emulated\n"
    "    chown root root /mnt/user/0/emulated\n"
    "    chmod 0710 /mnt/pass_through/0\n"
    "    chown root media_rw /mnt/pass_through/0\n"
    "    chmod 0710 /mnt/pass_through/0/emulated\n"
    "    chown root media_rw /mnt/pass_through/0/emulated\n"
    "    mkdir /mnt/runtime/default/emulated 0700 root root\n"
    "    mkdir /mnt/runtime/read/emulated 0700 root root\n"
    "    mkdir /mnt/runtime/write/emulated 0700 root root\n"
    "    mkdir /mnt/runtime/full/emulated 0700 root root\n"
    "    chmod 0700 /mnt/runtime/default/emulated\n"
    "    chmod 0700 /mnt/runtime/read/emulated\n"
    "    chmod 0700 /mnt/runtime/write/emulated\n"
    "    chmod 0700 /mnt/runtime/full/emulated\n"
    "    chown root root /mnt/runtime/default/emulated\n"
    "    chown root root /mnt/runtime/read/emulated\n"
    "    chown root root /mnt/runtime/write/emulated\n"
    "    chown root root /mnt/runtime/full/emulated\n"
    "    chmod 0755 /data/anbox/libanbox_pipe_compat.so\n"
    "    chmod 0755 /data/anbox/libanbox_pipe_compat32.so\n"
    "    start installd\n"
    "\n"
    "# ART APEX init.rc is not imported by the direct second-stage path. Keep\n"
    "# this declaration synchronized with /apex/com.android.art/etc/init.rc so\n"
    "# PackageManager can resolve the lazy aidl/artd interface for dexopt.\n"
    "service artd /apex/com.android.art/bin/artd\n"
    "    interface aidl artd\n"
    "    disabled\n"
    "    oneshot\n"
    "    class core\n"
    "    user artd\n"
    "    group artd\n"
    "    capabilities DAC_OVERRIDE DAC_READ_SEARCH FOWNER CHOWN\n"
    "    seclabel u:r:init:s0\n"
    "\n"
    "on boot\n"
    "    start artd\n"
    "    start gatekeeperd\n"
    "    start storaged\n"
    "    start media\n"
    "    start mediaextractor\n"
    "    start anbox-media-swcodec\n"
    "    start mediametrics\n"
    "    start statsd\n"
    "\n"
    "# This container image has no interactive Setup Wizard. Mark its primary\n"
    "# user provisioned once framework services are ready; otherwise Android 15\n"
    "# Settings deliberately finishes its launch and returns to Home.\n"
    "on property:sys.boot_completed=1\n"
    "    exec_background u:r:init:s0 root root -- /system/bin/settings put global device_provisioned 1\n"
    "    exec_background u:r:init:s0 root root -- /system/bin/settings put secure user_setup_complete 1\n"
    "\n"
    "service vendor.hwservicemanager /system/system_ext/bin/hwservicemanager\n"
    "    class core animation\n"
    "    user system\n"
    "    group system readproc\n"
    "    seclabel u:r:init:s0\n"
    "    onrestart setprop hwservicemanager.ready false\n"
    "    onrestart class_restart --only-enabled main\n"
    "    onrestart class_restart --only-enabled hal\n"
    "\n"
    "# The media.swcodec APEX rc is not imported by the container's direct\n"
    "# second-stage init path. Codec2's goldfish mapper also needs the same\n"
    "# qemu-pipe compatibility shim used by the graphics services. Without it,\n"
    "# MediaProvider blocks during startup and emulated storage never mounts.\n"
    "service anbox-media-swcodec /apex/com.android.media.swcodec/bin/mediaswcodec\n"
    "    class main\n"
    "    user mediacodec\n"
    "    group camera drmrpc mediadrm\n"
    "    ioprio rt 4\n"
    "    setenv LD_LIBRARY_PATH /data/anbox\n"
    "    setenv LD_PRELOAD libanbox_pipe_compat.so\n"
    "    seclabel u:r:init:s0\n"
    "\n"
    "service vendor.gralloc-3-0 /vendor/bin/hw/android.hardware.graphics.allocator@3.0-service.ranchu\n"
    "    interface android.hardware.graphics.allocator@3.0::IAllocator default\n"
    "    class hal animation\n"
    "    user system\n"
    "    group graphics drmrpc\n"
    "    capabilities SYS_NICE\n"
    "    setenv LD_PRELOAD /vendor/lib64/libanbox_pipe_compat.so\n"
    "    seclabel u:r:init:s0\n"
    "    onrestart restart surfaceflinger\n"
    "\n"
    "service vendor.graphics-composer-3 /vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu\n"
    "    interface aidl android.hardware.graphics.composer3.IComposer/default\n"
    "    class hal animation\n"
    "    user system\n"
    "    group graphics drmrpc\n"
    "    capabilities SYS_NICE\n"
    "    setenv LD_PRELOAD /vendor/lib64/libanbox_pipe_compat.so\n"
    "    seclabel u:r:init:s0\n"
    "    onrestart restart surfaceflinger\n"
    "\n"
    "service statsd /apex/com.android.os.statsd/bin/statsd\n"
    "    override\n"
    "    class main\n"
    "    socket statsdw dgram+passcred 0222 statsd statsd\n"
    "    user statsd\n"
    "    group statsd log\n"
    "    task_profiles ProcessCapacityHigh HighEnergySaving\n"
    "    seclabel u:r:init:s0\n"};
constexpr const char *compat_security_hal_init{
    "# Container-only early definitions for the API 35 software security HAL stack.\n"
    "# Keystore2 starts early and needs KeyMint visible before late vendor imports.\n"
    "on early-init\n"
    "    start vendor.keymint-default\n"
    "    start vendor.gatekeeper-1-0\n"
    "\n"
    "service vendor.keymint-default /vendor/bin/hw/android.hardware.security.keymint-service\n"
    "    interface aidl android.hardware.security.keymint.IKeyMintDevice/default\n"
    "    interface aidl android.hardware.security.keymint.IRemotelyProvisionedComponent/default\n"
    "    interface aidl android.hardware.security.secureclock.ISecureClock/default\n"
    "    interface aidl android.hardware.security.sharedsecret.ISharedSecret/default\n"
    "    class hal\n"
    "    user nobody\n"
    "    seclabel u:r:init:s0\n"
    "\n"
    "service vendor.gatekeeper-1-0 /vendor/bin/hw/android.hardware.gatekeeper@1.0-service.software\n"
    "    interface android.hardware.gatekeeper@1.0::IGatekeeper default\n"
    "    class hal\n"
    "    user system\n"
    "    group system\n"
    "    seclabel u:r:init:s0\n"};

void copy_framework_entry(const fs::path &source, const fs::path &destination,
                          bool inside_x86) {
  const auto filename = source.filename().string();
  if (inside_x86 && filename.rfind("boot-framework-adservices.", 0) == 0)
    return;

  const auto symlink_status = fs::symlink_status(source);
  if (fs::is_symlink(symlink_status)) {
    fs::create_symlink(fs::read_symlink(source), destination);
    return;
  }

  if (fs::is_directory(symlink_status)) {
    fs::create_directories(destination);
    const bool child_inside_x86 = inside_x86 || filename == "x86";
    for (fs::directory_iterator it{source}, end; it != end; ++it) {
      copy_framework_entry(it->path(), destination / it->path().filename(),
                           child_inside_x86);
    }
    return;
  }

  if (!fs::is_regular_file(symlink_status))
    return;

  boost::system::error_code hardlink_error;
  fs::create_hard_link(source, destination, hardlink_error);
  if (hardlink_error) {
    fs::copy_file(source, destination,
                  fs::copy_options::overwrite_existing);
  }
}

void create_framework_view(const fs::path &rootfs_path,
                           const fs::path &target_dir,
                           const fs::path &stub_jar,
                           const fs::path &patched_services_jar) {
  const auto source_dir = rootfs_path / "system/framework";
  if (!fs::is_directory(source_dir) ||
      !fs::exists(source_dir / "x86/boot-framework-adservices.art"))
    return;

  fs::remove_all(target_dir);
  fs::create_directories(target_dir);

  for (fs::directory_iterator it{source_dir}, end; it != end; ++it)
    copy_framework_entry(it->path(), target_dir / it->path().filename(),
                         false);

  if (fs::is_regular_file(stub_jar))
    fs::copy_file(stub_jar,
                  target_dir / "anbox-ondevicepersonalization-stub.jar",
                  fs::copy_options::overwrite_existing);

  if (!patched_services_jar.empty()) {
    // The view contains hard links to the immutable mounted image. Unlink the
    // target before copying so replacing services.jar cannot modify that image.
    fs::remove(target_dir / "services.jar");
    fs::copy_file(patched_services_jar, target_dir / "services.jar");

    // The stock x86_64 OAT/VDEX files were compiled from the unmodified DEX.
    // Removing only services.* from this private view makes ART consume the
    // patched DEX and place any regenerated artifacts under persistent /data.
    std::vector<fs::path> stale_artifacts;
    const auto oat_root = target_dir / "oat";
    if (fs::is_directory(oat_root)) {
      for (fs::recursive_directory_iterator it{oat_root}, end; it != end; ++it) {
        if (fs::is_regular_file(it->path()) &&
            it->path().filename().string().rfind("services.", 0) == 0) {
          stale_artifacts.push_back(it->path());
        }
      }
    }
    for (const auto &artifact : stale_artifacts)
      fs::remove(artifact);
  }

  if (::chmod(target_dir.c_str(), 0755) != 0)
    throw std::runtime_error(anbox::utils::string_format(
        "Failed to set framework compatibility directory permissions: %s",
        strerror(errno)));
}

void create_system_app_view(const fs::path &rootfs_path,
                            const fs::path &target_dir,
                            const fs::path &permission_app_dir) {
  const auto source_dir = rootfs_path / "system/app";
  if (!fs::is_directory(source_dir) || !fs::is_directory(permission_app_dir))
    return;

  fs::remove_all(target_dir);
  fs::create_directories(target_dir);
  for (fs::directory_iterator it{source_dir}, end; it != end; ++it)
    copy_framework_entry(it->path(), target_dir / it->path().filename(), false);

  const auto destination = target_dir / "ReboxedSignatureSpoofPermission";
  fs::create_directories(destination);
  fs::copy_file(
      permission_app_dir / "ReboxedSignatureSpoofPermission.apk",
      destination / "ReboxedSignatureSpoofPermission.apk");
  if (::chmod(target_dir.c_str(), 0755) != 0)
    throw std::runtime_error(anbox::utils::string_format(
        "Failed to set system app compatibility directory permissions: %s",
        strerror(errno)));
}

void add_container_service_labels(
    const fs::path &source_dir, const fs::path &target_dir,
    const fs::path &state_dir,
    std::unordered_map<std::string, std::string> &bind_mounts,
    const fs::path &signature_spoof_init) {
  if (!fs::is_directory(source_dir))
    return;

  for (fs::recursive_directory_iterator it{source_dir}, end; it != end; ++it) {
    if (!fs::is_regular_file(it->path()) || it->path().extension() != ".rc")
      continue;
    if (it->path().filename() == "apexd.rc" ||
        it->path().filename() == "netbpfload.rc")
      continue;

    std::ifstream input{it->path().string()};
    if (!input.is_open())
      throw std::runtime_error(
          anbox::utils::string_format("Failed to read init file %s",
                                      it->path().string()));

    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);)
      lines.push_back(line);

    bool changed = false;
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      auto line = lines[index];
      const auto first = line.find_first_not_of(" \t");
      if (first != std::string::npos) {
        if (it->path() == source_dir / "hw/init.rc" &&
            line.compare(first, std::string::npos,
                         "mount fusectl none /sys/fs/fuse/connections") ==
                0) {
          line.replace(first, std::string::npos,
                       "# Anbox Reboxed: host fusectl connections are hidden");
          changed = true;
        }
        if (!fs::exists("/dev/ashmem") &&
            it->path() == source_dir / "hw/init.rc" &&
            line.compare(first, std::string::npos,
                         "setprop sys.use_memfd false") == 0) {
          line.replace(first, std::string::npos,
                       "setprop sys.use_memfd true");
          changed = true;
        }
        if (it->path().filename() == "init.zygote32.rc" &&
            line.compare(first, 8, "service ") == 0) {
          const std::string generic_app_process{"/system/bin/app_process "};
          const auto app_process = line.find(generic_app_process, first);
          if (app_process != std::string::npos) {
            line.replace(app_process, generic_app_process.size() - 1,
                         "/system/bin/app_process32");
            changed = true;
          }
        }
        if (line.find("/system/bin/vdc keymaster earlyBootEnded", first) !=
            std::string::npos) {
          line.replace(first, std::string::npos,
                       "exec u:r:init:s0 -- /system/bin/true");
          changed = true;
        }
        const std::array<std::pair<const char *, const char *>, 4>
            exec_replacements{{
                {"exec - ", "exec u:r:init:s0 "},
                {"exec -- ", "exec u:r:init:s0 -- "},
                {"exec_background - ", "exec_background u:r:init:s0 "},
                {"exec_background -- ",
                 "exec_background u:r:init:s0 -- "},
            }};
        for (const auto &replacement : exec_replacements) {
          const std::string prefix{replacement.first};
          if (line.compare(first, prefix.size(), prefix) == 0) {
            line.replace(first, prefix.size(), replacement.second);
            changed = true;
            break;
          }
        }
      }
      output << line << '\n';
      if (lines[index].compare(0, 8, "service ") != 0)
        continue;

      // A service executable and its arguments may span multiple physical
      // lines using a trailing backslash. Keep the seclabel out of that
      // declaration so the continuation arguments remain part of `service`.
      for (;;) {
        const auto last = lines[index].find_last_not_of(" \t");
        if (last == std::string::npos || lines[index][last] != '\\' ||
            index + 1 >= lines.size())
          break;
        output << lines[++index] << '\n';
      }

      const auto filename = it->path().filename();
      if (filename == "surfaceflinger.rc") {
        output << "    setenv LD_PRELOAD /data/anbox/libanbox_pipe_compat.so\n";
        changed = true;
      }
      if (filename == "installd.rc") {
        output << "    setenv LD_PRELOAD /data/anbox/libanbox_installd_selinux_compat.so\n";
        changed = true;
      }
      if (filename == "init.zygote64.rc" ||
          (filename == "init.zygote64_32.rc" &&
           lines[index].compare(0, 15, "service zygote ") == 0)) {
        output << "    setenv LD_PRELOAD /data/anbox/libanbox_ashmem_ioctl_compat.so:/data/anbox/libanbox_pipe_compat.so\n";
        changed = true;
      }
      if (filename == "init.zygote32.rc" ||
          (filename == "init.zygote64_32.rc" &&
           lines[index].compare(0, 25, "service zygote_secondary ") == 0)) {
        output << "    setenv LD_PRELOAD /data/anbox/libanbox_ashmem_ioctl_compat32.so:/data/anbox/libanbox_pipe_compat32.so\n";
        changed = true;
      }
      bool has_seclabel = false;
      for (std::size_t option = index + 1; option < lines.size(); ++option) {
        if (lines[option].empty())
          continue;
        if (lines[option][0] != ' ' && lines[option][0] != '\t')
          break;
        const auto first = lines[option].find_first_not_of(" \t");
        if (first != std::string::npos &&
            lines[option].compare(first, 9, "seclabel ") == 0) {
          has_seclabel = true;
          break;
        }
      }

      if (!has_seclabel) {
        output << "    seclabel u:r:init:s0\n";
        changed = true;
      }
    }

    if (it->path() == source_dir / "hw/init.rc") {
      output << '\n' << compat_derive_classpath_init
             << '\n' << compat_extservices_apex_init
             << '\n' << compat_graphics_init
             << '\n' << compat_security_hal_init;
      if (!signature_spoof_init.empty()) {
        std::ifstream signature_init{signature_spoof_init.string()};
        if (!signature_init.is_open())
          throw std::runtime_error("Failed to read managed signature-spoof init file");
        output << '\n' << signature_init.rdbuf();
      }
      changed = true;
    }

    if (!changed)
      continue;

    const auto source_prefix = source_dir.string() + "/";
    const auto relative =
        it->path().string().substr(source_prefix.size());
    const auto generated = state_dir / relative;
    fs::create_directories(generated.parent_path());
    std::ofstream generated_output{generated.string(),
                                   std::ios::out | std::ios::trunc};
    if (!generated_output.is_open())
      throw std::runtime_error(
          anbox::utils::string_format(
              "Failed to create init compatibility file %s",
              generated.string()));
    generated_output << output.str();
    if (!generated_output.good())
      throw std::runtime_error(
          anbox::utils::string_format(
              "Failed to write init compatibility file %s",
              generated.string()));
    generated_output.close();
    if (::chmod(generated.c_str(), 0644) != 0)
      throw std::runtime_error(anbox::utils::string_format(
          "Failed to set init compatibility file permissions: %s",
          strerror(errno)));

    bind_mounts.emplace(generated.string(),
                        (target_dir / relative).string());
  }
}

void create_selinux_disabled_servicemanager(
    const fs::path &source, const fs::path &destination) {
  std::ifstream input{source.string(), std::ios::in | std::ios::binary};
  if (!input.is_open())
    throw std::runtime_error("Failed to read GSI servicemanager");

  std::vector<unsigned char> image{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};

  const auto patch = [&image](std::size_t offset,
                              std::initializer_list<unsigned char> expected,
                              std::initializer_list<unsigned char> replacement) {
    if (expected.size() != replacement.size() ||
        offset + expected.size() > image.size() ||
        !std::equal(expected.begin(), expected.end(), image.begin() + offset)) {
      throw std::runtime_error(
          "Unsupported Android servicemanager binary for SELinux-disabled GSI mode");
    }
    std::copy(replacement.begin(), replacement.end(), image.begin() + offset);
  };

  // Android 15's Access implementation treats an unavailable SELinux status
  // page as fatal. In Anbox Reboxed GSI mode SELinux is intentionally disabled, so:
  // - skip Access's libselinux initialization after its object is initialized;
  // - allow service find/add/list operations without policy lookups.
  //
  // These offsets are guarded by exact instruction matching. A different GSI
  // fails closed instead of receiving an unverified binary patch.
  patch(0x84c1, {0x48, 0x8d, 0x35, 0x28, 0x01, 0x00, 0x00},
        {0xe9, 0x34, 0x00, 0x00, 0x00, 0x90, 0x90});
  patch(0x89b0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
        {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3});
  patch(0x8be0, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48},
        {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3});

  std::ofstream output{destination.string(),
                       std::ios::out | std::ios::binary | std::ios::trunc};
  if (!output.is_open())
    throw std::runtime_error("Failed to create container servicemanager");
  output.write(reinterpret_cast<const char *>(image.data()), image.size());
  if (!output.good())
    throw std::runtime_error("Failed to write container servicemanager");
  output.close();
  if (::chmod(destination.c_str(), 0755) != 0)
    throw std::runtime_error(anbox::utils::string_format(
        "Failed to set container servicemanager permissions: %s",
        strerror(errno)));
}

void extract_capex(const fs::path &source, const fs::path &destination) {
  const auto pid = ::fork();
  if (pid < 0)
    throw std::runtime_error("Failed to fork CAPEX extractor");

  if (pid == 0) {
    const int output =
        ::open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0 || ::dup2(output, STDOUT_FILENO) < 0)
      ::_exit(126);
    ::close(output);
    ::execl("/usr/bin/unzip", "unzip", "-p", source.c_str(),
            "original_apex", static_cast<char *>(nullptr));
    ::_exit(127);
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    throw std::runtime_error(anbox::utils::string_format(
        "Failed to extract original APEX from %s", source.string()));
  }
}

void create_lts_relaxed_netbpfload(const fs::path &apex,
                                   const fs::path &destination,
                                   const fs::path &state_dir) {
  const auto payload = state_dir / "com.android.tethering.payload.img";

  const auto run = [](const std::vector<std::string> &arguments,
                      int output_fd = -1) {
    const auto pid = ::fork();
    if (pid < 0)
      throw std::runtime_error("Failed to fork netbpfload extractor");
    if (pid == 0) {
      if (output_fd >= 0 && ::dup2(output_fd, STDOUT_FILENO) < 0)
        ::_exit(126);
      std::vector<char *> argv;
      argv.reserve(arguments.size() + 1);
      for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
      argv.push_back(nullptr);
      ::execv(argv[0], argv.data());
      ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
      throw std::runtime_error("Failed to extract Connectivity netbpfload");
  };

  const int payload_fd =
      ::open(payload.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (payload_fd < 0)
    throw std::runtime_error("Failed to create Connectivity APEX payload");
  run({"/usr/bin/unzip", "-p", apex.string(), "apex_payload.img"},
      payload_fd);
  ::close(payload_fd);

  run({"/usr/bin/debugfs", "-R",
       "dump /bin/netbpfload " + destination.string(), payload.string()});
  fs::remove(payload);

  std::fstream image{destination.string(),
                     std::ios::in | std::ios::out | std::ios::binary};
  if (!image.is_open())
    throw std::runtime_error("Failed to open extracted netbpfload");
  constexpr std::streamoff offset{0xa442};
  const unsigned char expected[]{0x41, 0x89, 0xc7, 0x84, 0xc0};
  const unsigned char replacement[]{0x41, 0xb7, 0x01, 0xb0, 0x01};
  unsigned char actual[sizeof(expected)]{};
  image.seekg(offset);
  image.read(reinterpret_cast<char *>(actual), sizeof(actual));
  if (!image || !std::equal(std::begin(expected), std::end(expected),
                            std::begin(actual)))
    throw std::runtime_error(
        "Unsupported Android netbpfload binary for LTS-policy patch");
  image.seekp(offset);
  image.write(reinterpret_cast<const char *>(replacement),
              sizeof(replacement));

  // Android V treats failure to write bpf_jit_enable as fatal. Some kernels
  // build BPF JIT support without exposing the runtime sysctl. Redirect only
  // this call through unused text padding which maps -ENOENT to success. The
  // helper still logs the failed open, while EACCES, EPERM, write errors, and
  // every subsequent real BPF operation keep their original behavior.
  constexpr std::streamoff jit_call_offset{0xaa20};
  const unsigned char expected_jit_call[]{0xe8, 0xeb, 0x5e, 0x00, 0x00};
  const unsigned char replacement_jit_call[]{0xe8, 0xdd, 0x5e, 0x00, 0x00};
  unsigned char actual_jit_call[sizeof(expected_jit_call)]{};
  image.seekg(jit_call_offset);
  image.read(reinterpret_cast<char *>(actual_jit_call),
             sizeof(actual_jit_call));
  if (!image ||
      !std::equal(std::begin(expected_jit_call),
                  std::end(expected_jit_call),
                  std::begin(actual_jit_call)))
    throw std::runtime_error(
        "Unsupported Android netbpfload binary for BPF JIT sysctl patch");

  constexpr std::streamoff jit_kallsyms_call_offset{0xaa42};
  const unsigned char expected_jit_kallsyms_call[]{
      0xe8, 0xc9, 0x5e, 0x00, 0x00};
  const unsigned char replacement_jit_kallsyms_call[]{
      0xe8, 0xbb, 0x5e, 0x00, 0x00};
  unsigned char actual_jit_kallsyms_call[
      sizeof(expected_jit_kallsyms_call)]{};
  image.seekg(jit_kallsyms_call_offset);
  image.read(reinterpret_cast<char *>(actual_jit_kallsyms_call),
             sizeof(actual_jit_kallsyms_call));
  if (!image ||
      !std::equal(std::begin(expected_jit_kallsyms_call),
                  std::end(expected_jit_kallsyms_call),
                  std::begin(actual_jit_kallsyms_call)))
    throw std::runtime_error(
        "Unsupported Android netbpfload binary for BPF JIT kallsyms "
        "sysctl patch");

  constexpr std::streamoff jit_stub_offset{0x10902};
  const unsigned char expected_jit_stub[]{
      0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
      0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
  const unsigned char replacement_jit_stub[]{
      0x50,                          // push rax (preserve call alignment)
      0xe8, 0x08, 0x00, 0x00, 0x00,  // call writeToFile
      0x59,                          // pop rcx (caller replaces ecx next)
      0x3c, 0xfe,                    // cmp al, low byte of -ENOENT
      0x75, 0x02,                    // jne return
      0x31, 0xc0,                    // xor eax, eax
      0xc3};                         // return
  unsigned char actual_jit_stub[sizeof(expected_jit_stub)]{};
  image.seekg(jit_stub_offset);
  image.read(reinterpret_cast<char *>(actual_jit_stub),
             sizeof(actual_jit_stub));
  if (!image ||
      !std::equal(std::begin(expected_jit_stub),
                  std::end(expected_jit_stub),
                  std::begin(actual_jit_stub)))
    throw std::runtime_error(
        "Unsupported Android netbpfload binary text padding for BPF JIT "
        "sysctl patch");

  image.seekp(jit_call_offset);
  image.write(reinterpret_cast<const char *>(replacement_jit_call),
              sizeof(replacement_jit_call));
  image.seekp(jit_kallsyms_call_offset);
  image.write(
      reinterpret_cast<const char *>(replacement_jit_kallsyms_call),
      sizeof(replacement_jit_kallsyms_call));
  image.seekp(jit_stub_offset);
  image.write(reinterpret_cast<const char *>(replacement_jit_stub),
              sizeof(replacement_jit_stub));
  image.close();
  if (::chmod(destination.c_str(), 0755) != 0)
    throw std::runtime_error(anbox::utils::string_format(
        "Failed to set patched netbpfload permissions: %s", strerror(errno)));
}

void create_decompressed_apex_view(const fs::path &source_dir,
                                   const fs::path &destination_dir) {
  std::ostringstream signature;
  for (fs::directory_iterator it{source_dir}, end; it != end; ++it) {
    if (fs::is_regular_file(it->path()))
      signature << it->path().filename().string() << ':'
                << fs::file_size(it->path()) << '\n';
  }

  const auto marker = destination_dir / ".anbox-apex-view";
  if (fs::is_directory(destination_dir) && fs::is_regular_file(marker)) {
    std::ifstream existing{marker.string()};
    std::ostringstream content;
    content << existing.rdbuf();
    if (content.str() == signature.str())
      return;
  }

  const fs::path temporary{destination_dir.string() + ".tmp"};
  fs::remove_all(temporary);
  fs::create_directories(temporary);

  for (fs::directory_iterator it{source_dir}, end; it != end; ++it) {
    if (!fs::is_regular_file(it->path()))
      continue;

    auto filename = it->path().filename();
    if (it->path().extension() == ".capex") {
      filename = fs::path(it->path().stem().string() + ".apex");
      extract_capex(it->path(), temporary / filename);
    } else {
      fs::copy_file(it->path(), temporary / filename,
                    fs::copy_options::overwrite_existing);
    }
  }

  std::ofstream complete{(temporary / ".anbox-apex-view").string(),
                         std::ios::out | std::ios::trunc};
  complete << signature.str();
  if (!complete.good())
    throw std::runtime_error("Failed to finalize decompressed APEX view");
  complete.close();

  fs::remove_all(destination_dir);
  fs::rename(temporary, destination_dir);
}

#ifdef ENABLE_LXC2_SUPPORT
constexpr const char *lxc_config_idmap_key{"lxc.id_map"};
constexpr const char *lxc_config_net_type_key{"lxc.network.type"};
constexpr const char *lxc_config_net_flags_key{"lxc.network.flags"};
constexpr const char *lxc_config_net_link_key{"lxc.network.link"};
constexpr const char *lxc_config_pty_max_key{"lxc.pts"};
constexpr const char *lxc_config_tty_max_key{"lxc.tty"};
constexpr const char *lxc_config_uts_name_key{"lxc.utsname"};
constexpr const char *lxc_config_tty_dir_key{"lxc.devttydir"};
constexpr const char *lxc_config_init_cmd_key{"lxc.init_cmd"};
constexpr const char *lxc_config_rootfs_path_key{"lxc.rootfs"};
constexpr const char *lxc_config_log_level_key{"lxc.loglevel"};
constexpr const char *lxc_config_log_file_key{"lxc.logfile"};
constexpr const char *lxc_config_apparmor_profile_key{"lxc.aa_profile"};
constexpr const char *lxc_config_devices_deny_key{"lxc.cgroup.devices.deny"};
constexpr const char *lxc_config_devices_allow_key{"lxc.cgroup.devices.allow"};
#else
constexpr const char *lxc_config_idmap_key{"lxc.idmap"};
constexpr const char *lxc_config_net_type_key{"lxc.net.0.type"};
constexpr const char *lxc_config_net_flags_key{"lxc.net.0.flags"};
constexpr const char *lxc_config_net_link_key{"lxc.net.0.link"};
constexpr const char *lxc_config_pty_max_key{"lxc.pty.max"};
constexpr const char *lxc_config_tty_max_key{"lxc.tty.max"};
constexpr const char *lxc_config_uts_name_key{"lxc.uts.name"};
constexpr const char *lxc_config_tty_dir_key{"lxc.tty.dir"};
constexpr const char *lxc_config_init_cmd_key{"lxc.init.cmd"};
constexpr const char *lxc_config_rootfs_path_key{"lxc.rootfs.path"};
constexpr const char *lxc_config_log_level_key{"lxc.log.level"};
constexpr const char *lxc_config_log_file_key{"lxc.log.file"};
constexpr const char *lxc_config_apparmor_profile_key{"lxc.apparmor.profile"};
constexpr const char *lxc_config_devices_deny_key{"lxc.cgroup2.devices.deny"};
constexpr const char *lxc_config_devices_allow_key{"lxc.cgroup2.devices.allow"};
#endif

} // namespace

namespace anbox::container {
LxcContainer::LxcContainer(bool privileged,
                           bool rootfs_overlay,
                           const std::string& container_network_address,
                           const std::string &container_network_gateway,
                           const std::vector<std::string> &container_network_dns_servers,
                           const network::Credentials &creds)
    : state_(State::inactive),
      container_(nullptr),
      privileged_(privileged),
      rootfs_overlay_(rootfs_overlay),
      container_network_address_(container_network_address),
      container_network_gateway_(container_network_gateway),
      container_network_dns_servers_(container_network_dns_servers),
      creds_(creds) {
  utils::ensure_paths({
      SystemConfiguration::instance().container_config_dir(),
      SystemConfiguration::instance().container_state_dir(),
      SystemConfiguration::instance().log_dir(),
  });
}

LxcContainer::~LxcContainer() {
  stop();
  if (container_)
    lxc_container_put(container_);
}

std::vector<std::string> get_id_map(uid_t uid, gid_t gid) {
  const auto base_id = unprivileged_uid;
  const auto max_id = 100000;
  std::vector<std::string> config;

  config.push_back(utils::string_format("u 0 %d %d", base_id, android_system_uid - 1));
  config.push_back(utils::string_format("g 0 %d %d", base_id, android_system_uid - 1));

  // We need to bind the user id for the one running the client side
  // process as he is the owner of various socket files we bind mount
  // into the container.
  config.push_back(utils::string_format("u %d %d 1", android_system_uid, uid));
  config.push_back(utils::string_format("g %d %d 1", android_system_uid, gid));

  config.push_back(utils::string_format("u %d %d %d", android_system_uid + 1,
                                                     base_id + android_system_uid + 1,
                                                     max_id - android_system_uid));
  config.push_back(utils::string_format("g %d %d %d", android_system_uid + 1,
                                                     base_id + android_system_uid + 1,
                                                     max_id - android_system_uid));
  return config;
}

bool should_mount_graphics_compatibility_bind(
    bool modern_gfxstream, const std::string &bind_mount) {
  // The replacement GLES encoders translate the Android 15 guest protocol for
  // the legacy host decoder. Modern gfxstream must retain its matching vendor
  // encoders instead.
  if (modern_gfxstream &&
      bind_mount.find("/state/vendor-gles-compat/") != std::string::npos)
    return false;

  // The mapper overlay removes the guest DMA-only upload dispatch. It is
  // required with both transports because Anbox does not expose a real
  // goldfish address-space mapping to the guest. In particular, do not split
  // it from the legacy GLES encoder overlay: the original mapper otherwise
  // calls lockAndWriteDma with the pipe shim's zero physical address.
  return true;
}

void LxcContainer::setup_id_map() {
  auto config = get_id_map(creds_.uid(), creds_.gid());
  for (std::string val : config)
    set_config_item(lxc_config_idmap_key, val);
}

void LxcContainer::setup_network() {
  if (!fs::exists("/sys/class/net/anbox0")) {
    WARNING("Anbox Reboxed bridge interface 'anbox0' doesn't exist. Network functionality will not be available");
    return;
  }

  set_config_item(lxc_config_net_type_key, "veth");
  set_config_item(lxc_config_net_flags_key, "up");
  set_config_item(lxc_config_net_link_key, "anbox0");

  // Instead of relying on DHCP we will give Android a static IP configuration
  // for the virtual ethernet interface LXC creates for us. This will be bridged
  // to the host and will allows us to have reliable network connectivity and
  // not depend on any other system service.

  android::IpConfigBuilder ip_conf;
  ip_conf.set_version(android::IpConfigBuilder::Version::Version2);
  ip_conf.set_assignment(android::IpConfigBuilder::Assignment::Static);

  std::string address = default_container_ip_address;
  std::uint32_t ip_prefix_length = default_container_ip_prefix_length;
  if (!container_network_address_.empty()) {
    auto tokens = utils::string_split(container_network_address_, '/');
    if (tokens.size() == 1 || tokens.size() == 2)
      address = tokens[0];
    if (tokens.size() == 2)
      ip_prefix_length = atoi(tokens[1].c_str());
  }
  ip_conf.set_link_address(address, ip_prefix_length);

  std::string gateway = default_host_ip_address;
  if (!container_network_gateway_.empty())
    gateway = container_network_gateway_;
  ip_conf.set_gateway(gateway);

  if (container_network_dns_servers_.size() > 0)
    ip_conf.set_dns_servers(container_network_dns_servers_);
  else
    ip_conf.set_dns_servers({default_dns_server});

  ip_conf.set_id(0);

  std::vector<std::uint8_t> buffer(512);
  common::BinaryWriter writer(buffer.begin(), buffer.end());
  const auto size = ip_conf.write(writer);

  const auto data_ethernet_path = fs::path("data") / "misc" / "ethernet";
  const auto ip_conf_dir = SystemConfiguration::instance().data_dir() / data_ethernet_path;
  if (!fs::exists(ip_conf_dir))
    fs::create_directories(ip_conf_dir);

  // We have to walk through the created directory hierachy now and
  // ensure the permissions are set correctly. Otherwise the Android
  // system will fail to boot as it isn't allowed to write anything
  // into these directories. As previous versions of Anbox Reboxed which were
  // published to our users did this incorrectly we need to check on
  // every startup if those directories are still owned by root and
  // if they are we move them over to the unprivileged user.
  auto path = SystemConfiguration::instance().data_dir();
  for (auto iter = data_ethernet_path.begin(); iter != data_ethernet_path.end(); iter++) {
    path /= *iter;

    struct stat st;
    if (stat(path.c_str(), &st) < 0) {
      WARNING("Cannot retrieve permissions of path %s", path);
      continue;
    }

    if (st.st_uid != 0 && st.st_gid != 0)
      continue;

    if (::chown(path.c_str(), unprivileged_uid, unprivileged_uid) < 0)
      WARNING("Failed to set owner for path '%s'", path);
  }

  const auto ip_conf_path = ip_conf_dir / "ipconfig.txt";
  if (fs::exists(ip_conf_path))
    fs::remove(ip_conf_path);

  std::ofstream f(ip_conf_path.string(), std::ofstream::binary);
  if (f.is_open()) {
    f.write(reinterpret_cast<const char*>(buffer.data()), size);
    f.close();
  } else {
    ERROR("Failed to write IP configuration. Network functionality will not be available.");
  }
}

void LxcContainer::add_device(const std::string& device, const DeviceSpecification& spec) {
  struct stat st;
  const std::string *old_device_name;
  if (!spec.old_device_name.empty())
    old_device_name = &spec.old_device_name;
  else
    old_device_name = &device;
  int r = stat(old_device_name->c_str(), &st);
  if (r < 0) {
    const auto msg = utils::string_format("Failed to retrieve information about device %s", device);
    throw std::runtime_error(msg);
  }

  const auto device_major = major(st.st_rdev);
  const auto device_minor = minor(st.st_rdev);
  const auto mode = ((st.st_mode >> 9) << 9) | (spec.permission & ~(1 << 9));
  const auto new_device_name = fs::path(device).filename().string();
  const auto devices_path = fs::path(SystemConfiguration::instance().container_devices_dir());
  const auto new_device_path = (devices_path / new_device_name).string();

  const auto encoded_device_number = makedev(device_major, device_minor);
  r = mknod(new_device_path.c_str(), mode, encoded_device_number);
  if (r < 0) {
    auto msg = utils::string_format("Failed to create node for device %s: %s",
                                    device, strerror(errno));
    throw std::runtime_error(msg);
  }

  auto base_uid = unprivileged_uid;
  if (privileged_)
    base_uid = 0;

  const auto shifted_uid = base_uid + st.st_uid;
  const auto shifted_gid = base_uid + st.st_gid;
  r = chown(new_device_path.c_str(), shifted_uid, shifted_gid);
  if (r < 0) {
    auto msg = utils::string_format("Failed to change ownership of new node for %s: %s",
                                    device, strerror(errno));
    throw std::runtime_error(msg);
  }

  // Needed as mknod respects the umask
  r = chmod(new_device_path.c_str(), mode);
  if (r < 0) {
    auto msg = utils::string_format("Failed to change mode of new node for %s: %s",
                                    device, strerror(errno));
    throw::std::runtime_error(msg);
  }

  auto target_path = device;
  // Strip a leading slash as LXC doesn't like that
  if (utils::string_starts_with(device, "/"))
    target_path = device.substr(1, device.length() - 1);

  const auto entry = utils::string_format("%s %s none bind,create=file,optional 0 0",
                                          new_device_path, target_path);
  set_config_item("lxc.mount.entry", entry);
}

bool LxcContainer::create_binder_devices(unsigned int device_count, std::vector<std::unique_ptr<common::BinderDevice>>& devices) {
  // We will always allocate a static set of binders devices even if the container
  // doesn't use all of them
  for (unsigned int n = 0; n < device_count; n++) {
    auto device = common::BinderDeviceAllocator::new_device();
    if (!device)
      return false;

    DEBUG("Allocated binder device %s", device->path());
    devices.push_back(std::move(device));
  }

  return true;
}

void LxcContainer::start(const Configuration &configuration) {
  runtime_directories_to_restore_.clear();
  for (const auto &bind_mount : configuration.bind_mounts) {
    if (bind_mount.second == "/dev/input")
      runtime_directories_to_restore_.push_back(bind_mount.first);
  }

  if (getuid() != 0)
    throw std::runtime_error("You have to start the container as root");

  // Original Anbox exposed /data/data and /data/user/0 as unrelated host
  // directories. Android 15 and modern applications require them to name the
  // same credential-encrypted app-data tree. Preserve legacy-only app data
  // before container init bind-mounts the canonical directory over /data/data.
  migrate_legacy_credential_encrypted_app_data();

  if (container_ && container_->is_running(container_)) {
    WARNING("Container already started, stopping it now");
    container_->stop(container_);
  }

  if (!container_) {
    const auto container_config_dir = SystemConfiguration::instance().container_config_dir();
    DEBUG("Containers are stored in %s", container_config_dir);

    // Remove container config to be be able to rewrite it
    ::unlink(utils::string_format("%s/default/config", container_config_dir).c_str());

    container_ = lxc_container_new("default", container_config_dir.c_str());
    if (!container_)
      throw std::runtime_error("Failed to create LXC container instance");

    // If container is still running (for example after a crash) we stop it here
    // to ensure its configuration is synchronized.
    if (container_->is_running(container_))
      container_->stop(container_);
  }

  // We can mount proc/sys as rw here as we will run the container unprivileged
  // in the end
  set_config_item("lxc.mount.auto", "proc:mixed sys:mixed cgroup:mixed");

  set_config_item("lxc.autodev", "1");
#ifndef ENABLE_LXC2_SUPPORT
  // Android stores its property areas below /dev. Modern releases have enough
  // property contexts to exhaust smaller tmpfs mounts and fault valid shared
  // mappings with SIGBUS during PropertyInit.
  set_config_item("lxc.autodev.tmpfs.size", "134217728");
#endif
  set_config_item(lxc_config_pty_max_key, "1024");
  set_config_item(lxc_config_tty_max_key, "0");
  set_config_item(lxc_config_uts_name_key, "anbox");

  set_config_item(lxc_config_devices_deny_key, "");
  set_config_item(lxc_config_devices_allow_key, "");

  // We can't move bind-mounts, so don't use /dev/lxc/
  set_config_item(lxc_config_tty_dir_key, "");

  set_config_item("lxc.environment", "PATH=/system/bin:/system/sbin:/system/xbin");

  const auto memory_configuration = memory::load_configuration();
  memory::prepare_configured_swap(memory_configuration);
  memory::apply_lxc_limits(
      memory_configuration,
      [this](const std::string &key, const std::string &value) {
        set_config_item(key, value);
      });

  // Android's AlarmManager opens CLOCK_BOOTTIME_ALARM through timerfd_create().
  // Dropping CAP_WAKE_ALARM makes that syscall fail with EPERM, which forces
  // AlarmManager into its no-kernel-driver fallback path. That fallback only
  // knows how to send PendingIntent alarms and crashes on legitimate direct
  // IAlarmListener alarms where alarm.operation is intentionally null.
  //
  // Keep sys_boot dropped so Android cannot turn an init failure into an
  // unbounded LXC reboot loop, but leave wake_alarm available so the real
  // kernel alarm driver path is used.
  set_config_item("lxc.cap.drop", "sys_boot");

#ifdef ENABLE_SNAP_CONFINEMENT
  // If we're running inside the snap environment snap-confine already created a
  // cgroup for us we need to use as otherwise presevering a namespace wont help.
  if (utils::is_env_set("SNAP"))
    set_config_item("lxc.namespace.keep", "cgroup");
#endif

  auto rootfs_path = SystemConfiguration::instance().rootfs_dir();
  if (rootfs_overlay_)
    rootfs_path = SystemConfiguration::instance().combined_rootfs_dir();

  DEBUG("Using rootfs path %s", rootfs_path);
  set_config_item(lxc_config_rootfs_path_key, rootfs_path);

  const auto anbox_init_path = fs::path(rootfs_path) / "anbox-init.sh";
  const auto root_init_path = fs::path(rootfs_path) / "init";
  const auto android_init_path = fs::path(rootfs_path) / "system/bin/init";
  const bool modern_gsi =
      !fs::exists(anbox_init_path) && fs::exists(android_init_path);
  if (modern_gsi) {
    const fs::path ashmem_compat64{
        "/usr/local/lib/anbox/libanbox_ashmem_ioctl_compat.so"};
    const fs::path ashmem_compat32{
        "/usr/local/lib/anbox/libanbox_ashmem_ioctl_compat32.so"};
    const fs::path pipe_compat64{
        "/usr/local/lib/anbox/libanbox_pipe_compat.so"};
    const fs::path pipe_compat32{
        "/usr/local/lib/anbox/libanbox_pipe_compat32.so"};
    const fs::path installd_selinux_compat{
        "/usr/local/lib/anbox/libanbox_installd_selinux_compat.so"};
    if (!fs::is_regular_file(ashmem_compat64) ||
        !fs::is_regular_file(ashmem_compat32) ||
        !fs::is_regular_file(pipe_compat64) ||
        !fs::is_regular_file(pipe_compat32) ||
        !fs::is_regular_file(installd_selinux_compat))
      throw std::runtime_error(
          "Android 15 compatibility libraries are not installed");
    const fs::path compatibility_dir =
        SystemConfiguration::instance().data_dir() / "data/anbox";
    fs::create_directories(compatibility_dir);
    const auto install_compatibility_library =
        [&](const fs::path &source, const fs::path &destination) {
          fs::copy_file(source, destination,
                        fs::copy_options::overwrite_existing);
          if (::chmod(destination.c_str(), 0755) < 0)
            throw std::runtime_error(
                "Failed to set Android ashmem compatibility library mode");
        };
    install_compatibility_library(
        ashmem_compat64,
        compatibility_dir / "libanbox_ashmem_ioctl_compat.so");
    install_compatibility_library(
        ashmem_compat32,
        compatibility_dir / "libanbox_ashmem_ioctl_compat32.so");
    install_compatibility_library(
        pipe_compat64,
        compatibility_dir / "libanbox_pipe_compat.so");
    install_compatibility_library(
        pipe_compat32,
        compatibility_dir / "libanbox_pipe_compat32.so");
    install_compatibility_library(
        installd_selinux_compat,
        compatibility_dir / "libanbox_installd_selinux_compat.so");
  }
  auto extra_properties = configuration.extra_properties;
  const bool modern_gfxstream = std::find(
      configuration.extra_properties.begin(),
      configuration.extra_properties.end(),
      "ro.anbox.graphics_transport=gfxstream") !=
      configuration.extra_properties.end();
  const bool has_vendor_emulation_egl =
      fs::exists(fs::path(rootfs_path) /
                 "vendor/lib64/egl/libEGL_emulation.so");
  if (modern_gsi && has_vendor_emulation_egl) {
    extra_properties.push_back("ro.hardware.egl=emulation");
    extra_properties.push_back("persist.graphics.egl=emulation");
    extra_properties.push_back("ro.hardware.gralloc=ranchu");
    extra_properties.push_back("ro.kernel.qemu=1");
  } else if (modern_gsi &&
             fs::exists(fs::path(rootfs_path) /
                        "system/lib64/libEGL_angle.so")) {
    extra_properties.push_back("ro.hardware.egl=angle");
    extra_properties.push_back("persist.graphics.egl=angle");
  }
  if (modern_gsi) {
    // A generic system image expects the device/vendor configuration to
    // provide a Dalvik heap profile.  Without one ART falls back to a 16 MiB
    // growth limit, which is too small for system_server to finish user-unlock
    // broadcasts and provider installation.  Use Android's conventional
    // large-screen profile while retaining a bounded growth limit.
    extra_properties.push_back("dalvik.vm.heapstartsize=16m");
    extra_properties.push_back("dalvik.vm.heapgrowthlimit=256m");
    extra_properties.push_back("dalvik.vm.heapsize=512m");
    extra_properties.push_back("dalvik.vm.heaptargetutilization=0.75");
    extra_properties.push_back("dalvik.vm.heapminfree=512k");
    extra_properties.push_back("dalvik.vm.heapmaxfree=8m");

    // The API 35 software KeyMint implementation waits for these verified
    // boot properties while initializing its software attestation context.
    // Anbox Reboxed has no real verified boot chain, so provide explicit emulator
    // values instead of allowing KeyMint to block before Binder registration.
    extra_properties.push_back("ro.boot.verifiedbootstate=orange");
    extra_properties.push_back("ro.boot.vbmeta.device_state=unlocked");
    extra_properties.push_back(
        "ro.boot.vbmeta.digest=0000000000000000000000000000000000000000000000000000000000000000");
    extra_properties.push_back("ro.vendor.build.security_patch=2025-04-05");
  }
  if (modern_gsi && fs::exists("/dev/loop-control")) {
    const int loop_control =
        ::open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (loop_control < 0)
      throw std::runtime_error("Failed to open loop-control for APEX devices");

    for (int id = first_apex_loop; id <= last_apex_loop; ++id) {
      if (::ioctl(loop_control, LOOP_CTL_ADD, id) < 0 && errno != EEXIST) {
        const auto error = utils::string_format(
            "Failed to allocate APEX loop device %d: %s", id,
            strerror(errno));
        ::close(loop_control);
        throw std::runtime_error(error);
      }
    }
    ::close(loop_control);

    for (int id = first_apex_loop; id <= last_apex_loop; ++id) {
      const auto path = utils::string_format("/dev/loop%d", id);
      for (int attempt = 0; attempt < 50 && !fs::exists(path); ++attempt)
        ::usleep(20000);
      if (!fs::exists(path))
        throw std::runtime_error(
            utils::string_format("APEX loop device did not appear: %s", path));
    }
  }
  if (fs::exists(anbox_init_path)) {
    set_config_item(lxc_config_init_cmd_key, "/anbox-init.sh");
  } else if (fs::exists(android_init_path)) {
    WARNING("Android image has no /anbox-init.sh; starting modern GSI init in second-stage mode");
    set_config_item(lxc_config_init_cmd_key, "/system/bin/init second_stage");
  } else if (fs::exists(root_init_path) || fs::is_symlink(root_init_path)) {
    WARNING("Android image has no /anbox-init.sh or system init; starting /init");
    set_config_item(lxc_config_init_cmd_key, "/init");
  } else {
    throw std::runtime_error("Android image contains no supported init executable");
  }

  set_config_item(lxc_config_log_level_key, "0");
  const auto log_path = SystemConfiguration::instance().log_dir();
  set_config_item(lxc_config_log_file_key, utils::string_format("%s/container.log", log_path).c_str());

  // set RLIMIT_NICE to 1 so binder does not complain
  set_config_item("lxc.prlimit.nice", "1");

#ifndef ENABLE_LXC2_SUPPORT
    // Dump the console output to disk to have a chance to debug early boot problems
    set_config_item("lxc.console.logfile", utils::string_format("%s/console.log", log_path).c_str());
    set_config_item("lxc.console.rotate", "1");
#endif

  setup_network();

#ifdef ENABLE_SNAP_CONFINEMENT
  // We take the AppArmor profile snapd has defined for us as part of the
  // anbox-support interface. The container manager itself runs within a
  // child profile snap.anbox.container-manager//lxc too.
  set_config_item("lxc.apparmor.profile", "snap.anbox.container-manager//container");
#else
  set_config_item(lxc_config_apparmor_profile_key, "unconfined");
#endif

  if (!privileged_)
    setup_id_map();

  auto bind_mounts = configuration.bind_mounts;
  auto devices = configuration.devices;
  const auto container_root_uid = privileged_ ? 0 : unprivileged_uid;

  if (modern_gsi) {
    const auto state_dir =
        fs::path(SystemConfiguration::instance().container_state_dir());
    const auto signature_spoof = selected_signature_spoof_overlay();

    // vold_prepare_subdirs assumes security.selinux xattrs exist.  They cannot
    // exist on this SELinux-disabled host, so use the static compatibility
    // helper for prepare while retaining the stock binary for destroy.
    const auto vold_prepare_compat =
        fs::path("/usr/local/bin/vold_prepare_subdirs_anbox");
    const auto vold_prepare_stock =
        fs::path(rootfs_path) / "system/bin/vold_prepare_subdirs";
    if (!fs::is_regular_file(vold_prepare_compat))
      throw std::runtime_error("Missing vold_prepare_subdirs compatibility helper");
    bind_mounts.emplace(vold_prepare_compat.string(),
                        "/system/bin/vold_prepare_subdirs");
    bind_mounts.emplace(vold_prepare_stock.string(),
                        "/system/bin/vold_prepare_subdirs.anbox-real");

    auto ondevicepersonalization_stub =
        SystemConfiguration::instance().data_dir() /
        "vendor-rootfs/framework/anbox-ondevicepersonalization-stub.jar";
    if (!fs::is_regular_file(ondevicepersonalization_stub)) {
      ondevicepersonalization_stub =
          fs::path(rootfs_path) /
          "vendor/framework/anbox-ondevicepersonalization-stub.jar";
    }
    const auto framework_view = state_dir / "framework-with-anbox-compat";
    create_framework_view(fs::path(rootfs_path), framework_view,
                          ondevicepersonalization_stub,
                          signature_spoof.enabled
                              ? signature_spoof.services_jar
                              : fs::path{});
    if (fs::is_directory(framework_view)) {
      bind_mounts.emplace(framework_view.string(), "/system/framework");
    }
    if (signature_spoof.enabled) {
      INFO("Using controlled signature-spoof generation %s",
           signature_spoof.generation);
      const auto system_app_view =
          state_dir / "system-app-with-signature-compat";
      create_system_app_view(fs::path(rootfs_path), system_app_view,
                             signature_spoof.system_app_dir);
      bind_mounts.emplace(system_app_view.string(), "/system/app");
    }

    const auto metadata_dir =
        SystemConfiguration::instance().data_dir() / "metadata";
    fs::create_directories(metadata_dir / "apex/sessions");
    if (::chown(metadata_dir.c_str(), container_root_uid,
                container_root_uid) != 0 ||
        ::chmod(metadata_dir.c_str(), 0770) != 0) {
      throw std::runtime_error(utils::string_format(
          "Failed to prepare private /metadata: %s", strerror(errno)));
    }
    bind_mounts.emplace(metadata_dir.string(), "/metadata");

    const auto apex_view =
        SystemConfiguration::instance().data_dir() / "gsi-apex";
    create_decompressed_apex_view(
        fs::path(rootfs_path) / "system/apex", apex_view);
    bind_mounts.emplace(apex_view.string(), "/system/apex");

    const auto netbpfload_path = state_dir / "netbpfload_anbox";
    create_lts_relaxed_netbpfload(
        apex_view / "com.android.tethering.apex", netbpfload_path,
        state_dir);
    bind_mounts.emplace(netbpfload_path.string(),
                        "/system/bin/netutils-wrapper-1.0");

    const auto servicemanager_path = state_dir / "servicemanager_anbox";
    create_selinux_disabled_servicemanager(
        fs::path(rootfs_path) / "system/bin/servicemanager",
        servicemanager_path);
    bind_mounts.emplace(servicemanager_path.string(),
                        "/system/bin/servicemanager");

    const auto compat_apexd_path =
        state_dir / "apexd_anbox.rc";
    std::ofstream compat_apexd{compat_apexd_path.string(),
                               std::ios::out | std::ios::trunc};
    if (!compat_apexd.is_open())
      throw std::runtime_error("Failed to create APEX init compatibility file");
    compat_apexd << compat_apexd_init;
    if (!compat_apexd.good())
      throw std::runtime_error("Failed to write APEX init compatibility file");
    compat_apexd.close();
    bind_mounts.emplace(compat_apexd_path.string(),
                        "/system/etc/init/apexd.rc");

    const auto compat_bpf_path = state_dir / "netbpfload_anbox.rc";
    std::ofstream compat_bpf{compat_bpf_path.string(),
                             std::ios::out | std::ios::trunc};
    if (!compat_bpf.is_open())
      throw std::runtime_error("Failed to create BPF init compatibility file");
    compat_bpf << compat_bpf_init;
    if (!compat_bpf.good())
      throw std::runtime_error("Failed to write BPF init compatibility file");
    compat_bpf.close();
    bind_mounts.emplace(compat_bpf_path.string(),
                        "/system/etc/init/netbpfload.rc");

    // With SELinux disabled, Android init cannot calculate executable domain
    // transitions for services that omit an explicit seclabel. Generate
    // container-only copies of those rc files and run their services in init's
    // context. The immutable GSI remains untouched.
    add_container_service_labels(
        fs::path(rootfs_path) / "system/etc/init",
        fs::path("/system/etc/init"), state_dir / "init-compat/system",
        bind_mounts,
        signature_spoof.enabled ? signature_spoof.init_rc : fs::path{});
    const auto system_ext_init =
        fs::path(rootfs_path) / "system_ext/etc/init";
    const auto system_ext_bind_target =
        fs::is_symlink(fs::path(rootfs_path) / "system_ext")
            ? fs::path("/system/system_ext/etc/init")
            : fs::path("/system_ext/etc/init");
    add_container_service_labels(
        fs::is_directory(system_ext_init)
            ? system_ext_init
            : fs::path(rootfs_path) / "system/system_ext/etc/init",
        system_ext_bind_target,
        state_dir / "init-compat/system_ext", bind_mounts, fs::path{});

    const auto loop_mount_hook = state_dir / "mount-apex-loop-settings.sh";
    std::ofstream mount_hook{loop_mount_hook.string(),
                             std::ios::out | std::ios::trunc};
    if (!mount_hook.is_open())
      throw std::runtime_error("Failed to create APEX loop mount hook");
    mount_hook << "#!/bin/sh\nset -eu\n";
    // Android's 32-bit bionic stores owner tids in 16-bit pthread mutex fields.
    // On hosts with a large kernel.pid_max, 32-bit crash_dump processes abort
    // before they can report crashes:
    //
    //   32-bit pthread_mutex_t only supports pids <= 65535
    //
    // kernel.pid_max is scoped to the PID namespace on supported kernels.
    // LXC's built-in lxc.sysctl writer runs after /proc/sys has been remounted
    // read-only by proc:mixed, so set it from the post-mount hook while the
    // container proc tree is available through LXC_ROOTFS_MOUNT.
    mount_hook << "if ! /usr/bin/mount -o remount,rw "
                  "\"${LXC_ROOTFS_MOUNT}/proc/sys\"; then\n";
    mount_hook << "  echo \"Anbox Reboxed: failed to remount /proc/sys writable for "
                  "pid_max\" >&2\n";
    mount_hook << "  exit 1\n";
    mount_hook << "fi\n";
    mount_hook << "if ! echo 65535 > "
                  "\"${LXC_ROOTFS_MOUNT}/proc/sys/kernel/pid_max\"; then\n";
    mount_hook << "  echo \"Anbox Reboxed: failed to set container "
                  "kernel.pid_max=65535\" >&2\n";
    mount_hook << "  exit 1\n";
    mount_hook << "fi\n";
    // Starting a modern GSI directly in second-stage init bypasses Android's
    // first-stage setup_cgroups program. LXC creates the delegated cgroup-v2
    // root as root:root/0755, but ActivityManager (uid system) must create
    // groups itself for WebView/app zygotes. Regular zygote children hide this
    // because their groups are created while privileged; Chromium's isolated
    // uid exposes it and system_server aborts on EACCES. Match Cgroups2 in the
    // GSI's /system/etc/cgroups.json before Android starts.
    mount_hook << "/usr/bin/chown 1000:1000 "
                  "\"${LXC_ROOTFS_MOUNT}/sys/fs/cgroup\"\n";
    mount_hook << "/usr/bin/chmod 0775 "
                  "\"${LXC_ROOTFS_MOUNT}/sys/fs/cgroup\"\n";
    // cgroup v2 delegation also requires ownership of the root membership and
    // subtree-control files; changing only the directory lets system_server
    // mkdir uid_99000 but still makes the subsequent cgroup.procs write fail.
    mount_hook << "/usr/bin/chown 1000:1000 "
                  "\"${LXC_ROOTFS_MOUNT}/sys/fs/cgroup/cgroup.procs\" "
                  "\"${LXC_ROOTFS_MOUNT}/sys/fs/cgroup/cgroup.threads\" "
                  "\"${LXC_ROOTFS_MOUNT}/sys/fs/cgroup/cgroup.subtree_control\"\n";
    const auto add_private_loop_setting =
        [&mount_hook, &state_dir](int id, const char *name,
                                 const char *value) {
          const auto source = state_dir / utils::string_format(
              "loop%d_%s", id, name);
          std::ofstream output{source.string(),
                               std::ios::out | std::ios::trunc};
          if (!output.is_open())
            throw std::runtime_error(
                "Failed to create private APEX loop setting");
          output << value;
          if (!output.good())
            throw std::runtime_error(
                "Failed to initialize private APEX loop setting");
          output.close();
          if (::chmod(source.c_str(), 0644) != 0)
            throw std::runtime_error(
                "Failed to set private APEX loop setting permissions");
          mount_hook << "/usr/bin/mount --bind '" << source.string()
                     << "' \"${LXC_ROOTFS_MOUNT}/sys/block/loop" << id
                     << "/queue/" << name << "\"\n";
        };
    for (int id = first_apex_loop; id <= last_apex_loop; ++id) {
      add_private_loop_setting(id, "scheduler", "none\n");
      add_private_loop_setting(id, "nr_requests", "128\n");
      add_private_loop_setting(id, "read_ahead_kb", "128\n");
    }

    // lxc.mount.auto installs sysfs after ordinary mount entries. Hide the
    // host-global fusectl connection list from vold in this post-mount hook so
    // Android shutdown cannot abort unrelated host FUSE filesystems.
    const auto private_fuse_connections = state_dir / "fuse-connections";
    if (!fs::exists(private_fuse_connections))
      fs::create_directories(private_fuse_connections);
    if (::chmod(private_fuse_connections.c_str(), 0755) != 0) {
      throw std::runtime_error(utils::string_format(
          "Failed to set private fusectl directory permissions: %s",
          strerror(errno)));
    }
    mount_hook << "/usr/bin/mount --bind '"
               << private_fuse_connections.string()
               << "' \"${LXC_ROOTFS_MOUNT}/sys/fs/fuse/connections\"\n";
    mount_hook << "/usr/bin/rm -f \"${LXC_ROOTFS_MOUNT}/dev/qemu_pipe\" "
                  "\"${LXC_ROOTFS_MOUNT}/dev/goldfish_pipe_dprctd\" "
                  "\"${LXC_ROOTFS_MOUNT}/dev/anbox_bridge\" "
                  "\"${LXC_ROOTFS_MOUNT}/dev/anbox_audio\"\n";
    mount_hook << "/usr/bin/ln -s /dev/anbox_sockets/qemu_pipe "
                  "\"${LXC_ROOTFS_MOUNT}/dev/qemu_pipe\"\n";
    mount_hook << "/usr/bin/ln -s /dev/anbox_sockets/qemu_pipe "
                  "\"${LXC_ROOTFS_MOUNT}/dev/goldfish_pipe_dprctd\"\n";
    mount_hook << "/usr/bin/ln -s /dev/anbox_sockets/anbox_bridge "
                  "\"${LXC_ROOTFS_MOUNT}/dev/anbox_bridge\"\n";
    mount_hook << "/usr/bin/ln -s /dev/anbox_sockets/anbox_audio "
                  "\"${LXC_ROOTFS_MOUNT}/dev/anbox_audio\"\n";
    mount_hook.close();
    if (::chmod(loop_mount_hook.c_str(), 0755) != 0)
      throw std::runtime_error("Failed to make APEX loop mount hook executable");
    set_config_item("lxc.hook.mount", loop_mount_hook.string());
  }

  // First-stage Android init normally creates /dev/socket before handing over
  // to second-stage init. Modern GSIs started directly in second-stage mode
  // still require this directory for the property service sockets.
  const auto android_socket_dir =
      SystemConfiguration::instance().data_dir() / "dev-socket";
  if (!fs::exists(android_socket_dir))
    fs::create_directories(android_socket_dir);
  else if (modern_gsi) {
    // Android init owns the live contents of /dev/socket. When a previous GSI
    // boot crashes during service startup, stale sockets and service-private
    // directories can otherwise leak into the next boot because /dev/socket is
    // backed by a persistent host directory.
    for (fs::directory_iterator it{android_socket_dir}, end; it != end; ++it)
      fs::remove_all(it->path());
  }
  if (::chown(android_socket_dir.c_str(), container_root_uid,
              container_root_uid) != 0) {
    throw std::runtime_error(
        utils::string_format("Failed to assign /dev/socket backing directory: %s",
                             strerror(errno)));
  }
  if (::chmod(android_socket_dir.c_str(), 0755) != 0) {
    throw std::runtime_error(
        utils::string_format("Failed to set /dev/socket backing permissions: %s",
                             strerror(errno)));
  }
  if (modern_gsi) {
    const auto add_socket_alias = [&android_socket_dir](const char *alias,
                                                       const char *target) {
      const auto alias_path = android_socket_dir / alias;
      const auto status = fs::symlink_status(alias_path);
      if (fs::exists(status) || fs::is_symlink(status))
        fs::remove(alias_path);
      fs::create_symlink(target, alias_path);
    };

    // The Android 15 framework always attempts to connect to the secondary
    // zygote after forking system_server. Anbox Reboxed intentionally runs a single
    // 32-bit zygote for Linux 7 compatibility, so expose the secondary names
    // as aliases to the primary init-created sockets.
    add_socket_alias("zygote_secondary", "zygote");
    add_socket_alias("usap_pool_secondary", "usap_pool_primary");
  }
  bind_mounts.insert({android_socket_dir.string(), "/dev/socket"});

  // Android 15 init treats inability to write these hardening controls as
  // fatal. LXC intentionally protects the host-global /proc/sys hierarchy.
  // Bind private regular files over only the nodes init must update so Android
  // can enforce its in-container values without gaining access to host sysctls.
  const auto add_private_sysctl =
      [&bind_mounts, container_root_uid](const char *name,
                                        const char *target,
                                        const char *value) {
        const auto source =
            fs::path(SystemConfiguration::instance().container_state_dir()) /
            name;
        std::ofstream output{source.string(),
                             std::ios::out | std::ios::trunc};
        if (!output.is_open())
          throw std::runtime_error(
              utils::string_format("Failed to create private %s file", name));
        output << value;
        if (!output.good())
          throw std::runtime_error(
              utils::string_format("Failed to initialize private %s file",
                                   name));
        output.close();
        if (::chown(source.c_str(), container_root_uid, container_root_uid) !=
            0) {
          throw std::runtime_error(utils::string_format(
              "Failed to assign private %s file: %s", name, strerror(errno)));
        }
        if (::chmod(source.c_str(), 0644) != 0) {
          throw std::runtime_error(utils::string_format(
              "Failed to set private %s permissions: %s", name,
              strerror(errno)));
        }
        bind_mounts.emplace(source.string(), target);
      };
  add_private_sysctl("kptr_restrict", "/proc/sys/kernel/kptr_restrict", "2\n");
  if (modern_gsi) {
    add_private_sysctl("unprivileged_bpf_disabled",
                       "/proc/sys/kernel/unprivileged_bpf_disabled", "2\n");
    add_private_sysctl("mmap_rnd_bits", "/proc/sys/vm/mmap_rnd_bits", "32\n");
    add_private_sysctl("mmap_rnd_compat_bits",
                       "/proc/sys/vm/mmap_rnd_compat_bits", "16\n");

  }

  // If we have binderfs support we can dynamically allocate all our devices
  if (common::BinderDeviceAllocator::is_supported()) {
    DEBUG("Using binderfs to allocate our own binder nodes");

    std::vector<std::unique_ptr<common::BinderDevice>> binder_devices;
    if (!create_binder_devices(num_needed_binders, binder_devices) ||
        binder_devices.size() != num_needed_binders)
      throw std::runtime_error("Failed to allocate necessary binder devices");

    bind_mounts.insert({binder_devices[0]->path().string(), "/dev/binder"});
    bind_mounts.insert({binder_devices[1]->path().string(), "/dev/hwbinder"});
    bind_mounts.insert({binder_devices[2]->path().string(), "/dev/vndbinder"});
    binder_devices_ = std::move(binder_devices);
  } else {
    DEBUG("Using static binder device /dev/binder");
    devices.insert({"/dev/binder", { 0666 }});
    if (fs::exists("/dev/hwbinder"))
      devices.insert({"/dev/hwbinder", { 0666 }});
    if (fs::exists("/dev/vndbinder"))
      devices.insert({"/dev/vndbinder", { 0666 }});
  }

  for (const auto &bind_mount : bind_mounts) {
    std::string create_type = "file";

    if (fs::is_directory(bind_mount.first))
      create_type = "dir";

    auto target_path = bind_mount.second;
    // The target path needs to be absolute and pointing to the right
    // location inside the target rootfs as otherwise we get problems
    // when running in confined environments like snap's.
    if (!utils::string_starts_with(target_path, "/"))
      target_path = std::string("/") + target_path;
    target_path = rootfs_path + target_path;

    const auto entry = utils::string_format("%s %s none bind,create=%s,optional 0 0",
                                            bind_mount.first, target_path, create_type);
    set_config_item("lxc.mount.entry", entry);

    // API 35 ranchu's libandroidemu probes the deprecated goldfish pipe path
    // instead of Anbox Reboxed's historical /dev/qemu_pipe name. Both names must reach
    // the same host OpenGL connector.
    if (modern_gsi && bind_mount.second == "/dev/qemu_pipe") {
      const auto goldfish_target =
          rootfs_path + "/dev/goldfish_pipe_dprctd";
      const auto goldfish_entry = utils::string_format(
          "%s %s none bind,create=file,optional 0 0",
          bind_mount.first, goldfish_target);
      set_config_item("lxc.mount.entry", goldfish_entry);
    }
  }

  // Additional devices we need in our container
  devices.insert({"/dev/console", {0600}});
  devices.insert({"/dev/full", {0666}});
  devices.insert({"/dev/kmsg", {0644}});
  if (modern_gsi && fs::exists("/dev/loop-control")) {
    for (int id = first_apex_loop; id <= last_apex_loop; ++id) {
      const auto path = utils::string_format("/dev/loop%d", id);
      devices.insert({path, {0600}});
    }
    devices.insert({"/dev/loop-control", {0600}});
  }
  devices.insert({"/dev/null", {0666}});
  devices.insert({"/dev/random", {0666}});
  devices.insert({"/dev/tty", {0666}});
  devices.insert({"/dev/urandom", {0666}});
  devices.insert({"/dev/zero", {0666}});
  devices.insert({"/dev/tun", {0660, "/dev/net/tun"}});
  // Android 15 uses memfd-backed shared memory on kernels where the removed
  // staging ashmem driver is unavailable. Preserve /dev/ashmem for legacy
  // guests and kernels that still provide it, but do not manufacture a
  // required device from a nonexistent host node.
  if (fs::exists("/dev/ashmem"))
    devices.insert({"/dev/ashmem", {0666}});

  // Remove all left over devices from last time first before
  // creating any new ones
  const auto devices_dir = SystemConfiguration::instance().container_devices_dir();
  fs::remove_all(devices_dir);
  fs::create_directories(devices_dir);

  for (const auto& device : devices)
    add_device(device.first, device.second);

  // If we have any additional properties, add them to the first property file
  // available in the image and overlay it with a bind mount. Modern GSIs no
  // longer provide the legacy /default.prop file.
  if (!extra_properties.empty()) {
    const auto container_state_dir = SystemConfiguration::instance().container_state_dir();
    fs::path source_prop_path;
    for (const auto &relative_path :
         std::array<const char *, 3>{"default.prop",
                                     "system/etc/prop.default",
                                     "system/build.prop"}) {
      const auto candidate = fs::path(rootfs_path) / relative_path;
      if (fs::exists(candidate)) {
        source_prop_path = candidate;
        break;
      }
    }

    if (source_prop_path.empty())
      throw std::runtime_error("Android image contains no supported property file");

    auto new_prop_path = fs::path(container_state_dir) / "anbox.prop";
    auto prop_content = utils::read_file_if_exists_or_throw(source_prop_path.string());

    std::ofstream default_props;
    default_props.open(new_prop_path.string(), std::ios_base::out);
    if (!default_props.is_open())
      throw std::runtime_error("Failed to open new default properties file");

    default_props << "# Properties added by Anbox Reboxed" << std::endl;
    for (const auto& prop : extra_properties)
      default_props << prop << std::endl;

    default_props << std::endl
                  << prop_content << std::endl;

    default_props.close();

    set_config_item("lxc.mount.entry",
                    utils::string_format("%s %s none bind,optional,ro 0 0",
                                         new_prop_path.string(),
                                         source_prop_path.string()));

  }

  fs::path bindtab = SystemConfiguration::instance().data_dir() / "bindtab";
  if ( fs::exists(bindtab) && fs::is_regular_file(bindtab) ) {
    std::ifstream bindtab_data;
    bindtab_data.open(bindtab.string(), std::ios_base::in);

    if (bindtab_data.is_open()) {
      std::string bind_mnt;
      while (std::getline(bindtab_data, bind_mnt)) {
        if (!should_mount_graphics_compatibility_bind(modern_gfxstream,
                                                      bind_mnt)) {
          INFO("Graphics transport=gfxstream: preserving Android 15 vendor encoder instead of compatibility bind '%s'",
               bind_mnt);
          continue;
        }
        if (bind_mnt.rfind("/", 0) == 0)
          set_config_item("lxc.mount.entry", bind_mnt);
      }
    }
  }

  if (!container_->save_config(container_, nullptr))
    throw std::runtime_error("Failed to save container configuration");

  if (!container_->start(container_, 0, nullptr))
    throw std::runtime_error("Failed to start container");

  state_ = Container::State::running;

  DEBUG("Container successfully started");
}

void LxcContainer::stop() {
  if (!container_ || !container_->is_running(container_))
    return;

  if (!container_->stop(container_))
    throw std::runtime_error("Failed to stop container");

  state_ = Container::State::inactive;
  binder_devices_.clear();

  // Android init normalizes /dev/input to root:root 0755 through the bind
  // mount. Restore host-user write access after unmounting so the next session
  // can remove and recreate its input sockets without privileged residue.
  for (const auto &path : runtime_directories_to_restore_)
    ::chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
  runtime_directories_to_restore_.clear();

  DEBUG("Container successfully stopped");
}

void LxcContainer::set_config_item(const std::string &key,
                                   const std::string &value) {
  if (!container_->set_config_item(container_, key.c_str(), value.c_str())) {
    const auto msg = utils::string_format("Failed to set config item %s", key);
    throw std::runtime_error(msg);
  }
}

Container::State LxcContainer::state() { return state_; }
}
