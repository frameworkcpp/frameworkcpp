// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Implementación del reactor. Se incluye al final de reactor.hpp: necesita
// los tipos completos de la API pública, por eso no lo incluye él mismo.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <new>
#include <sys/un.h>
#include <thread>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/reactor/detail/affinity.hpp"
#include "frameworkcpp/reactor/detail/io_uring_ring.hpp"
#include "frameworkcpp/reactor/detail/op.hpp"
#include "frameworkcpp/reactor/detail/op_queue.hpp"
#include "frameworkcpp/reactor/detail/strand.hpp"
#include "frameworkcpp/reactor/detail/timer_wheel.hpp"

namespace frameworkcpp::reactor::detail {

// Localiza el slot de un socket por su id. Devuelve nullptr si el id no
// corresponde a ningún socket registrado.
inline socket_slot_t *find_slot(reactor_state_t *state, std::uint64_t socket_id) noexcept {
  if (socket_id == 0 || socket_id > state->num_sockets_)
    return nullptr;
  return &state->sockets_[socket_id - 1];
}

} // namespace frameworkcpp::reactor::detail

namespace frameworkcpp::reactor {

inline bool io_uring_available() noexcept {
  detail::io_uring_ring_t _ring{};
  return _ring.init(2);
}

inline reactor_t::~reactor_t() {
  stop_all();
}

inline bool reactor_t::bind_arena(arena::arena_t &arena, const reactor_config_t &config) noexcept {
  if (bound_)
    return false;

  arena::object_spec_t _specs[reactor_spec_count_k] = {};
  make_specs(config, _specs);

  const std::size_t _total_ops_k = config.queues_ * config.max_jobs_per_queue_;
  const std::size_t _total_strands_k = config.queues_ * config.max_strands_;
  const std::size_t _total_timers_k = config.queues_ * config.max_timers_per_queue_;

  arena::arena_shard_t _shard{};
  try {
    _shard = arena.create_shard(
        reactor_shard_tag_k, std::span<const arena::object_spec_t>(_specs, reactor_spec_count_k));
  } catch (...) {
    return false;
  }
  if (!_shard)
    return false;

  auto *_queues_raw = _shard.allocate_tagged(reactor_shard_tag_k,
                                             sizeof(detail::queue_control_t) * config.queues_,
                                             alignof(detail::queue_control_t));
  auto *_handles_raw = _shard.allocate_tagged(
      reactor_shard_tag_k + 1, sizeof(queue_t) * config.queues_, alignof(queue_t));
  auto *_ops_raw = _shard.allocate_tagged(
      reactor_shard_tag_k + 2, sizeof(detail::op_t) * _total_ops_k, alignof(detail::op_t));
  auto *_ring_raw = _shard.allocate_tagged(
      reactor_shard_tag_k + 3, sizeof(detail::op_t *) * _total_ops_k, alignof(detail::op_t *));
  auto *_sockets_raw = _shard.allocate_tagged(reactor_shard_tag_k + 4,
                                              sizeof(detail::socket_slot_t) * config.max_sockets_,
                                              alignof(detail::socket_slot_t));
  auto *_strands_raw = _shard.allocate_tagged(reactor_shard_tag_k + 5,
                                              sizeof(detail::strand_t) * _total_strands_k,
                                              alignof(detail::strand_t));

  auto *_timers_raw = _shard.allocate_tagged(
      reactor_shard_tag_k + 6, sizeof(detail::op_t) * _total_timers_k, alignof(detail::op_t));
  auto *_timer_heap_raw = _shard.allocate_tagged(
      reactor_shard_tag_k + 7, sizeof(detail::op_t *) * _total_timers_k, alignof(detail::op_t *));

  if (_queues_raw == nullptr || _handles_raw == nullptr || _ops_raw == nullptr ||
      _ring_raw == nullptr || _sockets_raw == nullptr || _strands_raw == nullptr ||
      _timers_raw == nullptr || _timer_heap_raw == nullptr)
    return false;

  state_.queues_ = reinterpret_cast<detail::queue_control_t *>(_queues_raw);
  state_.queue_handles_ = reinterpret_cast<queue_t *>(_handles_raw);
  state_.ops_ = reinterpret_cast<detail::op_t *>(_ops_raw);
  state_.op_ring_ = reinterpret_cast<detail::op_t **>(_ring_raw);
  state_.sockets_ = reinterpret_cast<detail::socket_slot_t *>(_sockets_raw);
  state_.strands_ = reinterpret_cast<detail::strand_t *>(_strands_raw);
  state_.timers_ = reinterpret_cast<detail::op_t *>(_timers_raw);
  state_.timer_heap_ = reinterpret_cast<detail::op_t **>(_timer_heap_raw);
  state_.num_queues_ = config.queues_;
  state_.num_sockets_ = config.max_sockets_;
  state_.num_strands_ = config.max_strands_;
  state_.num_timers_ = config.max_timers_per_queue_;

  for (std::size_t _index = 0; _index < _total_strands_k; ++_index)
    new (static_cast<void *>(&state_.strands_[_index])) detail::strand_t();

  for (std::size_t _index = 0; _index < _total_timers_k; ++_index) {
    new (static_cast<void *>(&state_.timers_[_index])) detail::op_t();
    state_.timers_[_index].done_ = true; // libre desde el arranque
  }

  for (std::size_t _index = 0; _index < config.max_sockets_; ++_index) {
    new (static_cast<void *>(&state_.sockets_[_index])) detail::socket_slot_t();
    state_.sockets_[_index].fd_ = -1;
  }

  for (std::size_t _index = 0; _index < _total_ops_k; ++_index) {
    new (static_cast<void *>(&state_.ops_[_index])) detail::op_t();
    state_.ops_[_index].done_ = true; // libre desde el arranque
  }

  for (std::size_t _index = 0; _index < config.queues_; ++_index) {
    new (static_cast<void *>(&state_.queues_[_index])) detail::queue_control_t();
    state_.queues_[_index].op_queue_.init(&state_.op_ring_[_index * config.max_jobs_per_queue_],
                                          config.max_jobs_per_queue_);
    if (!state_.queues_[_index].ring_.init(static_cast<std::uint32_t>(config.max_jobs_per_queue_)))
      return false;
    state_.queues_[_index].strands_ = &state_.strands_[_index * config.max_strands_];
    state_.queues_[_index].num_strands_ = config.max_strands_;
    state_.queues_[_index].sockets_ = state_.sockets_;
    state_.queues_[_index].num_sockets_ = config.max_sockets_;
    state_.queues_[_index].ops_segment_ = &state_.ops_[_index * config.max_jobs_per_queue_];
    state_.queues_[_index].ops_segment_size_ = config.max_jobs_per_queue_;
    state_.queues_[_index].timers_segment_ = &state_.timers_[_index * config.max_timers_per_queue_];
    state_.queues_[_index].timers_segment_size_ = config.max_timers_per_queue_;
    state_.queues_[_index].timers_.init(&state_.timer_heap_[_index * config.max_timers_per_queue_],
                                        config.max_timers_per_queue_);

    new (static_cast<void *>(&state_.queue_handles_[_index])) queue_t();
    state_.queue_handles_[_index].control_ = &state_.queues_[_index];
    state_.queue_handles_[_index].id_ = static_cast<std::uint64_t>(_index) + 1;
    state_.queue_handles_[_index].name_ = "";
    state_.queue_handles_[_index].ops_segment_ = state_.queues_[_index].ops_segment_;
    state_.queue_handles_[_index].ops_segment_size_ = config.max_jobs_per_queue_;
  }

  affinity_ = config.affinity_;
  bound_ = true;
  return true;
}

inline bool reactor_t::initialized() const noexcept {
  return bound_;
}

inline queue_t *reactor_t::get_or_create(std::string_view name) const noexcept {
  if (!bound_)
    return nullptr;
  for (std::size_t _index = 0; _index < state_.num_queues_; ++_index) {
    if (state_.queue_handles_[_index].name_ == name)
      return &state_.queue_handles_[_index];
  }
  for (std::size_t _index = 0; _index < state_.num_queues_; ++_index) {
    if (state_.queue_handles_[_index].name_.empty()) {
      state_.queue_handles_[_index].name_ = name;
      return &state_.queue_handles_[_index];
    }
  }
  return nullptr;
}

inline socket_t reactor_t::create_socket(int fd, queue_t *queue) const noexcept {
  socket_t _socket{};
  if (!bound_ || queue == nullptr || fd < 0)
    return _socket;
  for (std::size_t _index = 0; _index < state_.num_sockets_; ++_index) {
    if (state_.sockets_[_index].fd_ < 0) {
      state_.sockets_[_index].fd_ = fd;
      state_.sockets_[_index].queue_ = queue;
      state_.sockets_[_index].socket_id_ = _index + 1;
      _socket.fd_ = fd;
      _socket.id_ = state_.sockets_[_index].socket_id_;
      _socket.queue_ = queue;
      return _socket;
    }
  }
  return _socket;
}

inline void reactor_t::release_socket(detail::queue_control_t *qc,
                                      std::uint64_t socket_id) noexcept {
  if (socket_id == 0 || socket_id > qc->num_sockets_)
    return;
  qc->sockets_[socket_id - 1].inflight_.fetch_sub(1, std::memory_order_release);
}

// Reloj monotónico en nanosegundos. Es la base de todos los deadlines.
inline std::uint64_t monotonic_now_ns() noexcept {
  struct timespec _ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &_ts);
  return (static_cast<std::uint64_t>(_ts.tv_sec) * 1000000000ULL) +
         static_cast<std::uint64_t>(_ts.tv_nsec);
}

