// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>

#include "frameworkcpp/arena/shard.hpp"

namespace frameworkcpp::environment {
// Tuning knobs for the module. Everything the module owns — key table, value
// pool, journal ring, retirement queue, per-thread views — is carved from a
// single arena shard at init, so all of these are hard, fixed capacities:
// nothing grows after init and nothing touches the heap.
//
// Values are immutable and recycled: a cell is reused once every live view has
// moved past the update that retired it. The memory is fixed, but the number of
// set_env calls is not — it is bounded only by how many retired cells are still
// pinned by lagging readers at any instant.
struct environment_config_t {
  std::size_t max_keys_ = 256;       // distinct keys (file + environ + set_env)
  std::size_t max_key_len_ = 256;    // longest key, in bytes
  std::size_t max_value_len_ = 4096; // longest value, in bytes
  std::size_t value_cells_ = 1024;   // recyclable value cells (live + in-flight retired)
  std::size_t journal_size_ = 256;   // delta ring length; must be a power of two
  std::size_t max_threads_ = 64;     // concurrent per-thread views
  bool read_process_env_ = true;     // inherit the process environment at init
}; // struct environment_config_t

// How many bytes the module's own shard will occupy in the arena. Usable in a
// constant expression (defined in <frameworkcpp/environment/environment.hpp>).
constexpr std::size_t get_environment_footprint(const environment_config_t &config);
} // namespace frameworkcpp::environment
