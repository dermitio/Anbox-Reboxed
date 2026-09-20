/*
 * Copyright (C) 2026 The Anbox Reboxed Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */

#include "anbox/memory_manager.h"

#include "anbox/system_configuration.h"
#include "anbox/utils.h"

#include <boost/filesystem.hpp>

#include <linux/fs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = boost::filesystem;

namespace {

constexpr long btrfs_super_magic = 0x9123683e;
constexpr long ext_super_magic = 0xef53;
constexpr long f2fs_super_magic = 0xf2f52010;
constexpr long xfs_super_magic = 0x58465342;

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return value;
}

bool parse_bool(const std::string &value) {
  const auto normalized = lower(trim(value));
  if (normalized == "1" || normalized == "true" || normalized == "yes" ||
      normalized == "on")
    return true;
  if (normalized == "0" || normalized == "false" || normalized == "no" ||
      normalized == "off")
    return false;
  throw std::runtime_error("Invalid boolean value '" + value + "'");
}

std::map<std::string, std::string> read_config_file(const fs::path &path) {
  std::ifstream input{path.string()};
  if (!input)
    throw std::runtime_error("Failed to open memory configuration " +
                             path.string() + ": " + std::strerror(errno));

  std::map<std::string, std::string> result;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos)
      throw std::runtime_error(path.string() + ":" +
                               std::to_string(line_number) +
                               ": expected key=value");
    const auto key = trim(line.substr(0, separator));
    const auto value = trim(line.substr(separator + 1));
    if (key.empty())
      throw std::runtime_error(path.string() + ":" +
                               std::to_string(line_number) +
                               ": empty key");
    result[key] = value;
  }
  return result;
}

int run_command(const std::vector<std::string> &arguments) {
  if (arguments.empty())
    return -1;
  const pid_t pid = ::fork();
  if (pid < 0)
    throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("waitpid failed: " +
                               std::string(std::strerror(errno)));
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

std::map<std::string, std::uint64_t> meminfo() {
  std::ifstream input{"/proc/meminfo"};
  std::map<std::string, std::uint64_t> values;
  std::string key;
  std::uint64_t value = 0;
  std::string unit;
  while (input >> key >> value >> unit) {
    if (!key.empty() && key.back() == ':')
      key.pop_back();
    values[key] = unit == "kB" ? value * 1024 : value;
  }
  return values;
}

std::string human_bytes(std::uint64_t bytes) {
  static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' '
         << units[unit];
  return output.str();
}

bool configured_swap_active(const std::string &path) {
  std::ifstream swaps{"/proc/swaps"};
  std::string line;
  std::getline(swaps, line);
  while (std::getline(swaps, line)) {
    std::istringstream fields{line};
    std::string filename;
    fields >> filename;
    if (filename == path)
      return true;
  }
  return false;
}

void require_root(const char *operation) {
  if (::geteuid() != 0)
    throw std::runtime_error(std::string(operation) + " requires root privileges");
}

bool has_swap_signature(const std::string &path) {
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size < 10)
    return false;
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return false;
  input.seekg(page_size - 10);
  char signature[10]{};
  input.read(signature, sizeof(signature));
  return input.gcount() == sizeof(signature) &&
         std::string(signature, sizeof(signature)) == "SWAPSPACE2";
}

bool cgroup_has_processes(const fs::path &path) {
  std::ifstream processes{(path / "cgroup.procs").string()};
  std::string pid;
  return static_cast<bool>(processes >> pid);
}

void validate_swap_filesystem(const fs::path &parent, int fd) {
  struct statfs filesystem {};
  if (::statfs(parent.c_str(), &filesystem) < 0)
    throw std::runtime_error("Cannot inspect swap filesystem: " +
                             std::string(std::strerror(errno)));
  if (filesystem.f_type != ext_super_magic &&
      filesystem.f_type != xfs_super_magic &&
      filesystem.f_type != btrfs_super_magic &&
      filesystem.f_type != f2fs_super_magic) {
    std::ostringstream message;
    message << "Filesystem type 0x" << std::hex << filesystem.f_type
            << " is not in Anbox Reboxed's validated swapfile filesystem set";
    throw std::runtime_error(message.str());
  }

  if (filesystem.f_type == btrfs_super_magic) {
    unsigned long flags = 0;
    if (::ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0)
      throw std::runtime_error("Cannot read Btrfs swapfile flags: " +
                               std::string(std::strerror(errno)));
    flags |= FS_NOCOW_FL;
    if (::ioctl(fd, FS_IOC_SETFLAGS, &flags) < 0)
      throw std::runtime_error("Cannot set NOCOW on Btrfs swapfile: " +
                               std::string(std::strerror(errno)));
  }
}