inline std::uint64_t reactor_t::schedule_timer(queue_t *queue,
                                               std::uint64_t delay_ns,
                                               reactor_fn_t fn,
                                               void *ctx,
                                               std::uint64_t strand_id) noexcept {
  if (!bound_ || queue == nullptr || queue->control_ == nullptr)
    return 0;
  detail::queue_control_t *_qc = queue->control_;

  // Un op libre del segmento de timers de esta queue.
  detail::op_t *_op = nullptr;
  for (std::size_t _index = 0; _index < _qc->timers_segment_size_; ++_index) {
    if (_qc->timers_segment_[_index].done_) {
      _op = &_qc->timers_segment_[_index];
      break;
    }
  }
  if (_op == nullptr)
    return 0;

  const std::uint64_t _id_k = this->next_timer_id();
  _op->kind_ = op_kind_t::timer_;
  _op->fd_ = -1;
  _op->buffer_ = nullptr;
  _op->len_ = 0;
  _op->fn_ = fn;
  _op->ctx_ = ctx;
  _op->strand_id_ = strand_id;
  _op->socket_id_ = 0;
  _op->priority_ = 0;
  _op->result_ = op_result_t{};
  _op->next_ = nullptr;
  _op->timer_id_ = _id_k;
  _op->deadline_ns_ = monotonic_now_ns() + delay_ns;
  _op->done_ = false;
  _op->heap_index_ = 0;

  if (!_qc->timers_.insert(_op)) {
    _op->done_ = true;
    return 0;
  }
  return _id_k;
}

