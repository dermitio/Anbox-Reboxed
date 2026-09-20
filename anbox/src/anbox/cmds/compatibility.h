/*
 * Copyright (C) 2026 The Anbox Authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ANBOX_CMDS_COMPATIBILITY_H_
#define ANBOX_CMDS_COMPATIBILITY_H_

#include "anbox/cli.h"

#include <string>
#include <vector>

namespace anbox::cmds {

class CompatibilityCommand : public cli::CommandWithFlagsAndAction {
 public:
  CompatibilityCommand(const std::string& name, const std::string& action,
                       const std::string& description,
                       bool gfxstream = false);
};

int run_gles_compatibility_tool(const std::vector<std::string>& arguments);
int run_gfxstream_compatibility_tool(
    const std::vector<std::string>& arguments);

}  // namespace anbox::cmds
#endif