fs::path find_cgroup_path() {
  const fs::path root{"/sys/fs/cgroup"};
  const auto exact = root / "lxc.payload.default";
  if (fs::is_directory(exact) && cgroup_has_processes(exact))
    return exact;
  if (!fs::is_directory(root))
    return {};
  for (fs::directory_iterator entry{root}, end; entry != end; ++entry) {
    const auto name = entry->path().filename().string();
    if (name.rfind("lxc.payload.default-", 0) == 0 &&
        cgroup_has_processes(entry->path()))
      return entry->path();
  }
  return {};
}

std::string read_first_line(const fs::path &path) {
  std::ifstream input{path.string()};
  std::string value;
  if (input)
    std::getline(input, value);
  return value.empty() ? "unavailable" : value;
}

}  // namespace

std::uint64_t anbox::memory::parse_size(const std::string &input,
                                        bool allow_max, bool *is_max) {
  auto value = lower(trim(input));
  if (is_max)
    *is_max = false;
  if (allow_max && value == "max") {
    if (is_max)
      *is_max = true;
    return 0;
  }
  if (value.empty())
    throw std::runtime_error("Empty size value");

  std::size_t consumed = 0;
  std::uint64_t number = 0;
  try {
    number = std::stoull(value, &consumed, 10);
  } catch (const std::exception &) {
    throw std::runtime_error("Invalid size value '" + input + "'");
  }
  auto suffix = value.substr(consumed);
  std::uint64_t multiplier = 1;
  if (suffix.empty() || suffix == "b") multiplier = 1;
  else if (suffix == "k" || suffix == "kb" || suffix == "kib") multiplier = 1ULL << 10;
  else if (suffix == "m" || suffix == "mb" || suffix == "mib") multiplier = 1ULL << 20;
  else if (suffix == "g" || suffix == "gb" || suffix == "gib") multiplier = 1ULL << 30;
  else if (suffix == "t" || suffix == "tb" || suffix == "tib") multiplier = 1ULL << 40;
  else throw std::runtime_error("Invalid size suffix in '" + input + "'");

  if (number > std::numeric_limits<std::uint64_t>::max() / multiplier)
    throw std::runtime_error("Size value overflows: '" + input + "'");
  return number * multiplier;
}

std::string anbox::memory::normalize_limit(const std::string &value) {
  bool is_max = false;
  const auto bytes = parse_size(value, true, &is_max);
  return is_max ? "max" : std::to_string(bytes);
}

anbox::memory::Configuration anbox::memory::load_configuration() {
  Configuration configuration;
  const auto env_path = anbox::utils::get_env_value("ANBOX_MEMORY_CONFIG");
  std::vector<fs::path> candidates;
  if (!env_path.empty())
    candidates.emplace_back(env_path);
  else {
    candidates.emplace_back("/etc/anbox/memory.conf");
    candidates.emplace_back("/var/lib/anbox/memory.conf");
    candidates.emplace_back(SystemConfiguration::instance().data_dir() /
                            "memory.conf");
  }

  fs::path selected;
  for (const auto &candidate : candidates) {
    if (fs::exists(candidate)) {
      selected = candidate;
      break;
    }
  }
  if (selected.empty())
    return configuration;

  configuration.source = selected.string();
  const auto values = read_config_file(selected);
  const std::map<std::string, std::function<void(const std::string &)>> handlers{
      {"android.memory.high", [&](const auto &v) { configuration.memory_high = normalize_limit(v); }},
      {"android.memory.max", [&](const auto &v) { configuration.memory_max = normalize_limit(v); }},
      {"android.memory.swap.max", [&](const auto &v) { configuration.memory_swap_max = normalize_limit(v); }},
      {"android.swap.enabled", [&](const auto &v) { configuration.swap_enabled = parse_bool(v); }},
      {"android.swap.file", [&](const auto &v) { configuration.swap_file = v; }},
      {"android.swap.file_size", [&](const auto &v) { configuration.swap_file_size = parse_size(v, false); }},
  };
  for (const auto &[key, value] : values) {
    const auto handler = handlers.find(key);
    if (handler == handlers.end())
      throw std::runtime_error("Unknown memory configuration key '" + key +
                               "' in " + selected.string());
    handler->second(value);
  }

  if (!configuration.swap_file.empty() && configuration.swap_file[0] != '/')
    throw std::runtime_error("android.swap.file must be an absolute path");
  if (configuration.swap_enabled && configuration.swap_file_size == 0)
    throw std::runtime_error(
        "android.swap.enabled=true requires android.swap.file_size");

  if (!configuration.memory_high.empty() &&
      !configuration.memory_max.empty() &&
      configuration.memory_high != "max" && configuration.memory_max != "max" &&
      std::stoull(configuration.memory_high) > std::stoull(configuration.memory_max))
    throw std::runtime_error("android.memory.high must not exceed android.memory.max");
  return configuration;
}

