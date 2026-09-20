/*
 * Copyright (C) 2026 The Anbox Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */

#ifndef ANBOX_MEMORY_MANAGER_H_
#define ANBOX_MEMORY_MANAGER_H_

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace anbox::memory {

struct Configuration {
  bool swap_enabled{false};
  std::string swap_file{"/var/lib/anbox/anbox.swap"};
  std::uint64_t swap_file_size{0};
  std::string memory_high;
  std::string memory_max;
  std::string memory_swap_max;
  std::string source;
};

Configuration load_configuration();
std::uint64_t parse_size(const std::string &value, bool allow_max,
                         bool *is_max = nullptr);
std::string normalize_limit(const std::string &value);

void prepare_configured_swap(const Configuration &configuration);
void apply_lxc_limits(const Configuration &configuration,
                      const std::function<void(const std::string &,
                                               const std::string &)> &setter);

int print_status(std::ostream &out);
int create_swap(std::ostream &out);
int enable_swap(std::ostream &out);
int disable_swap(std::ostream &out);

}  // namespace anbox::memory

#endif  // ANBOX_MEMORY_MANAGER_H_