inline bool reactor_t::cancel_timer(queue_t *queue, std::uint64_t timer_id) const noexcept {
  if (!bound_ || queue == nullptr || queue->control_ == nullptr || timer_id == 0)
    return false;
  detail::queue_control_t *_qc = queue->control_;
  for (std::size_t _index = 0; _index < _qc->timers_segment_size_; ++_index) {
    detail::op_t *_op = &_qc->timers_segment_[_index];
    if (_op->done_ || _op->timer_id_ != timer_id)
      continue;
    if (_qc->timers_.remove(_op)) {
      _op->done_ = true;
      _op->timer_id_ = 0;
      return true;
    }
  }
  return false;
}

inline std::size_t reactor_t::get_timer_count(queue_t *queue) const noexcept {
  if (!bound_ || queue == nullptr || queue->control_ == nullptr)
    return 0;
  return queue->control_->timers_.size();
}

inline std::uint64_t reactor_t::next_timer_id() noexcept {
  return timer_id_.fetch_add(1, std::memory_order_relaxed);
}

inline socket_t
reactor_t::open_socket(int family, int type, int protocol, queue_t *queue) const noexcept {
  socket_t _socket{};
  if (!bound_ || queue == nullptr)
    return _socket;
  const int _fd_k = ::socket(family, type, protocol);
  if (_fd_k < 0)
    return _socket;
  _socket = create_socket(_fd_k, queue);
  if (!_socket) {
    ::close(_fd_k);
    return _socket;
  }
  return _socket;
}

