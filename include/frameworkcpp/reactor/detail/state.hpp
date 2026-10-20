// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "frameworkcpp/reactor/detail/io_uring_ring.hpp"
#include "frameworkcpp/reactor/detail/op.hpp"
#include "frameworkcpp/reactor/detail/op_queue.hpp"
#include "frameworkcpp/reactor/detail/strand.hpp"

namespace frameworkcpp::reactor {
class queue_t;
} // namespace frameworkcpp::reactor

namespace frameworkcpp::reactor::detail {

struct socket_slot_t;

struct queue_control_t {
  io_uring_ring_t ring_;
  op_queue_t op_queue_;
  strand_t *strands_ = nullptr; // segmento propio de la queue
  std::size_t num_strands_ = 0; // tamaño del segmento
  op_t *ops_segment_ = nullptr; // segmento propio de operaciones
  std::size_t ops_segment_size_ = 0;
  socket_slot_t *sockets_ = nullptr;
  std::size_t num_sockets_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  bool running_ = false;
}; // struct queue_control_t

struct socket_slot_t {
  int fd_ = -1;
  queue_t *queue_ = nullptr;
  std::uint64_t socket_id_ = 0;
  // Operaciones en vuelo para este socket. While it is not zero the socket
  // cannot be migrated to another queue.
  std::atomic<std::uint32_t> inflight_{0};
}; // struct socket_slot_t

struct reactor_state_t {
  queue_control_t *queues_ = nullptr;
  queue_t *queue_handles_ = nullptr;
  op_t *ops_ = nullptr;
  op_t **op_ring_ = nullptr;
  socket_slot_t *sockets_ = nullptr;
  strand_t *strands_ = nullptr;
  std::size_t num_queues_ = 0;
  std::size_t num_sockets_ = 0;
  std::size_t num_strands_ = 0;
  std::atomic<bool> running_{false};
}; // struct reactor_state_t

} // namespace frameworkcpp::reactor::detail
