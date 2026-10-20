// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "frameworkcpp/arena/shard.hpp"

namespace frameworkcpp::reactor {

// Número máximo de colas nombradas soportado por un reactor.
inline constexpr std::size_t max_reactor_queues_k = 64;

// Número máximo de sockets registrados por reactor.
inline constexpr std::size_t max_reactor_sockets_k = 4096;

// Número máximo de strands por reactor.
inline constexpr std::size_t max_reactor_strands_k = 4096;

// Número de specs que el reactor carvea del arena.
inline constexpr std::size_t reactor_spec_count_k = 8;

// Tag del shard del reactor dentro de la arena.
inline constexpr std::uint64_t reactor_shard_tag_k = 0x72656163746f72ULL; // "reactor"

struct reactor_config_t {
  std::size_t queues_ = 1;                // colas nombradas del reactor
  std::size_t workers_per_queue_ = 1;     // hilos de eventos por cola (1 = affinity único)
  std::size_t max_jobs_per_queue_ = 1024; // ops en cola por queue (punteros pre-reservados)
  std::size_t max_sockets_ = max_reactor_sockets_k;
  std::size_t max_strands_ = max_reactor_strands_k;
  std::size_t max_timers_per_queue_ = 256; // timers vivos por queue
  bool affinity_ = false;                  // pinchar cada queue a su propio núcleo
  bool enable_io_ = true;                  // usar io_uring. Si el kernel no lo
                                           // expone, el reactor sigue vivo: los
                                           // timers y los strands funcionan, y
                                           // solo las operaciones de E/S fallan
}; // struct reactor_config_t

namespace detail {
struct queue_control_t;
struct socket_slot_t;
struct op_t;
struct strand_t;
} // namespace detail

} // namespace frameworkcpp::reactor
