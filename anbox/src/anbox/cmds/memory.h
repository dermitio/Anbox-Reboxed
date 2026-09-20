/*
 * Copyright (C) 2026 The Anbox Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */

#ifndef ANBOX_CMDS_MEMORY_H_
#define ANBOX_CMDS_MEMORY_H_

#include "anbox/cli.h"

namespace anbox::cmds {

class MemoryCommand : public cli::CommandWithFlagsAndAction {
 public:
  enum class Action { status, create_swap, enable_swap, disable_swap };
  MemoryCommand(const std::string &name, const std::string &description,
                Action action);
};

}  // namespace anbox::cmds

#endif  // ANBOX_CMDS_MEMORY_H_
