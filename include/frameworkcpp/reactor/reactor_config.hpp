// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "frameworkcpp/arena/shard.hpp"
#include "frameworkcpp/concurrency.hpp"

namespace frameworkcpp::reactor {

// Número máximo de colas nombradas soportado por un reactor.
inline constexpr std::size_t max_reactor_queues_k = 64;

// Número máximo de sockets registrados por reactor.
inline constexpr std::size_t max_reactor_sockets_k = 4096;

// Número máximo de strands por reactor.
inline constexpr std::size_t max_reactor_strands_k = 4096;

// Número de specs que el reactor carvea del arena.
inline constexpr std::size_t reactor_spec_count_k = 10;

// Tag del shard del reactor dentro de la arena.
inline constexpr std::uint64_t reactor_shard_tag_k = 0x72656163746f72ULL; // "reactor"

// Backend de eventos que usa el reactor.
//
// io_uring es el backend de performance: el kernel agrupa las syscalls y el
// hot path no las paga. epoll es el backend portable: funciona en cualquier
// Linux, incluidos los que no exponen io_uring (emulacion, kernels viejos).
enum class backend_t : std::uint8_t {
  auto_ = 0,     // io_uring si el kernel lo expone, si no epoll
  io_uring_ = 1, // exigir io_uring: bind_arena falla si no esta
  epoll_ = 2,    // exigir epoll
}; // enum class backend_t

struct reactor_config_t {
  std::size_t queues_ = 1;                // colas nombradas del reactor
  std::size_t max_jobs_per_queue_ = 1024; // ops en cola por queue (punteros pre-reservados)
  std::size_t max_sockets_ = max_reactor_sockets_k;
  std::size_t max_strands_ = max_reactor_strands_k;
  std::size_t max_timers_per_queue_ = 256; // timers vivos por queue
  bool affinity_ = false;                  // pinchar cada queue a su propio núcleo
  backend_t backend_ = backend_t::auto_;   // backend de eventos

  // Contrato de concurrencia de cada queue. single_thread_ promete un solo
  // productor y un solo consumidor; lock_free_ admite varios de cada lado con
  // atomics; thread_safe_ los serializa con un mutex.
  concurrency_model_t concurrency_ = concurrency_model_t::lock_free_;

  // Hilos que ejecutan cada queue. Cada queue sigue teniendo un solo event
  // loop por hilo; un valor mayor que 1 se rechaza por ahora porque el ring
  // de io_uring y el heap de timers no admiten varios consumidores.
  std::size_t workers_per_queue_ = 1;
}; // struct reactor_config_t

namespace detail {
struct queue_control_t;
struct socket_slot_t;
struct op_t;
struct strand_t;
} // namespace detail

} // namespace frameworkcpp::reactor
