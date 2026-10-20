// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Implementación del reactor. Se incluye al final de reactor.hpp: necesita
// los tipos completos de la API pública, por eso no lo incluye él mismo.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/reactor/detail/io_uring_ring.hpp"
#include "frameworkcpp/reactor/detail/op.hpp"
#include "frameworkcpp/reactor/detail/op_queue.hpp"
#include "frameworkcpp/reactor/detail/strand.hpp"

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
                                              sizeof(detail::strand_t) * config.max_strands_,
                                              alignof(detail::strand_t));

  if (_queues_raw == nullptr || _handles_raw == nullptr || _ops_raw == nullptr ||
      _ring_raw == nullptr || _sockets_raw == nullptr || _strands_raw == nullptr)
    return false;

  state_.queues_ = reinterpret_cast<detail::queue_control_t *>(_queues_raw);
  state_.queue_handles_ = reinterpret_cast<queue_t *>(_handles_raw);
  state_.ops_ = reinterpret_cast<detail::op_t *>(_ops_raw);
  state_.op_ring_ = reinterpret_cast<detail::op_t **>(_ring_raw);
  state_.sockets_ = reinterpret_cast<detail::socket_slot_t *>(_sockets_raw);
  state_.strands_ = reinterpret_cast<detail::strand_t *>(_strands_raw);
  state_.num_queues_ = config.queues_;
  state_.num_sockets_ = config.max_sockets_;

  for (std::size_t _index = 0; _index < config.max_strands_; ++_index)
    new (static_cast<void *>(&state_.strands_[_index])) detail::strand_t();

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
    state_.queues_[_index].strands_ = state_.strands_;
    state_.queues_[_index].num_strands_ = config.max_strands_;
    state_.queues_[_index].ops_segment_ = &state_.ops_[_index * config.max_jobs_per_queue_];
    state_.queues_[_index].ops_segment_size_ = config.max_jobs_per_queue_;

    new (static_cast<void *>(&state_.queue_handles_[_index])) queue_t();
    state_.queue_handles_[_index].control_ = &state_.queues_[_index];
    state_.queue_handles_[_index].id_ = static_cast<std::uint64_t>(_index) + 1;
    state_.queue_handles_[_index].name_ = "";
    state_.queue_handles_[_index].ops_segment_ = state_.queues_[_index].ops_segment_;
    state_.queue_handles_[_index].ops_segment_size_ = config.max_jobs_per_queue_;
  }

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

inline void reactor_t::run_all() noexcept {
  if (!bound_)
    return;
  state_.running_.store(true, std::memory_order_release);
  for (std::size_t _index = 0; _index < state_.num_queues_; ++_index) {
    state_.queues_[_index].running_ = true;
    state_.queues_[_index].thread_ = std::thread([this, _index]() { this->run_queue(_index); });
  }
}

inline void reactor_t::run_queue(std::size_t index) const noexcept {
  auto *_qc = &this->state_.queues_[index];
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

    // 3. Esperar completaciones. El timeout permite revisar el flag de stop.
    struct io_uring_cqe *_cqe = nullptr;
    if (_qc->ring_.wait_cqe(&_cqe, 1000000L))
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
  const auto _res_k = cqe->res;
  _op->result_.ec_ = (_res_k < 0) ? static_cast<int>(-_res_k) : 0;
  _op->result_.bytes_ = (_res_k >= 0) ? static_cast<std::size_t>(_res_k) : 0;
  if (_op->fn_ != nullptr)
    _op->fn_(_op->ctx_, _op->result_);
  if (_op->strand_id_ != 0) {
    const auto _strand_index_k = static_cast<std::size_t>(_op->strand_id_ % qc->num_strands_);
    qc->strands_[_strand_index_k].release();
  }
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
    sockaddr _addr{};
    socklen_t _addr_len = sizeof(_addr);
    io_uring_prep_accept(_sqe, op->fd_, &_addr, &_addr_len, 0);
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
    io_uring_prep_connect(_sqe, op->fd_, nullptr, 0);
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
                          int priority) noexcept {
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
      _op->priority_ = priority;
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

inline bool socket_t::recv(
    void *buffer, std::size_t len, reactor_fn_t fn, void *ctx, std::uint64_t strand_id) noexcept {
  if (queue_ == nullptr)
    return false;
  return queue_->post(op_kind_t::recv_, fd_, buffer, len, fn, ctx, strand_id, 0);
}

inline bool socket_t::send(const void *buffer,
                           std::size_t len,
                           reactor_fn_t fn,
                           void *ctx,
                           std::uint64_t strand_id) noexcept {
  if (queue_ == nullptr)
    return false;
  return queue_->post(
      op_kind_t::send_, fd_, const_cast<void *>(buffer), len, fn, ctx, strand_id, 0);
}

inline bool socket_t::accept(reactor_fn_t fn, void *ctx, std::uint64_t strand_id) noexcept {
  if (queue_ == nullptr)
    return false;
  return queue_->post(op_kind_t::accept_, fd_, nullptr, 0, fn, ctx, strand_id, 0);
}

inline bool socket_t::connect(const sockaddr *addr,
                              std::size_t addr_len,
                              reactor_fn_t fn,
                              void *ctx,
                              std::uint64_t strand_id) noexcept {
  (void) addr;
  (void) addr_len;
  if (queue_ == nullptr)
    return false;
  return queue_->post(op_kind_t::connect_, fd_, nullptr, 0, fn, ctx, strand_id, 0);
}

inline void socket_t::close() noexcept {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = -1;
}

} // namespace frameworkcpp::reactor
