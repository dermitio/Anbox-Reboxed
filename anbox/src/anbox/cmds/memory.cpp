/*
 * Copyright (C) 2026 The Anbox Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */

#include "anbox/cmds/memory.h"

#include "anbox/memory_manager.h"

anbox::cmds::MemoryCommand::MemoryCommand(const std::string &name,
                                          const std::string &description,
                                          Action command_action)
    : CommandWithFlagsAndAction{cli::Name{name}, cli::Usage{name},
                                cli::Description{description}} {
  action([command_action](const cli::Command::Context &context) {
    try {
      switch (command_action) {
        case Action::status:
          return memory::print_status(context.cout);
        case Action::create_swap:
          return memory::create_swap(context.cout);
        case Action::enable_swap:
          return memory::enable_swap(context.cout);
        case Action::disable_swap:
          return memory::disable_swap(context.cout);
        default:
          return EXIT_FAILURE;
      }
    } catch (const std::exception &error) {
      context.cout << "Error: " << error.what() << '\n';
      return EXIT_FAILURE;
    }
  });
}