void anbox::memory::apply_lxc_limits(
    const Configuration &configuration,
    const std::function<void(const std::string &, const std::string &)> &setter) {
  if (configuration.memory_high.empty() && configuration.memory_max.empty() &&
      configuration.memory_swap_max.empty())
    return;
  if (!fs::exists("/sys/fs/cgroup/cgroup.controllers"))
    throw std::runtime_error("Configured Anbox Reboxed memory limits require cgroup v2");
  if (!configuration.memory_high.empty())
    setter("lxc.cgroup2.memory.high", configuration.memory_high);
  if (!configuration.memory_max.empty())
    setter("lxc.cgroup2.memory.max", configuration.memory_max);
  if (!configuration.memory_swap_max.empty())
    setter("lxc.cgroup2.memory.swap.max", configuration.memory_swap_max);
}

void anbox::memory::prepare_configured_swap(
    const Configuration &configuration) {
  if (!configuration.swap_enabled)
    return;
  require_root("Enabling configured Anbox Reboxed swap");
  if (!fs::is_regular_file(configuration.swap_file))
    throw std::runtime_error("Configured Anbox Reboxed swapfile does not exist; run 'anbox swap-create'");
  if (!has_swap_signature(configuration.swap_file))
    throw std::runtime_error("Configured Anbox Reboxed swapfile has no valid swap signature");
  if (!configured_swap_active(configuration.swap_file) &&
      run_command({"swapon", "--", configuration.swap_file}) != 0)
    throw std::runtime_error("swapon rejected the configured Anbox Reboxed swapfile; check filesystem swapfile support");
}

int anbox::memory::create_swap(std::ostream &out) {
  require_root("swap-create");
  const auto configuration = load_configuration();
  if (configuration.swap_file_size == 0)
    throw std::runtime_error("Configure android.swap.file_size before creating swap");
  const fs::path path{configuration.swap_file};
  const auto parent = path.parent_path();
  if (!fs::is_directory(parent))
    throw std::runtime_error("Swapfile parent directory does not exist: " + parent.string());

  if (fs::exists(path)) {
    struct stat state {};
    if (::lstat(path.c_str(), &state) < 0)
      throw std::runtime_error("Cannot inspect existing swapfile: " + std::string(std::strerror(errno)));
    if (!S_ISREG(state.st_mode) || state.st_uid != 0)
      throw std::runtime_error("Existing swap path is not a root-owned regular file");
    if (static_cast<std::uint64_t>(state.st_size) != configuration.swap_file_size)
      throw std::runtime_error("Existing swapfile has a different size; disable it and remove it explicitly before replacement");
    if (!has_swap_signature(path.string()))
      throw std::runtime_error("Existing same-sized file is not initialized as swap; refusing to overwrite it");
    ::chmod(path.c_str(), 0600);
    out << "Swapfile already exists with the configured size; unchanged: " << path.string() << '\n';
    return EXIT_SUCCESS;
  }

  const auto temporary = path.string() + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    throw std::runtime_error("Cannot create temporary swapfile: " + std::string(std::strerror(errno)));
  try {
    validate_swap_filesystem(parent, fd);
    const int allocation = ::posix_fallocate(fd, 0, configuration.swap_file_size);
    if (allocation != 0)
      throw std::runtime_error("Swapfile preallocation failed: " + std::string(std::strerror(allocation)));
    struct stat state {};
    if (::fstat(fd, &state) < 0)
      throw std::runtime_error("Cannot inspect allocated swapfile: " + std::string(std::strerror(errno)));
    if (static_cast<std::uint64_t>(state.st_blocks) * 512 < configuration.swap_file_size)
      throw std::runtime_error("Filesystem produced a sparse swapfile; refusing it");
    if (::fsync(fd) < 0)
      throw std::runtime_error("Cannot sync allocated swapfile: " + std::string(std::strerror(errno)));
    ::close(fd);
    if (run_command({"mkswap", temporary}) != 0)
      throw std::runtime_error("mkswap failed for the new Anbox Reboxed swapfile");
    if (::rename(temporary.c_str(), path.c_str()) < 0)
      throw std::runtime_error("Cannot install new swapfile: " + std::string(std::strerror(errno)));
  } catch (...) {
    ::close(fd);
    ::unlink(temporary.c_str());
    throw;
  }
  out << "Created " << human_bytes(configuration.swap_file_size) << " swapfile at "
      << path.string() << " (inactive)\n";
  return EXIT_SUCCESS;
}

