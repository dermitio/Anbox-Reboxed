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

#include <signal.h>
#include <sys/prctl.h>

#include <algorithm>

#include "anbox/system_configuration.h"
#include "anbox/daemon.h"
#include "anbox/logger.h"

#include "anbox/cmds/container_manager.h"
#include "anbox/cmds/session_manager.h"
#include "anbox/cmds/system_info.h"
#include "anbox/cmds/launch.h"
#include "anbox/cmds/version.h"
#include "anbox/cmds/wait_ready.h"
#include "anbox/cmds/check_features.h"
#include "anbox/cmds/android_tool.h"
#include "anbox/cmds/memory.h"
#include "anbox/cmds/compatibility.h"
#include "anbox/cmds/display_status.h"
#include "anbox/cmds/reboxed_tools.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

namespace anbox {
Daemon::Daemon()
    : cmd{cli::Name{"anbox-reboxed"}, cli::Usage{"Anbox Reboxed"},
          cli::Description{"The Anbox Reboxed Android runtime"}} {
  cmd.command(std::make_shared<cmds::Version>())
     .command(std::make_shared<cmds::SessionManager>())
     .command(std::make_shared<cmds::Launch>())
     .command(std::make_shared<cmds::ContainerManager>())
     .command(std::make_shared<cmds::SystemInfo>())
     .command(std::make_shared<cmds::WaitReady>())
     .command(std::make_shared<cmds::CheckFeatures>())
     .command(std::make_shared<cmds::DisplayStatus>())
     .command(std::make_shared<cmds::ReboxedBridge>())
     .command(std::make_shared<cmds::NativeBridge>())
     .command(std::make_shared<cmds::CompatibilityCommand>(
         "compatibility-status", "status", "Show Android guest compatibility overlay status"))
     .command(std::make_shared<cmds::CompatibilityCommand>(
         "compatibility-apply", "apply", "Generate the supported Android guest compatibility overlay"))
     .command(std::make_shared<cmds::CompatibilityCommand>(
         "compatibility-reset", "reset", "Remove generated Android guest compatibility overlays"))
     .command(std::make_shared<cmds::CompatibilityCommand>(
         "gfxstream-compatibility-status", "status",
         "Show Android gfxstream mapper compatibility status", true))
     .command(std::make_shared<cmds::CompatibilityCommand>(
         "gfxstream-compatibility-apply", "apply",
         "Generate the supported Android gfxstream mapper overlay", true))
     .command(std::make_shared<cmds::CompatibilityCommand>(
         "gfxstream-compatibility-reset", "reset",
         "Remove the Android gfxstream mapper overlay", true))
     .command(std::make_shared<cmds::MemoryCommand>(
         "memory-status", "Show host and Anbox Reboxed cgroup memory status",
         cmds::MemoryCommand::Action::status))
     .command(std::make_shared<cmds::MemoryCommand>(
         "swap-create", "Create the configured Anbox Reboxed swapfile",
         cmds::MemoryCommand::Action::create_swap))
     .command(std::make_shared<cmds::MemoryCommand>(
         "swap-enable", "Enable only the configured Anbox Reboxed swapfile",
         cmds::MemoryCommand::Action::enable_swap))
     .command(std::make_shared<cmds::MemoryCommand>(
         "swap-disable", "Disable only the configured Anbox Reboxed swapfile",
         cmds::MemoryCommand::Action::disable_swap))
     .command(std::make_shared<cmds::AndroidTool>(
         "shell", "shell [command [args...]]", "Open an Android container shell",
         std::vector<std::string>{"/system/bin/sh"}))
     .command(std::make_shared<cmds::AndroidTool>(
         "getprop", "getprop [name]", "Read Android system properties",
         std::vector<std::string>{"/system/bin/getprop"}))
     .command(std::make_shared<cmds::AndroidTool>(
         "logcat", "logcat [options]", "Read Android logs",
         std::vector<std::string>{"/system/bin/logcat"}))
     .command(std::make_shared<cmds::AndroidTool>(
         "service-list", "service-list", "List Android Binder services",
         std::vector<std::string>{"/system/bin/service", "list"}))
     .command(std::make_shared<cmds::Install>())
     .command(std::make_shared<cmds::ListPackages>("list-package"))
     .command(std::make_shared<cmds::ListPackages>("list-packages"));

  Log().Init(anbox::Logger::Severity::kWarning);

  const auto log_level = utils::get_env_value("ANBOX_LOG_LEVEL", "");
  if (!log_level.empty() && !Log().SetSeverityFromString(log_level))
    WARNING("Failed to set logging severity to '%s'", log_level);
}

int Daemon::Run(const std::vector<std::string> &arguments) try {
  auto argv = arguments;
  const bool help_requested =
      arguments.empty() || arguments.front() == "help" ||
      std::find(arguments.begin(), arguments.end(), "--help") != arguments.end() ||
      std::find(arguments.begin(), arguments.end(), "-h") != arguments.end();
  if (arguments.empty() ||
      (arguments.size() == 1 &&
       (arguments[0] == "--help" || arguments[0] == "-h")))
    argv = {"help"};
  else if (arguments.size() == 1 && arguments[0] == "--version")
    argv = {"version"};
  else if (arguments.size() == 2 &&
           (arguments[0] == "bridge" || arguments[0] == "native-bridge") &&
           (arguments[1] == "--help" || arguments[1] == "-h"))
    argv = {arguments[0], "help"};
  const auto result = cmd.run({std::cin, std::cout, argv});
  return help_requested ? EXIT_SUCCESS : result;
} catch (std::exception &err) {
  ERROR("%s", err.what());

  return EXIT_FAILURE;
}
}  // namespace anbox