inline socket_t reactor_t::listen_tcp(queue_t *queue,
                                      const sockaddr *addr,
                                      std::size_t addr_len,
                                      int backlog) const noexcept {
  socket_t _socket = open_socket(AF_INET, SOCK_STREAM, 0, queue);
  if (!_socket)
    return _socket;
  if (!_socket.bind_to(addr, addr_len) || !_socket.listen_to(backlog)) {
    _socket.close();
    return socket_t{};
  }
  return _socket;
}

inline socket_t
reactor_t::listen_unix(queue_t *queue, const char *path, int backlog) const noexcept {
  socket_t _socket = open_socket(AF_UNIX, SOCK_STREAM, 0, queue);
  if (!_socket)
    return _socket;
  sockaddr_un _addr{};
  _addr.sun_family = AF_UNIX;
  std::strncpy(_addr.sun_path, path, sizeof(_addr.sun_path) - 1);
  if (!_socket.bind_to(reinterpret_cast<const sockaddr *>(&_addr), sizeof(_addr)) ||
      !_socket.listen_to(backlog)) {
    _socket.close();
    ::unlink(path);
    return socket_t{};
  }
  return _socket;
}

inline socket_t
reactor_t::bind_udp(queue_t *queue, const sockaddr *addr, std::size_t addr_len) const noexcept {
  socket_t _socket = open_socket(AF_INET, SOCK_DGRAM, 0, queue);
  if (!_socket)
    return _socket;
  if (!_socket.bind_to(addr, addr_len)) {
    _socket.close();
    return socket_t{};
  }
  return _socket;
}

inline socket_t reactor_t::open_icmp(queue_t *queue) const noexcept {
  return open_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP, queue);
}

inline bool reactor_t::migrate(socket_t &socket, queue_t *target) noexcept {
  if (!bound_ || target == nullptr || socket.id_ == 0)
    return false;
  detail::socket_slot_t *_slot = detail::find_slot(&state_, socket.id_);
  if (_slot == nullptr || _slot->fd_ < 0)
    return false;
  // Con operaciones en vuelo el socket pertenece a la queue que las atiende:
  // mudarlo ahora dejaría una conexión con dos dueños.
  if (_slot->inflight_.load(std::memory_order_acquire) != 0)
    return false;
  if (_slot->queue_ == target)
    return true;
  _slot->queue_ = target;
  socket.queue_ = target;
  return true;
}

inline std::size_t reactor_t::get_socket_count(queue_t *queue) const noexcept {
  if (!bound_ || queue == nullptr)
    return 0;
  std::size_t _count = 0;
  for (std::size_t _index = 0; _index < state_.num_sockets_; ++_index) {
    if (state_.sockets_[_index].fd_ >= 0 && state_.sockets_[_index].queue_ == queue)
      ++_count;
  }
  return _count;
}

inline std::size_t reactor_t::rebalance() const noexcept {
  if (!bound_ || state_.num_queues_ < 2)
    return 0;

  std::size_t _counts[max_reactor_queues_k] = {};
  if (state_.num_queues_ > max_reactor_queues_k)
    return 0;

  for (std::size_t _index = 0; _index < state_.num_sockets_; ++_index) {
    const detail::socket_slot_t &_slot = state_.sockets_[_index];
    if (_slot.fd_ < 0 || _slot.queue_ == nullptr)
      continue;
    const std::uint64_t _queue_id_k = _slot.queue_->id();
    if (_queue_id_k == 0 || _queue_id_k > max_reactor_queues_k)
      continue;
    ++_counts[_queue_id_k - 1];
  }

  std::size_t _busiest = 0;
  std::size_t _quietest = 0;
  for (std::size_t _index = 1; _index < state_.num_queues_; ++_index) {
    if (_counts[_index] > _counts[_busiest])
      _busiest = _index;
    if (_counts[_index] < _counts[_quietest])
      _quietest = _index;
  }
  if (_counts[_busiest] - _counts[_quietest] <= 1)
    return 0;

  queue_t *_target = &state_.queue_handles_[_quietest];
  std::size_t _moved = 0;
  for (std::size_t _index = 0;
       _index < state_.num_sockets_ && _counts[_busiest] > _counts[_quietest] + 1;
       ++_index) {
    detail::socket_slot_t &_slot = state_.sockets_[_index];
    if (_slot.fd_ < 0 || _slot.queue_ == nullptr)
      continue;
    if (_slot.queue_->id() != _busiest + 1)
      continue;
    if (_slot.inflight_.load(std::memory_order_acquire) != 0)
      continue; // busy: se moverá en un rebalanceo posterior
    _slot.queue_ = _target;
    --_counts[_busiest];
    ++_counts[_quietest];
    ++_moved;
  }
  return _moved;
}

