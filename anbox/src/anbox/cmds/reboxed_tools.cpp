/*
 * Copyright (C) 2026 The Anbox Reboxed contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "anbox/cmds/reboxed_tools.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

std::string find_tool(const std::string &environment_name,
                      const std::string &tool_name) {
  if (const char *override_path = std::getenv(environment_name.c_str());
      override_path && ::access(override_path, X_OK) == 0)
    return override_path;

  for (const auto *root : {"/usr/local/lib/anbox/", "/usr/lib/anbox/"}) {
    const auto path = std::string{root} + tool_name;
    if (::access(path.c_str(), X_OK) == 0)
      return path;
  }

  // Keep the build tree directly runnable without installing it first.
  const auto source_path = std::string{ANBOX_SOURCE_DIR} + "/scripts/" + tool_name;
  if (::access(source_path.c_str(), X_OK) == 0)
    return source_path;

  throw std::runtime_error(tool_name + " is not installed");
}

int run_tool(const std::string &path, const std::string &action,
             const std::vector<std::string> &arguments) {
  std::vector<std::string> storage{path, action};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (auto &argument : storage)
    argv.push_back(argument.data());
  argv.push_back(nullptr);

  const auto child = ::fork();
  if (child < 0)
    throw std::runtime_error(std::string{"Failed to fork: "} +
                             std::strerror(errno));
  if (child == 0) {
    ::execv(path.c_str(), argv.data());
    ::_exit(127);
  }

  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error(std::string{"Failed waiting for "} + path);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}

std::shared_ptr<anbox::cmds::ExternalToolAction> native_action(
    const std::string &name, const std::string &usage,
    const std::string &description) {
  return std::make_shared<anbox::cmds::ExternalToolAction>(
      name, usage, description, "ANBOX_NATIVE_BRIDGE_TOOL",
      "anbox-native-bridge", name);
}

std::shared_ptr<anbox::cmds::ExternalToolAction> bridge_action(
    const std::string &name, const std::string &usage,
    const std::string &description) {
  return std::make_shared<anbox::cmds::ExternalToolAction>(
      name, usage, description, "ANBOX_REBOXED_BRIDGE_TOOL",
      "anbox-reboxed-bridge", name);
}

}  // namespace

anbox::cmds::ExternalToolAction::ExternalToolAction(
    const std::string &name, const std::string &usage,
    const std::string &description, const std::string &environment_name,
    const std::string &tool_name, const std::string &action)
    : Command{cli::Name{name}, cli::Usage{usage}, cli::Description{description}},
      usage_{usage},
      environment_name_{environment_name},
      tool_name_{tool_name},
      action_{action} {}

int anbox::cmds::ExternalToolAction::run(const Context &context) {
  if (!context.args.empty() &&
      (context.args.front() == "--help" || context.args.front() == "-h")) {
    help(context.cout);
    return EXIT_SUCCESS;
  }
  try {
    return run_tool(find_tool(environment_name_, tool_name_), action_,
                    context.args);
  } catch (const std::exception &error) {
    context.cout << "Error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

void anbox::cmds::ExternalToolAction::help(std::ostream &out) {
  out << "USAGE:\n    anbox-reboxed " << usage_ << "\n\n"
      << description().as_string() << '\n';
}

anbox::cmds::NativeBridge::NativeBridge()
    : CommandWithSubcommands{
          cli::Name{"native-bridge"}, cli::Usage{"native-bridge <command>"},
          cli::Description{"Manage official and external Android Native Bridge backends"}} {
  command(native_action("install", "native-bridge install berberis | --source <path>",
                        "Select the official backend or stage an external payload"))
      .command(native_action("enable", "native-bridge enable [berberis|external]",
                             "Enable the installed Native Bridge backend"))
      .command(native_action("disable", "native-bridge disable",
                             "Disable the translator and restore properties"))
      .command(native_action("remove", "native-bridge remove",
                             "Remove the translator overlay and saved state"))
      .command(native_action("status", "native-bridge status",
                             "Show translator, ABI, property, and test status"))
      .command(native_action("verify", "native-bridge verify",
                             "Run ARM32 and ARM64 Android-process JNI probes"));
}

anbox::cmds::ReboxedBridge::ReboxedBridge()
    : CommandWithSubcommands{
          cli::Name{"bridge"}, cli::Usage{"bridge <command>"},
          cli::Description{"Operate the Reboxed host/guest bridge"}} {
  command(bridge_action("daemon", "bridge daemon [options]",
                        "Run the unprivileged versioned bridge daemon"))
      .command(bridge_action("status", "bridge status [options]",
                             "Show bridge and transfer status"))
      .command(bridge_action("send", "bridge send <path> [options]",
                             "Send a validated host file to Android"))
      .command(bridge_action("install", "bridge install <apk...> [options]",
                             "Preflight and install an APK or split set"))
      .command(bridge_action("launch", "bridge launch [intent options]",
                             "Forward a validated intent to Android"))
      .command(bridge_action("diagnose", "bridge diagnose",
                             "Request Android ABI and Native Bridge diagnostics"))
      .command(native_action("verify", "bridge verify",
                             "Run ARM32 and ARM64 Android-process JNI probes"))
      .command(bridge_action("cancel", "bridge cancel <transfer-id>",
                             "Cancel an active file transfer"));
}