int anbox::memory::enable_swap(std::ostream &out) {
  require_root("swap-enable");
  const auto configuration = load_configuration();
  if (!fs::is_regular_file(configuration.swap_file))
    throw std::runtime_error("Configured swapfile does not exist; run 'anbox swap-create'");
  if (!has_swap_signature(configuration.swap_file))
    throw std::runtime_error("Configured file has no swap signature");
  if (configured_swap_active(configuration.swap_file)) {
    out << "Configured Anbox Reboxed swapfile is already active\n";
    return EXIT_SUCCESS;
  }
  if (run_command({"swapon", "--", configuration.swap_file}) != 0)
    throw std::runtime_error("swapon rejected the file; the filesystem may not support this swapfile layout");
  out << "Enabled host-global swapfile " << configuration.swap_file << '\n';
  out << "Anbox Reboxed containment is provided by memory.swap.max, not by swapfile ownership\n";
  return EXIT_SUCCESS;
}

int anbox::memory::disable_swap(std::ostream &out) {
  require_root("swap-disable");
  const auto configuration = load_configuration();
  if (!configured_swap_active(configuration.swap_file)) {
    out << "Configured Anbox Reboxed swapfile is not active; unchanged\n";
    return EXIT_SUCCESS;
  }
  if (run_command({"swapoff", "--", configuration.swap_file}) != 0)
    throw std::runtime_error("swapoff failed for the configured Anbox Reboxed swapfile");
  out << "Disabled configured Anbox Reboxed swapfile " << configuration.swap_file << '\n';
  return EXIT_SUCCESS;
}

int anbox::memory::print_status(std::ostream &out) {
  const auto configuration = load_configuration();
  const auto memory = meminfo();
  const auto mem_total = memory.count("MemTotal") ? memory.at("MemTotal") : 0;
  const auto mem_available = memory.count("MemAvailable") ? memory.at("MemAvailable") : 0;
  const auto swap_total = memory.count("SwapTotal") ? memory.at("SwapTotal") : 0;
  const auto swap_free = memory.count("SwapFree") ? memory.at("SwapFree") : 0;

  out << "Configuration: " << (configuration.source.empty() ? "none (disabled defaults)" : configuration.source) << '\n';
  out << "Host RAM used: " << human_bytes(mem_total - std::min(mem_total, mem_available))
      << " / " << human_bytes(mem_total) << '\n';
  out << "Host swap used: " << human_bytes(swap_total - std::min(swap_total, swap_free))
      << " / " << human_bytes(swap_total) << '\n';
  out << "Configured swapfile: " << configuration.swap_file << '\n';
  out << "Configured swapfile size: " << human_bytes(configuration.swap_file_size) << '\n';
  out << "Configured automatic enable: " << (configuration.swap_enabled ? "yes" : "no") << '\n';
  out << "Configured swapfile active: " << (configured_swap_active(configuration.swap_file) ? "yes" : "no") << '\n';
  out << "Configured memory.high: " << (configuration.memory_high.empty() ? "unset" : configuration.memory_high) << '\n';
  out << "Configured memory.max: " << (configuration.memory_max.empty() ? "unset" : configuration.memory_max) << '\n';
  out << "Configured memory.swap.max: " << (configuration.memory_swap_max.empty() ? "unset" : configuration.memory_swap_max) << '\n';

  const auto cgroup = find_cgroup_path();
  if (cgroup.empty()) {
    out << "Anbox Reboxed cgroup: container not running\n";
  } else {
    out << "Anbox Reboxed cgroup: " << cgroup.string() << '\n';
    for (const auto *name : {"memory.current", "memory.peak", "memory.swap.current",
                             "memory.high", "memory.max", "memory.swap.max"})
      out << name << ": " << read_first_line(cgroup / name) << '\n';
  }
  out << "Note: swap is host-global; cgroup v2 limits provide Anbox Reboxed containment.\n";
  out << "Heavy swapping reduces performance and increases SSD writes.\n";
  return EXIT_SUCCESS;
}
