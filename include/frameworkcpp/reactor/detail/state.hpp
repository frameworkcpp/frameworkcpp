// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/epoll.h>
#include <thread>

#include "frameworkcpp/reactor/detail/epoll_ring.hpp"
#include "frameworkcpp/reactor/detail/io_uring_ring.hpp"
#include "frameworkcpp/reactor/detail/op.hpp"
#include "frameworkcpp/reactor/detail/op_queue.hpp"
#include "frameworkcpp/reactor/detail/strand.hpp"
#include "frameworkcpp/reactor/detail/timer_wheel.hpp"

namespace frameworkcpp::reactor {
class queue_t;
} // namespace frameworkcpp::reactor

namespace frameworkcpp::reactor::detail {

struct socket_slot_t;

struct queue_control_t {
  // Backends de eventos. Solo uno está activo por queue.
  io_uring_ring_t ring_;
  epoll_ring_t epoll_;
  bool use_epoll_ = false;

  op_queue_t op_queue_;
  strand_t *strands_ = nullptr; // segmento propio de la queue
  std::size_t num_strands_ = 0;
  op_t *ops_segment_ = nullptr; // segmento propio de operaciones
  std::size_t ops_segment_size_ = 0;
  op_t *timers_segment_ = nullptr; // segmento propio de timers
  std::size_t timers_segment_size_ = 0;
  timer_wheel_t timers_;
  // Timers vivos: agendados, esperando o en el heap. El conteo del heap solo
  // no alcanza porque un timer recien agendado aun no entro.
  std::atomic<std::size_t> timers_live_{0};

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
  // Dirección del emisor del último datagrama recibido.
  sockaddr_storage peer_{};
  socklen_t peer_len_ = 0;
  // Una operacion pendiente por direccion. El backend de epoll necesita al
  // maximo una por direccion porque el interest se registra por descriptor.
  // Son atomicos porque el hilo que cierra el socket los consulta mientras la
  // queue los escribe.
  std::atomic<op_t *> pending_read_{nullptr};
  std::atomic<op_t *> pending_write_{nullptr};
  // Eventos registrados en epoll para este descriptor.
  std::uint32_t interest_ = 0;
  // El descriptor se cierra cuando termine la operacion en vuelo.
  std::atomic<bool> close_pending_{false};
  // El socket ya fue cerrado: cierra de nuevo no hace nada.
  std::atomic<bool> closed_{false};
}; // struct socket_slot_t

struct reactor_state_t {
  queue_control_t *queues_ = nullptr;
  queue_t *queue_handles_ = nullptr;
  op_t *ops_ = nullptr;
  op_t **op_ring_ = nullptr;
  op_t *timers_ = nullptr;
  op_t **timer_heap_ = nullptr;
  socket_slot_t *sockets_ = nullptr;
  strand_t *strands_ = nullptr;
  struct epoll_event *events_ = nullptr;
  std::size_t num_queues_ = 0;
  std::size_t num_sockets_ = 0;
  std::size_t num_strands_ = 0;
  std::size_t num_timers_ = 0;
  std::atomic<bool> running_{false};
}; // struct reactor_state_t

} // namespace frameworkcpp::reactor::detail