inline void reactor_t::run_all() noexcept {
  if (!bound_)
    return;
  state_.running_.store(true, std::memory_order_release);
  for (std::size_t _index = 0; _index < state_.num_queues_; ++_index) {
    state_.queues_[_index].running_ = true;
    state_.queues_[_index].thread_ = std::thread([this, _index]() { this->run_queue(_index); });
  }
}

inline void reactor_t::expire_timers(detail::queue_control_t *qc) noexcept {
  const std::uint64_t _now_k = monotonic_now_ns();
  for (;;) {
    detail::op_t *_op = qc->timers_.pop();
    if (_op == nullptr)
      return;
    if (_op->deadline_ns_ > _now_k) {
      // Todavía no vence: vuelve al heap.
      (void) qc->timers_.insert(_op);
      return;
    }
    if (_op->strand_id_ != 0) {
      // Un timer con strand no puede ejecutarse mientras haya otra op del
      // mismo strand en vuelo: entra en la cola del strand y lo despacha el
      // propio hilo de la queue, igual que cualquier otra operación.
      const auto _strand_index_k = static_cast<std::size_t>(_op->strand_id_ % qc->num_strands_);
      qc->strands_[_strand_index_k].enqueue(_op);
      continue;
    }
    _op->result_.ec_ = 0;
    _op->result_.bytes_ = 0;
    if (_op->fn_ != nullptr)
      _op->fn_(_op->ctx_, _op->result_);
    _op->done_ = true;
    _op->timer_id_ = 0;
  }
}

inline void reactor_t::run_queue(std::size_t index) const noexcept {
  auto *_qc = &this->state_.queues_[index];
  if (affinity_)
    (void) detail::pin_thread_to_cpu(index);
  while (!_qc->stop_.load(std::memory_order_acquire)) {
    // 1. Desencadenar strands listos: un op por strand, en orden.
    for (std::size_t _strand = 0; _strand < _qc->num_strands_; ++_strand) {
      auto *_strand_op = _qc->strands_[_strand].try_dispatch();
      if (_strand_op != nullptr)
        reactor_t::execute_op(_qc, _strand_op);
    }

    // 2. Consumir la cola de ops pendientes.
    auto *_op = _qc->op_queue_.pop();
    if (_op != nullptr) {
      if (_op->strand_id_ != 0) {
        const auto _strand_index_k = static_cast<std::size_t>(_op->strand_id_ % _qc->num_strands_);
        _qc->strands_[_strand_index_k].enqueue(_op);
      } else {
        reactor_t::execute_op(_qc, _op);
      }
      continue;
    }

    // 3. Expirar los timers vencidos.
    reactor_t::expire_timers(_qc);

    // 4. Esperar completaciones. El timeout permite revisar el flag de stop y
    //    despertar a tiempo para el siguiente deadline.
    long _timeout_ns = 1000000L;
    const std::uint64_t _next_k = _qc->timers_.next_deadline();
    if (_next_k != 0) {
      const std::uint64_t _now_k = monotonic_now_ns();
      if (_next_k > _now_k) {
        const std::uint64_t _delta_k = _next_k - _now_k;
        _timeout_ns = (_delta_k < 1000000ULL) ? static_cast<long>(_delta_k) : 1000000L;
      } else {
        _timeout_ns = 0;
      }
    }
    struct io_uring_cqe *_cqe = nullptr;
    if (_qc->ring_.wait_cqe(&_cqe, _timeout_ns))
      reactor_t::complete_op(_qc, _cqe);
  }
  _qc->running_ = false;
}

