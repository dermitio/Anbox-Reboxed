/*
 * Copyright (C) 2026 The Anbox Authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "anbox/cmds/compatibility.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

std::string find_tool(const char* environment_name, const char* tool_name) {
  const char* override_path = std::getenv(environment_name);
  if (override_path && ::access(override_path, X_OK) == 0)
    return override_path;
  for (const auto* root : {"/usr/local/lib/anbox/", "/usr/lib/anbox/"}) {
    const auto path = std::string{root} + tool_name;
    if (::access(path.c_str(), X_OK) == 0)
      return path;
  }
  throw std::runtime_error(std::string{tool_name} + " is not installed");
}

int run_tool(const std::string& tool,
             const std::vector<std::string>& arguments) {
  std::vector<std::string> storage;
  storage.push_back(tool);
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : storage)
    argv.push_back(value.data());
  argv.push_back(nullptr);

  const auto child = ::fork();
  if (child < 0)
    throw std::runtime_error(std::string("Failed to fork compatibility tool: ") +
                             std::strerror(errno));
  if (child == 0) {
    ::execv(tool.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error(std::string("Failed waiting for compatibility tool: ") +
                               std::strerror(errno));
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}

}  // namespace

int anbox::cmds::run_gles_compatibility_tool(
    const std::vector<std::string>& arguments) {
  return run_tool(find_tool("ANBOX_GLES_COMPAT_TOOL", "anbox-gles-compat"),
                  arguments);
}

int anbox::cmds::run_gfxstream_compatibility_tool(
    const std::vector<std::string>& arguments) {
  return run_tool(
      find_tool("ANBOX_GFXSTREAM_COMPAT_TOOL", "anbox-gfxstream-compat"),
      arguments);
}

anbox::cmds::CompatibilityCommand::CompatibilityCommand(
    const std::string& name, const std::string& command_action,
    const std::string& description, bool gfxstream)
    : CommandWithFlagsAndAction{cli::Name{name}, cli::Usage{name},
                                cli::Description{description}} {
  action([command_action, gfxstream](const cli::Command::Context& context) {
    try {
      const auto result = gfxstream
          ? run_gfxstream_compatibility_tool({command_action})
          : run_gles_compatibility_tool({command_action});
      if (result != EXIT_SUCCESS)
        context.cout << "Compatibility command failed with status " << result << '\n';
      return result;
    } catch (const std::exception& error) {
      context.cout << "Error: " << error.what() << '\n';
      return EXIT_FAILURE;
    }
  });
}
