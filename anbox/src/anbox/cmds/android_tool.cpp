/*
 * Copyright (C) 2026 The Anbox Reboxed revival contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */

#include "anbox/cmds/android_tool.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

constexpr const char* container_path = "/var/lib/anbox/containers";
constexpr const char* container_name = "default";
constexpr const char* lxc_prefix_environment = "ANBOX_REBOXED_LXC_PREFIX";

std::vector<std::string> lxc_tool(const std::string& name) {
  const auto* prefix = std::getenv(lxc_prefix_environment);
  if (!prefix || prefix[0] == '\0')
    return {"sudo", name};

  const std::string root{prefix};
  return {"sudo", "env", "LD_LIBRARY_PATH=" + root + "/usr/lib",
          root + "/usr/bin/" + name};
}

int run_program(const std::vector<std::string>& arguments, bool quiet = false) {
  if (arguments.empty())
    return EXIT_FAILURE;

  const auto pid = fork();
  if (pid < 0) {
    std::cerr << "Failed to fork: " << std::strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }

  if (pid == 0) {
    if (quiet) {
      const auto null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
      if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
      }
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    std::cerr << "Failed to execute " << arguments[0] << ": "
              << std::strerror(errno) << std::endl;
    _exit(127);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      std::cerr << "Failed waiting for child process: " << std::strerror(errno)
                << std::endl;
      return EXIT_FAILURE;
    }
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return EXIT_FAILURE;
}

std::vector<std::string> attach_prefix() {
  auto arguments = lxc_tool("lxc-attach");
  arguments.insert(arguments.end(),
                   {"-P", container_path, "-n", container_name, "--"});
  return arguments;
}

}  // namespace

bool anbox::cmds::android_container_is_running() {
  auto arguments = lxc_tool("lxc-info");
  arguments.insert(arguments.end(),
                   {"-P", container_path, "-n", container_name, "-sH"});
  return run_program(arguments, true) == EXIT_SUCCESS;
}

int anbox::cmds::run_android_command(const std::vector<std::string>& command,
                                     bool check_container) {
  if (check_container && !android_container_is_running()) {
    std::cerr << "Anbox Reboxed container '" << container_name
              << "' is not running. Start anbox-container-manager.service first."
              << std::endl;
    return EXIT_FAILURE;
  }

  auto arguments = attach_prefix();
  arguments.insert(arguments.end(), command.begin(), command.end());
  return run_program(arguments);
}

anbox::cmds::AndroidTool::AndroidTool(
    const std::string& name, const std::string& usage,
    const std::string& description,
    const std::vector<std::string>& android_command, bool append_arguments)
    : Command{cli::Name{name}, cli::Usage{usage}, cli::Description{description}},
      android_command_{android_command},
      append_arguments_{append_arguments} {}

int anbox::cmds::AndroidTool::run(const Context& context) {
  auto command = android_command_;
  if (append_arguments_)
    command.insert(command.end(), context.args.begin(), context.args.end());
  return run_android_command(command);
}

void anbox::cmds::AndroidTool::help(std::ostream& out) {
  out << "USAGE:\n    anbox " << usage().as_string() << "\n\n"
      << description().as_string() << std::endl;
}

anbox::cmds::Install::Install()
    : Command{cli::Name{"install"}, cli::Usage{"install <apk>"},
              cli::Description{"Install an APK using Android PackageManager"}} {}

int anbox::cmds::Install::run(const Context& context) {
  if (context.args.size() != 1) {
    help(context.cout);
    return EXIT_FAILURE;
  }
  if (!android_container_is_running()) {
    std::cerr << "Anbox Reboxed container '" << container_name << "' is not running."
              << std::endl;
    return EXIT_FAILURE;
  }

  const auto source = context.args[0];
  if (access(source.c_str(), R_OK) != 0) {
    std::cerr << "Cannot read APK '" << source << "': " << std::strerror(errno)
              << std::endl;
    return EXIT_FAILURE;
  }

  const auto guest_name =
      std::string{".anbox-install-"} + std::to_string(getpid()) + ".apk";
  const auto host_path = std::string{"/var/lib/anbox/data/"} + guest_name;
  const auto guest_path = std::string{"/data/"} + guest_name;

  auto result = run_program({"sudo", "install", "-m", "0644", source, host_path});
  if (result == EXIT_SUCCESS)
    result = run_android_command({"/system/bin/pm", "install", "-r", guest_path},
                                 false);

  const auto cleanup_result = run_program({"sudo", "rm", "-f", host_path});
  if (cleanup_result != EXIT_SUCCESS)
    std::cerr << "Warning: failed to remove temporary APK " << host_path
              << std::endl;
  return result;
}

void anbox::cmds::Install::help(std::ostream& out) {
  out << "USAGE:\n    anbox-reboxed install <apk>\n\n"
      << "Copies the APK into the container data directory temporarily and runs "
         "pm install -r."
      << std::endl;
}

anbox::cmds::ListPackages::ListPackages(const std::string& name)
    : Command{cli::Name{name}, cli::Usage{name + " [pm-list-options]"},
              cli::Description{"List packages known to Android PackageManager"}} {}

int anbox::cmds::ListPackages::run(const Context& context) {
  std::vector<std::string> command{"/system/bin/cmd", "package", "list",
                                   "packages"};
  command.insert(command.end(), context.args.begin(), context.args.end());
  return run_android_command(command);
}

void anbox::cmds::ListPackages::help(std::ostream& out) {
  out << "USAGE:\n    anbox " << name().as_string()
      << " [pm-list-options]\n\nLists installed Android packages." << std::endl;
}