inline void reactor_t::complete_op(detail::queue_control_t *qc, struct io_uring_cqe *cqe) noexcept {
  // user_data lleva el índice de la op dentro del segmento de la queue: un
  // entero, no un puntero, así que no hay cast de entero a puntero.
  const auto _index_k = static_cast<std::size_t>(cqe->user_data);
  qc->ring_.cqe_seen(cqe);
  if (_index_k >= qc->ops_segment_size_)
    return;
  auto *_op = &qc->ops_segment_[_index_k];
  const long _res_k = cqe->res;
  _op->result_.ec_ = (_res_k < 0) ? static_cast<int>(-_res_k) : 0;
  _op->result_.bytes_ = (_res_k >= 0) ? static_cast<std::size_t>(_res_k) : 0;
  // En accept, el resultado del kernel es el descriptor de la conexión
  // aceptada: es lo que el handler necesita para registrarlo.
  _op->result_.fd_ =
      (_op->kind_ == op_kind_t::accept_ && _res_k >= 0) ? static_cast<int>(_res_k) : -1;
  // En recvfrom, el kernel dejó la dirección del emisor en el op.
  if (_op->kind_ == op_kind_t::recvfrom_ && _op->socket_id_ != 0 &&
      _op->socket_id_ <= qc->num_sockets_) {
    const socklen_t _peer_len_k = _op->message_.msg_namelen;
    if (_peer_len_k != 0) {
      qc->sockets_[_op->socket_id_ - 1].peer_ = _op->peer_;
      qc->sockets_[_op->socket_id_ - 1].peer_len_ = _peer_len_k;
    }
  }
  if (_op->fn_ != nullptr)
    _op->fn_(_op->ctx_, _op->result_);
  if (_op->strand_id_ != 0) {
    const auto _strand_index_k = static_cast<std::size_t>(_op->strand_id_ % qc->num_strands_);
    qc->strands_[_strand_index_k].release();
  }
  release_socket(qc, _op->socket_id_);
  _op->done_ = true;
}

inline void reactor_t::execute_op(detail::queue_control_t *qc, detail::op_t *op) noexcept {
  const auto _index_k = static_cast<std::uint64_t>(op - qc->ops_segment_);
  switch (op->kind_) {
  case op_kind_t::recv_: {
    auto *_sqe = qc->ring_.acquire_sqe();
    if (_sqe == nullptr) {
      qc->op_queue_.push(op);
      return;
    }
    io_uring_prep_recv(_sqe, op->fd_, op->buffer_, op->len_, 0);
    _sqe->user_data = _index_k;
    (void) qc->ring_.submit(1);
    break;
  }
  case op_kind_t::send_: {
    auto *_sqe = qc->ring_.acquire_sqe();
    if (_sqe == nullptr) {
      qc->op_queue_.push(op);
      return;
    }
    io_uring_prep_send(_sqe, op->fd_, op->buffer_, op->len_, 0);
    _sqe->user_data = _index_k;
    (void) qc->ring_.submit(1);
    break;
  }
  case op_kind_t::accept_: {
    auto *_sqe = qc->ring_.acquire_sqe();
    if (_sqe == nullptr) {
      qc->op_queue_.push(op);
      return;
    }
    // La dirección del peer se escribe en el propio op: es el único sitio
    // donde puede vivir hasta que llegue la completación.
    op->peer_len_ = sizeof(sockaddr_storage);
    io_uring_prep_accept(
        _sqe, op->fd_, reinterpret_cast<sockaddr *>(&op->peer_), &op->peer_len_, 0);
    _sqe->user_data = _index_k;
    (void) qc->ring_.submit(1);
    break;
  }
  case op_kind_t::sendto_: {
    auto *_sqe = qc->ring_.acquire_sqe();
    if (_sqe == nullptr) {
      qc->op_queue_.push(op);
      return;
    }
    io_uring_prep_sendto(_sqe,
                         op->fd_,
                         op->buffer_,
                         op->len_,
                         0,
                         reinterpret_cast<const sockaddr *>(&op->peer_),
                         op->peer_len_);
    _sqe->user_data = _index_k;
    (void) qc->ring_.submit(1);
    break;
  }
  case op_kind_t::recvfrom_: {
    auto *_sqe = qc->ring_.acquire_sqe();
    if (_sqe == nullptr) {
      qc->op_queue_.push(op);
      return;
    }
    // recvmsg deja el datagrama en el buffer y la dirección del emisor en
    // msg_name, que apunta al almacenamiento del propio op.
    op->iov_.iov_base = op->buffer_;
    op->iov_.iov_len = op->len_;
    op->message_.msg_name = &op->peer_;
    op->message_.msg_namelen = sizeof(op->peer_);
    op->message_.msg_iov = &op->iov_;
    op->message_.msg_iovlen = 1;
    io_uring_prep_recvmsg(_sqe, op->fd_, &op->message_, 0);
    _sqe->user_data = _index_k;
    (void) qc->ring_.submit(1);
    break;
  }
  case op_kind_t::connect_: {
    auto *_sqe = qc->ring_.acquire_sqe();
    if (_sqe == nullptr) {
      qc->op_queue_.push(op);
      return;
    }
    io_uring_prep_connect(
        _sqe, op->fd_, reinterpret_cast<const sockaddr *>(&op->peer_), op->peer_len_);
    _sqe->user_data = _index_k;
    (void) qc->ring_.submit(1);
    break;
  }
  case op_kind_t::timer_:
  case op_kind_t::custom_:
  default: {
    op->result_.ec_ = 0;
    op->result_.bytes_ = 0;
    if (op->fn_ != nullptr)
      op->fn_(op->ctx_, op->result_);
    if (op->strand_id_ != 0) {
      const auto _strand_index_k = static_cast<std::size_t>(op->strand_id_ % qc->num_strands_);
      qc->strands_[_strand_index_k].release();
    }
    release_socket(qc, op->socket_id_);
    op->done_ = true;
    break;
  }
  }
}

