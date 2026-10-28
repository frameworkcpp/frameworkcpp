// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sys/epoll.h>
#include <thread>

#include "frameworkcpp/reactor/reactor_config.hpp"

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
  strand_t *strands_ = nullptr; // segmento propio de la queue
  std::size_t num_strands_ = 0;
  std::atomic<std::size_t> next_strand_{0}; // claves repartidas por create_strand
#ifndef NDEBUG
  // Verificacion de contrato: single_thread_ en el eje productor solo admite
  // un hilo. En release no existe ni el costo de leer el thread id.
  std::atomic<std::uint64_t> producer_thread_{0};
#endif
  op_t *ops_segment_ = nullptr; // segmento propio de operaciones
  std::size_t ops_segment_size_ = 0;
  op_t *timers_segment_ = nullptr; // segmento propio de timers
  std::size_t timers_segment_size_ = 0;
  // Timers vivos: agendados, esperando o en el heap. El conteo del heap solo
  // no alcanza porque un timer recien agendado aun no entro.
  std::atomic<std::size_t> timers_live_{0};
  socket_slot_t *sockets_ = nullptr;
  std::size_t num_sockets_ = 0;
  std::size_t workers_ = 0;
  std::array<std::thread, max_workers_per_queue_k> threads_{};

  // Backends de eventos. Solo uno está activo por queue.
  epoll_ring_t epoll_;
  struct epoll_event *events_base_ = nullptr;
  std::size_t events_per_worker_ = 0;
  timer_wheel_t timers_;
  op_queue_t op_queue_;
  io_uring_ring_t ring_;
  bool use_epoll_ = false;

  // Serializan las estructuras que comparten los workers. Con un solo worker
  // los caminos no tocan estos mutex.
  std::mutex submit_mutex_;
  std::mutex cqe_mutex_;
  std::mutex state_mutex_;
  std::mutex timer_mutex_;
#ifndef NDEBUG
  std::atomic<bool> producer_claimed_{false};
#endif
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  queue_concurrency_t concurrency_ = {};
}; // struct queue_control_t

struct socket_slot_t {
  // El descriptor lo escribe el hilo de la queue que lo cierra y lo leen los
  // hilos que registran sockets. Es atomico para que ese cruce no sea una
  // carrera de datos: el hilo que cierra publica -1 con release y el que
  // registra lo toma con acquire.
  std::atomic<int> fd_{-1};
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
