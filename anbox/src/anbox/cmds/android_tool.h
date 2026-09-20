/*
 * Copyright (C) 2026 The Anbox revival contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */

#ifndef ANBOX_CMDS_ANDROID_TOOL_H_
#define ANBOX_CMDS_ANDROID_TOOL_H_

#include <string>
#include <vector>

#include "anbox/cli.h"

namespace anbox::cmds {

int run_android_command(const std::vector<std::string>& command,
                        bool check_container = true);
bool android_container_is_running();

class AndroidTool : public cli::Command {
 public:
  AndroidTool(const std::string& name, const std::string& usage,
              const std::string& description,
              const std::vector<std::string>& android_command,
              bool append_arguments = true);

  int run(const Context& context) override;
  void help(std::ostream& out) override;

 private:
  std::vector<std::string> android_command_;
  bool append_arguments_;
};

class Install : public cli::Command {
 public:
  Install();
  int run(const Context& context) override;
  void help(std::ostream& out) override;
};

class ListPackages : public cli::Command {
 public:
  explicit ListPackages(const std::string& name);
  int run(const Context& context) override;
  void help(std::ostream& out) override;
};

}  // namespace anbox::cmds

#endif