inline void reactor_t::stop_all() noexcept {
  if (!bound_)
    return;
  state_.running_.store(false, std::memory_order_release);
  for (std::size_t _index = 0; _index < state_.num_queues_; ++_index) {
    state_.queues_[_index].stop_.store(true, std::memory_order_release);
    if (state_.queues_[_index].thread_.joinable())
      state_.queues_[_index].thread_.join();
  }
  bound_ = false;
}

// --- queue_t ---

inline bool queue_t::post(op_kind_t kind,
                          int fd,
                          void *buffer,
                          std::size_t len,
                          reactor_fn_t fn,
                          void *ctx,
                          std::uint64_t strand_id,
                          int priority,
                          std::uint64_t socket_id,
                          const sockaddr *addr,
                          std::size_t addr_len) noexcept {
  if (control_ == nullptr || ops_segment_ == nullptr)
    return false;
  for (std::size_t _index = 0; _index < ops_segment_size_; ++_index) {
    if (ops_segment_[_index].done_) {
      auto *_op = &ops_segment_[_index];
      _op->kind_ = kind;
      _op->fd_ = fd;
      _op->buffer_ = buffer;
      _op->len_ = len;
      _op->fn_ = fn;
      _op->ctx_ = ctx;
      _op->strand_id_ = strand_id;
      _op->socket_id_ = socket_id;
      _op->priority_ = priority;
      if (addr != nullptr && addr_len > 0) {
        std::memcpy(&_op->peer_, addr, addr_len);
        _op->peer_len_ = static_cast<socklen_t>(addr_len);
      } else {
        _op->peer_len_ = 0;
      }
      _op->result_ = op_result_t{};
      _op->done_ = false;
      _op->next_ = nullptr;
      return control_->op_queue_.push(_op);
    }
  }
  return false; // pool de ops de esta queue agotado
}

inline void queue_t::run() noexcept {}

inline void queue_t::stop() noexcept {}

inline std::size_t queue_t::pending_ops() const noexcept {
  if (control_ == nullptr)
    return 0;
  return control_->op_queue_.size();
}

// --- socket_t ---

inline detail::socket_slot_t *socket_t::slot() const noexcept {
  if (queue_ == nullptr || queue_->control_ == nullptr)
    return nullptr;
  detail::queue_control_t *_qc = queue_->control_;
  if (id_ == 0 || id_ > _qc->num_sockets_)
    return nullptr;
  return &_qc->sockets_[id_ - 1];
}

inline bool socket_t::is_idle() const noexcept {
  const detail::socket_slot_t *_slot = slot();
  if (_slot == nullptr)
    return false;
  return _slot->inflight_.load(std::memory_order_acquire) == 0;
}

