// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>

#include "frameworkcpp/arena/shard.hpp"

namespace frameworkcpp::environment {
// Tuning knobs for the module. Everything the module owns — key table,
// value cells, patch journal, per-thread views — is carved from a single arena
// shard at init, so all of these are hard, fixed capacities: nothing grows
// after init and nothing touches the heap.
struct environment_config_t {
    std::size_t max_keys = 256;       // distinct keys (file + environ + set_env)
    std::size_t max_key_len = 256;    // longest key, in bytes
    std::size_t max_value_len = 4096; // longest value, in bytes
    std::size_t max_updates = 256;    // total set_env calls; each consumes one cell
    std::size_t max_threads = 64;     // concurrent per-thread views
    bool read_process_env = true;     // inherit the process environment at init
};

// How many bytes the module's own shard will occupy in the arena. Add this to
// your arena budget when sizing it.
std::size_t environment_footprint(const environment_config_t &config);
} // namespace frameworkcpp::environment