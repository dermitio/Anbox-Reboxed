/*
 * Copyright (C) 2026 The Anbox Reboxed contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ANBOX_CMDS_REBOXED_TOOLS_H_
#define ANBOX_CMDS_REBOXED_TOOLS_H_

#include "anbox/cli.h"

#include <string>

namespace anbox::cmds {

class ExternalToolAction : public cli::Command {
 public:
  ExternalToolAction(const std::string &name, const std::string &usage,
                     const std::string &description,
                     const std::string &environment_name,
                     const std::string &tool_name,
                     const std::string &action);

  int run(const Context &context) override;
  void help(std::ostream &out) override;

 private:
  std::string usage_;
  std::string environment_name_;
  std::string tool_name_;
  std::string action_;
};

class NativeBridge : public cli::CommandWithSubcommands {
 public:
  NativeBridge();
};

class ReboxedBridge : public cli::CommandWithSubcommands {
 public:
  ReboxedBridge();
};

}  // namespace anbox::cmds

#endif