inline bool socket_t::post_op(op_kind_t kind,
                              void *buffer,
                              std::size_t len,
                              reactor_fn_t fn,
                              void *ctx,
                              std::uint64_t strand_id) noexcept {
  if (queue_ == nullptr)
    return false;
  detail::socket_slot_t *_slot = slot();
  if (_slot == nullptr)
    return false;
  _slot->inflight_.fetch_add(1, std::memory_order_acq_rel);
  if (!queue_->post(kind, fd_, buffer, len, fn, ctx, strand_id, 0, id_)) {
    _slot->inflight_.fetch_sub(1, std::memory_order_release);
    return false;
  }
  return true;
}

inline bool socket_t::recv(
    void *buffer, std::size_t len, reactor_fn_t fn, void *ctx, std::uint64_t strand_id) noexcept {
  return post_op(op_kind_t::recv_, buffer, len, fn, ctx, strand_id);
}

inline bool socket_t::send(const void *buffer,
                           std::size_t len,
                           reactor_fn_t fn,
                           void *ctx,
                           std::uint64_t strand_id) noexcept {
  return post_op(op_kind_t::send_, const_cast<void *>(buffer), len, fn, ctx, strand_id);
}

inline bool socket_t::accept(reactor_fn_t fn, void *ctx, std::uint64_t strand_id) noexcept {
  return post_op(op_kind_t::accept_, nullptr, 0, fn, ctx, strand_id);
}

inline bool socket_t::connect(const sockaddr *addr,
                              std::size_t addr_len,
                              reactor_fn_t fn,
                              void *ctx,
                              std::uint64_t strand_id) noexcept {
  if (queue_ == nullptr || addr == nullptr || addr_len == 0)
    return false;
  if (addr_len > sizeof(sockaddr_storage))
    return false;
  detail::socket_slot_t *_slot = slot();
  if (_slot == nullptr)
    return false;
  _slot->inflight_.fetch_add(1, std::memory_order_acq_rel);
  if (!queue_->post(
          op_kind_t::connect_, fd_, nullptr, 0, fn, ctx, strand_id, 0, id_, addr, addr_len)) {
    _slot->inflight_.fetch_sub(1, std::memory_order_release);
    return false;
  }
  return true;
}

inline bool socket_t::bind_to(const sockaddr *addr, std::size_t addr_len) const noexcept {
  if (fd_ < 0 || addr == nullptr)
    return false;
  return ::bind(fd_, addr, static_cast<socklen_t>(addr_len)) == 0;
}

inline bool socket_t::listen_to(int backlog) const noexcept {
  if (fd_ < 0)
    return false;
  return ::listen(fd_, backlog) == 0;
}

inline const sockaddr *socket_t::last_peer() const noexcept {
  const detail::socket_slot_t *_slot = slot();
  if (_slot == nullptr || _slot->peer_len_ == 0)
    return nullptr;
  return reinterpret_cast<const sockaddr *>(&_slot->peer_);
}

inline bool socket_t::send_to(const void *buffer,
                              std::size_t len,
                              const sockaddr *addr,
                              std::size_t addr_len,
                              reactor_fn_t fn,
                              void *ctx,
                              std::uint64_t strand_id) noexcept {
  if (queue_ == nullptr || addr == nullptr || addr_len == 0)
    return false;
  if (addr_len > sizeof(sockaddr_storage))
    return false;
  detail::socket_slot_t *_slot = slot();
  if (_slot == nullptr)
    return false;
  _slot->inflight_.fetch_add(1, std::memory_order_acq_rel);
  if (!queue_->post(op_kind_t::sendto_,
                    fd_,
                    const_cast<void *>(buffer),
                    len,
                    fn,
                    ctx,
                    strand_id,
                    0,
                    id_,
                    addr,
                    addr_len)) {
    _slot->inflight_.fetch_sub(1, std::memory_order_release);
    return false;
  }
  return true;
}

inline bool socket_t::recv_from(
    void *buffer, std::size_t len, reactor_fn_t fn, void *ctx, std::uint64_t strand_id) noexcept {
  return post_op(op_kind_t::recvfrom_, buffer, len, fn, ctx, strand_id);
}

inline void socket_t::close() noexcept {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = -1;
}

} // namespace frameworkcpp::reactor
