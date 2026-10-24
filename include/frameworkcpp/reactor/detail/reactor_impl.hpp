// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Implementación del reactor. Se incluye al final de reactor.hpp: necesita
// los tipos completos de la API pública, por eso no lo incluye él mismo.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
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
  if (config.queues_ == 0 || config.queues_ > max_reactor_queues_k ||
      config.max_jobs_per_queue_ == 0 ||
      (config.max_jobs_per_queue_ & (config.max_jobs_per_queue_ - 1)) != 0 ||
      config.max_sockets_ == 0 || config.max_strands_ == 0 || config.max_timers_per_queue_ == 0)
    return false;
  // Cada queue ejecuta un solo event loop: el ring de io_uring y el heap de
  // timers no admiten varios consumidores. La perilla se valida para que no
  // prometa hilos que no existen.
  if (config.workers_per_queue_ != 1)
    return false;

  // Si el bind falla, el reactor queda como si nunca se hubiera llamado: un
  // reintento no debe encontrar un estado a medias.
  const auto _reset_k = [this]() noexcept {
    state_.queues_ = nullptr;
    state_.queue_handles_ = nullptr;
    state_.ops_ = nullptr;
    state_.op_ring_ = nullptr;
    state_.sockets_ = nullptr;
    state_.strands_ = nullptr;
    state_.timers_ = nullptr;
    state_.timer_heap_ = nullptr;
    state_.num_queues_ = 0;
    state_.num_sockets_ = 0;
    state_.num_strands_ = 0;
    state_.num_timers_ = 0;
    affinity_ = false;
    concurrency_ = concurrency_model_t::lock_free_;
  };

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
    _reset_k();
    return false;
  }
  if (!_shard) {
    _reset_k();
    return false;
  }

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
  auto *_events_raw = _shard.allocate_tagged(reactor_shard_tag_k + 8,
                                             sizeof(struct epoll_event) * _total_ops_k,
                                             alignof(struct epoll_event));
  auto *_sequence_raw = _shard.allocate_tagged(reactor_shard_tag_k + 9,
                                               sizeof(std::atomic<std::size_t>) * _total_ops_k,
                                               alignof(std::atomic<std::size_t>));

  if (_queues_raw == nullptr || _handles_raw == nullptr || _ops_raw == nullptr ||
      _ring_raw == nullptr || _sockets_raw == nullptr || _strands_raw == nullptr ||
      _timers_raw == nullptr || _timer_heap_raw == nullptr || _events_raw == nullptr ||
      _sequence_raw == nullptr) {
    _reset_k();
    return false;
  }

  state_.queues_ = reinterpret_cast<detail::queue_control_t *>(_queues_raw);
  state_.queue_handles_ = reinterpret_cast<queue_t *>(_handles_raw);
  state_.ops_ = reinterpret_cast<detail::op_t *>(_ops_raw);
  state_.op_ring_ = reinterpret_cast<detail::op_t **>(_ring_raw);
  state_.sockets_ = reinterpret_cast<detail::socket_slot_t *>(_sockets_raw);
  state_.strands_ = reinterpret_cast<detail::strand_t *>(_strands_raw);
  state_.timers_ = reinterpret_cast<detail::op_t *>(_timers_raw);
  state_.timer_heap_ = reinterpret_cast<detail::op_t **>(_timer_heap_raw);
  state_.events_ = reinterpret_cast<struct epoll_event *>(_events_raw);
  state_.num_queues_ = config.queues_;
  state_.num_sockets_ = config.max_sockets_;
  state_.num_strands_ = config.max_strands_;
  state_.num_timers_ = config.max_timers_per_queue_;

  for (std::size_t _index = 0; _index < _total_strands_k; ++_index)
    new (static_cast<void *>(&state_.strands_[_index])) detail::strand_t();

  for (std::size_t _index = 0; _index < _total_timers_k; ++_index) {
    new (static_cast<void *>(&state_.timers_[_index])) detail::op_t();
    state_.timers_[_index].done_.store(true, std::memory_order_relaxed); // libre
  }

  for (std::size_t _index = 0; _index < config.max_sockets_; ++_index) {
    new (static_cast<void *>(&state_.sockets_[_index])) detail::socket_slot_t();
    state_.sockets_[_index].fd_ = -1;
  }

  for (std::size_t _index = 0; _index < _total_ops_k; ++_index) {
    new (static_cast<void *>(&state_.ops_[_index])) detail::op_t();
    state_.ops_[_index].done_.store(true, std::memory_order_relaxed); // libre
  }

  for (std::size_t _index = 0; _index < config.queues_; ++_index) {
    new (static_cast<void *>(&state_.queues_[_index])) detail::queue_control_t();
    auto *_sequence_k = reinterpret_cast<std::atomic<std::size_t> *>(
        _sequence_raw + (_index * config.max_jobs_per_queue_ * sizeof(std::atomic<std::size_t>)));
    if (!state_.queues_[_index].op_queue_.init(
            &state_.op_ring_[_index * config.max_jobs_per_queue_],
            _sequence_k,
            config.max_jobs_per_queue_,
            config.concurrency_)) {
      _reset_k();
      return false;
    }
    // Backend de eventos. io_uring es el de performance; epoll es el
    // portable y entra cuando el kernel no expone io_uring.
    const bool _want_io_uring_k = config.backend_ != backend_t::epoll_;
    const bool _want_epoll_k = config.backend_ != backend_t::io_uring_;
    if (_want_io_uring_k) {
      if (state_.queues_[_index].ring_.init(
              static_cast<std::uint32_t>(config.max_jobs_per_queue_))) {
        state_.queues_[_index].use_epoll_ = false;
      } else if (_want_epoll_k) {
        // El kernel no expone io_uring: se cae a epoll, que funciona en
        // cualquier Linux.
        state_.queues_[_index].use_epoll_ = true;
      } else {
        _reset_k();
        return false;
      }
    } else {
      state_.queues_[_index].use_epoll_ = true;
    }
    if (state_.queues_[_index].use_epoll_) {
      const std::size_t _events_offset_k = _index * config.max_jobs_per_queue_;
      if (!state_.queues_[_index].epoll_.init(&state_.events_[_events_offset_k],
                                              config.max_jobs_per_queue_)) {
        _reset_k();
        return false;
      }
    }
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
    state_.queue_handles_[_index].concurrency_ = config.concurrency_;
    state_.queue_handles_[_index].owner_ = this;
    state_.queue_handles_[_index].index_ = _index;
  }

  affinity_ = config.affinity_;
  concurrency_ = config.concurrency_;
  bound_ = true;
  return true;
}

inline bool reactor_t::initialized() const noexcept {
  return bound_;
}

inline backend_t reactor_t::get_backend() const noexcept {
  if (!bound_ || state_.num_queues_ == 0)
    return backend_t::epoll_;
  return state_.queues_[0].use_epoll_ ? backend_t::epoll_ : backend_t::io_uring_;
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

inline strand_t reactor_t::create_strand(queue_t *queue,
                                         concurrency_model_t concurrency) const noexcept {
  strand_t _strand{};
  if (!bound_ || queue == nullptr || queue->control_ == nullptr)
    return _strand;
  detail::queue_control_t *_qc = queue->control_;
  const std::size_t _index_k = _qc->next_strand_.fetch_add(1, std::memory_order_relaxed);
  if (_index_k >= _qc->num_strands_)
    return _strand;
  _qc->strands_[_index_k].set_concurrency_model(concurrency);
  _strand.id_ = static_cast<std::uint64_t>(_index_k) + 1;
  _strand.concurrency_ = concurrency;
  _strand.queue_ = queue;
  return _strand;
}

inline strand_t reactor_t::create_strand(queue_t *queue) const noexcept {
  if (queue == nullptr)
    return strand_t{};
  return create_strand(queue, queue->get_concurrency_model());
}

inline socket_t reactor_t::create_socket(int fd, queue_t *queue) const noexcept {
  socket_t _socket{};
  if (!bound_ || queue == nullptr || fd < 0)
    return _socket;
  // El reactor trabaja en modo no bloqueante: asi una operacion nunca
  // bloquea el hilo de la queue. El socket queda en ese modo para quien lo
  // use despues.
  const int _flags_k = ::fcntl(fd, F_GETFL, 0);
  if (_flags_k >= 0)
    (void) ::fcntl(fd, F_SETFL, _flags_k | O_NONBLOCK);
  for (std::size_t _index = 0; _index < state_.num_sockets_; ++_index) {
    if (state_.sockets_[_index].fd_ < 0) {
      state_.sockets_[_index].fd_ = fd;
      state_.sockets_[_index].closed_.store(false, std::memory_order_release);
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

// Reloj monotónico en nanosegundos. Es la base de todos los deadlines.
inline std::uint64_t monotonic_now_ns() noexcept {
  struct timespec _ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &_ts);
  return (static_cast<std::uint64_t>(_ts.tv_sec) * 1000000000ULL) +
         static_cast<std::uint64_t>(_ts.tv_nsec);
}

// Toma una operacion libre del segmento. El compare_exchange asegura que dos
// hilos que posteen a la vez no tomen la misma operacion.
inline detail::op_t *acquire_op(detail::op_t *segment, std::size_t count) noexcept {
  for (std::size_t _index = 0; _index < count; ++_index) {
    bool _expected_k = true;
    if (segment[_index].done_.compare_exchange_strong(
            _expected_k, false, std::memory_order_acq_rel))
      return &segment[_index];
  }
  return nullptr;
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
  detail::op_t *_op = acquire_op(_qc->timers_segment_, _qc->timers_segment_size_);
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
  _op->heap_index_ = 0;

  // El heap de timers solo lo toca el hilo de la queue: la operacion se
  // encola y ahi se inserta. Asi el heap no necesita locks.
  if (!_qc->op_queue_.push(_op)) {
    _op->done_.store(true, std::memory_order_release);
    return 0;
  }
  _qc->timers_live_.fetch_add(1, std::memory_order_acq_rel);
  return _id_k;
}

inline bool reactor_t::cancel_timer(queue_t *queue, std::uint64_t timer_id) const noexcept {
  if (!bound_ || queue == nullptr || queue->control_ == nullptr || timer_id == 0)
    return false;
  detail::queue_control_t *_qc = queue->control_;

  // El heap de timers solo lo manipula el hilo de la queue, asi que la
  // cancelacion se encola y se aplica ahi.
  detail::op_t *_op = acquire_op(_qc->timers_segment_, _qc->timers_segment_size_);
  if (_op == nullptr)
    return false;
  _op->kind_ = op_kind_t::cancel_;
  _op->timer_id_ = 0;
  _op->cancel_target_ = timer_id;
  _op->fn_ = nullptr;
  _op->ctx_ = nullptr;
  _op->strand_id_ = 0;
  _op->socket_id_ = 0;
  _op->next_ = nullptr;
  if (!_qc->op_queue_.push(_op)) {
    _op->done_.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

inline std::size_t reactor_t::get_timer_count(queue_t *queue) const noexcept {
  if (!bound_ || queue == nullptr || queue->control_ == nullptr)
    return 0;
  return queue->control_->timers_live_.load(std::memory_order_acquire);
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
    state_.queues_[_index].stop_.store(false, std::memory_order_release);
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
      // propio hilo de la queue, igual que cualquier otra operacion.
      const auto _strand_index_k = static_cast<std::size_t>(_op->strand_id_ % qc->num_strands_);
      _op->kind_ = op_kind_t::custom_;
      qc->strands_[_strand_index_k].enqueue(_op);
      continue;
    }
    _op->result_.ec_ = 0;
    _op->result_.bytes_ = 0;
    if (_op->fn_ != nullptr)
      _op->fn_(_op->ctx_, _op->result_);
    _op->done_.store(true, std::memory_order_release);
    _op->timer_id_ = 0;
    qc->timers_live_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

// --- helpers de backend ---

// Una operación de lectura ocupa la dirección de lectura del socket.
inline bool is_read_op(op_kind_t kind) noexcept {
  return kind == op_kind_t::recv_ || kind == op_kind_t::accept_ || kind == op_kind_t::recvfrom_;
}

// Una operación de escritura ocupa la dirección de escritura del socket.
inline bool is_write_op(op_kind_t kind) noexcept {
  return kind == op_kind_t::send_ || kind == op_kind_t::sendto_ || kind == op_kind_t::connect_;
}

// Sincroniza los eventos registrados en epoll con las operaciones pendientes
// del socket. Solo lo llama el hilo de la queue.
inline void update_interest(detail::queue_control_t *qc, detail::socket_slot_t *slot) noexcept {
  std::uint32_t _events = 0;
  if (slot->pending_read_.load(std::memory_order_acquire) != nullptr)
    _events |= EPOLLIN;
  if (slot->pending_write_.load(std::memory_order_acquire) != nullptr)
    _events |= EPOLLOUT;
  if (_events == slot->interest_)
    return;
  if (_events == 0) {
    if (slot->interest_ != 0)
      (void) qc->epoll_.remove(slot->fd_);
  } else if (slot->interest_ == 0) {
    if (!qc->epoll_.add(slot->fd_, _events, slot->socket_id_))
      (void) qc->epoll_.modify(slot->fd_, _events, slot->socket_id_);
  } else {
    (void) qc->epoll_.modify(slot->fd_, _events, slot->socket_id_);
  }
  slot->interest_ = _events;
}

// Suelta la reserva de E/S del socket. Vive fuera de la clase porque lo usan
// tanto los helpers libres como los metodos del reactor.
inline void release_socket(detail::queue_control_t *qc, std::uint64_t socket_id) noexcept {
  if (socket_id == 0 || socket_id > qc->num_sockets_)
    return;
  qc->sockets_[socket_id - 1].inflight_.fetch_sub(1, std::memory_order_release);
}

// Termina una operación: libera la dirección del socket, avisa al handler y
// devuelve la operación al pool.
inline void finish_op(
    detail::queue_control_t *qc, detail::op_t *op, int ec, std::size_t bytes, int fd) noexcept {
  op->result_.ec_ = ec;
  op->result_.bytes_ = bytes;
  op->result_.fd_ = fd;

  detail::socket_slot_t *_slot = nullptr;
  if (op->socket_id_ != 0 && op->socket_id_ <= qc->num_sockets_) {
    _slot = &qc->sockets_[op->socket_id_ - 1];
    if (_slot->pending_read_.load(std::memory_order_acquire) == op)
      _slot->pending_read_.store(nullptr, std::memory_order_release);
    if (_slot->pending_write_.load(std::memory_order_acquire) == op)
      _slot->pending_write_.store(nullptr, std::memory_order_release);
    // En recvfrom, el kernel dejó la dirección del emisor en el op.
    if (op->kind_ == op_kind_t::recvfrom_) {
      const socklen_t _peer_len_k = op->message_.msg_namelen;
      if (_peer_len_k != 0) {
        _slot->peer_ = op->peer_;
        _slot->peer_len_ = _peer_len_k;
      }
    }
    if (qc->use_epoll_)
      update_interest(qc, _slot);
    // Un socket cerrado con una operacion en vuelo mantiene el descriptor
    // hasta que la operacion termina: cerrarlo antes dejaria la operacion
    // sin dueño y el slot ocupado para siempre.
    if (_slot->close_pending_.load(std::memory_order_acquire) &&
        _slot->pending_read_.load(std::memory_order_acquire) == nullptr &&
        _slot->pending_write_.load(std::memory_order_acquire) == nullptr) {
      ::close(_slot->fd_);
      _slot->fd_ = -1;
      _slot->close_pending_.store(false, std::memory_order_release);
    }
  }

  if (op->fn_ != nullptr)
    op->fn_(op->ctx_, op->result_);
  if (op->strand_id_ != 0) {
    const auto _strand_index_k = static_cast<std::size_t>(op->strand_id_ % qc->num_strands_);
    qc->strands_[_strand_index_k].release();
  }
  release_socket(qc, op->socket_id_);
  op->done_.store(true, std::memory_order_release);
}

// --- backend io_uring ---

inline void reactor_t::complete_op(detail::queue_control_t *qc, struct io_uring_cqe *cqe) noexcept {
  // user_data lleva el índice de la op dentro del segmento de la queue: un
  // entero, no un puntero, así que no hay cast de entero a puntero.
  const auto _index_k = static_cast<std::size_t>(cqe->user_data);
  qc->ring_.cqe_seen(cqe);
  if (_index_k >= qc->ops_segment_size_)
    return;
  auto *_op = &qc->ops_segment_[_index_k];
  const long _res_k = cqe->res;
  // En accept, el resultado del kernel es el descriptor de la conexión
  // aceptada: es lo que el handler necesita para registrarlo.
  const int _fd_k =
      (_op->kind_ == op_kind_t::accept_ && _res_k >= 0) ? static_cast<int>(_res_k) : -1;
  const int _ec_k = (_res_k < 0) ? static_cast<int>(-_res_k) : 0;
  const auto _bytes_k = (_res_k >= 0) ? static_cast<std::size_t>(_res_k) : 0;
  finish_op(qc, _op, _ec_k, _bytes_k, _fd_k);
}

inline void reactor_t::execute_io_uring_op(detail::queue_control_t *qc, detail::op_t *op) noexcept {
  // Una operacion por direccion y socket, igual que en el backend de epoll:
  // el contrato es el mismo en los dos.
  detail::socket_slot_t *_slot = nullptr;
  if (op->socket_id_ != 0 && op->socket_id_ <= qc->num_sockets_) {
    _slot = &qc->sockets_[op->socket_id_ - 1];
    const bool _read_k = is_read_op(op->kind_);
    if (_read_k || is_write_op(op->kind_)) {
      std::atomic<detail::op_t *> *_pending_k =
          _read_k ? &_slot->pending_read_ : &_slot->pending_write_;
      if (_pending_k->load(std::memory_order_acquire) != nullptr) {
        finish_op(qc, op, EALREADY, 0, -1);
        return;
      }
      _pending_k->store(op, std::memory_order_release);
    }
  }

  const auto _index_k = static_cast<std::uint64_t>(op - qc->ops_segment_);
  auto *_sqe = qc->ring_.acquire_sqe();
  if (_sqe == nullptr) {
    // Sin SQE no se puede publicar la operación: vuelve a la cola y su
    // reserva de direccion se suelta.
    if (_slot != nullptr) {
      if (_slot->pending_read_.load(std::memory_order_acquire) == op)
        _slot->pending_read_.store(nullptr, std::memory_order_release);
      if (_slot->pending_write_.load(std::memory_order_acquire) == op)
        _slot->pending_write_.store(nullptr, std::memory_order_release);
    }
    qc->op_queue_.push(op);
    return;
  }
  switch (op->kind_) {
  case op_kind_t::recv_:
    io_uring_prep_recv(_sqe, op->fd_, op->buffer_, op->len_, 0);
    break;
  case op_kind_t::send_:
    io_uring_prep_send(_sqe, op->fd_, op->buffer_, op->len_, 0);
    break;
  case op_kind_t::accept_:
    op->peer_len_ = sizeof(sockaddr_storage);
    io_uring_prep_accept(
        _sqe, op->fd_, reinterpret_cast<sockaddr *>(&op->peer_), &op->peer_len_, 0);
    break;
  case op_kind_t::sendto_:
    io_uring_prep_sendto(_sqe,
                         op->fd_,
                         op->buffer_,
                         op->len_,
                         0,
                         reinterpret_cast<const sockaddr *>(&op->peer_),
                         op->peer_len_);
    break;
  case op_kind_t::recvfrom_: {
    op->iov_.iov_base = op->buffer_;
    op->iov_.iov_len = op->len_;
    op->message_.msg_name = &op->peer_;
    op->message_.msg_namelen = sizeof(op->peer_);
    op->message_.msg_iov = &op->iov_;
    op->message_.msg_iovlen = 1;
    io_uring_prep_recvmsg(_sqe, op->fd_, &op->message_, 0);
    break;
  }
  case op_kind_t::connect_:
    io_uring_prep_connect(
        _sqe, op->fd_, reinterpret_cast<const sockaddr *>(&op->peer_), op->peer_len_);
    break;
  default:
    qc->op_queue_.push(op);
    return;
  }
  _sqe->user_data = _index_k;
  (void) qc->ring_.submit(1);
}

// --- backend epoll ---

// Registra la operación pendiente del socket y actualiza el interest. La
// dirección solo admite una operación a la vez: es lo que permite que el
// interest se registre por descriptor.
inline void reactor_t::arm_epoll_op(detail::queue_control_t *qc, detail::op_t *op) noexcept {
  detail::socket_slot_t *_slot = nullptr;
  if (op->socket_id_ == 0 || op->socket_id_ > qc->num_sockets_) {
    finish_op(qc, op, EINVAL, 0, -1);
    return;
  }
  _slot = &qc->sockets_[op->socket_id_ - 1];

  const bool _read_k = is_read_op(op->kind_);
  if (!_read_k && !is_write_op(op->kind_)) {
    finish_op(qc, op, EINVAL, 0, -1);
    return;
  }
  std::atomic<detail::op_t *> *_pending_k =
      _read_k ? &_slot->pending_read_ : &_slot->pending_write_;
  if (_pending_k->load(std::memory_order_acquire) != nullptr) {
    // Ya hay una operacion en esa direccion: el socket no puede con dos.
    finish_op(qc, op, EALREADY, 0, -1);
    return;
  }
  if (op->kind_ == op_kind_t::connect_) {
    // El connect se emite aqui: el kernel responde EINPROGRESS y la
    // conexion se sabe buena cuando el descriptor quede escribible.
    if (::connect(op->fd_, reinterpret_cast<const sockaddr *>(&op->peer_), op->peer_len_) == 0) {
      // Conexion inmediata (loopback): no hace falta esperar.
      finish_op(qc, op, 0, 0, -1);
      return;
    }
    if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
      const int _err_k = errno;
      finish_op(qc, op, _err_k, 0, -1);
      return;
    }
  }

  _pending_k->store(op, std::memory_order_release);
  update_interest(qc, _slot);
}

// Ejecuta la operación de E/S que epoll reportó como lista.
inline void reactor_t::run_epoll_op(detail::queue_control_t *qc, detail::op_t *op) noexcept {
  switch (op->kind_) {
  case op_kind_t::recv_: {
    const auto _n_k = ::recv(op->fd_, op->buffer_, op->len_, MSG_DONTWAIT);
    if (_n_k < 0) {
      const int _err_k = errno;
      if (_err_k == EAGAIN || _err_k == EWOULDBLOCK || _err_k == EINTR)
        return; // sigue pendiente
      finish_op(qc, op, _err_k, 0, -1);
      return;
    }
    finish_op(qc, op, 0, static_cast<std::size_t>(_n_k), -1);
    return;
  }
  case op_kind_t::send_: {
    const auto _n_k = ::send(op->fd_, op->buffer_, op->len_, MSG_DONTWAIT);
    if (_n_k < 0) {
      const int _err_k = errno;
      if (_err_k == EAGAIN || _err_k == EWOULDBLOCK || _err_k == EINTR)
        return;
      finish_op(qc, op, _err_k, 0, -1);
      return;
    }
    finish_op(qc, op, 0, static_cast<std::size_t>(_n_k), -1);
    return;
  }
  case op_kind_t::accept_: {
    const int _fd_k = ::accept4(op->fd_, nullptr, nullptr, SOCK_NONBLOCK);
    if (_fd_k < 0) {
      const int _err_k = errno;
      if (_err_k == EAGAIN || _err_k == EWOULDBLOCK || _err_k == EINTR)
        return;
      finish_op(qc, op, _err_k, 0, -1);
      return;
    }
    finish_op(qc, op, 0, 0, _fd_k);
    return;
  }
  case op_kind_t::sendto_: {
    const auto _n_k = ::sendto(op->fd_,
                               op->buffer_,
                               op->len_,
                               MSG_DONTWAIT,
                               reinterpret_cast<const sockaddr *>(&op->peer_),
                               op->peer_len_);
    if (_n_k < 0) {
      const int _err_k = errno;
      if (_err_k == EAGAIN || _err_k == EWOULDBLOCK || _err_k == EINTR)
        return;
      finish_op(qc, op, _err_k, 0, -1);
      return;
    }
    finish_op(qc, op, 0, static_cast<std::size_t>(_n_k), -1);
    return;
  }
  case op_kind_t::recvfrom_: {
    socklen_t _peer_len_k = sizeof(op->peer_);
    const auto _n_k = ::recvfrom(op->fd_,
                                 op->buffer_,
                                 op->len_,
                                 MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr *>(&op->peer_),
                                 &_peer_len_k);
    if (_n_k < 0) {
      const int _err_k = errno;
      if (_err_k == EAGAIN || _err_k == EWOULDBLOCK || _err_k == EINTR)
        return;
      finish_op(qc, op, _err_k, 0, -1);
      return;
    }
    op->message_.msg_namelen = _peer_len_k;
    finish_op(qc, op, 0, static_cast<std::size_t>(_n_k), -1);
    return;
  }
  case op_kind_t::connect_: {
    // El socket ya esta en modo no bloqueante: connect devuelve EINPROGRESS
    // y la conexion se completa cuando el descriptor quede escribible.
    int _err_k = 0;
    socklen_t _err_len_k = sizeof(_err_k);
    if (::getsockopt(op->fd_, SOL_SOCKET, SO_ERROR, &_err_k, &_err_len_k) < 0)
      _err_k = errno;
    finish_op(qc, op, _err_k, 0, -1);
    return;
  }
  default:
    finish_op(qc, op, EINVAL, 0, -1);
    return;
  }
}

inline void reactor_t::handle_epoll_event(detail::queue_control_t *qc,
                                          const struct epoll_event &event) noexcept {
  const std::uint64_t _token_k = event.data.u64;
  if (_token_k == 0 || _token_k > qc->num_sockets_)
    return;
  auto *_slot = &qc->sockets_[_token_k - 1];
  detail::op_t *_op = nullptr;
  const std::uint32_t _events_k = event.events;
  if ((_events_k & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0 &&
      _slot->pending_read_.load(std::memory_order_acquire) != nullptr)
    _op = _slot->pending_read_.load(std::memory_order_acquire);
  else if ((_events_k & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0 &&
           _slot->pending_write_.load(std::memory_order_acquire) != nullptr)
    _op = _slot->pending_write_.load(std::memory_order_acquire);
  if (_op == nullptr)
    return;
  reactor_t::run_epoll_op(qc, _op);
}

// --- despacho por backend ---

inline void reactor_t::execute_op(detail::queue_control_t *qc, detail::op_t *op) noexcept {
  if (op->kind_ == op_kind_t::timer_) {
    // El timer entra en el heap de la queue, que solo manipula su hilo.
    if (!qc->timers_.insert(op)) {
      op->result_.ec_ = ENOSPC;
      op->result_.bytes_ = 0;
      if (op->fn_ != nullptr)
        op->fn_(op->ctx_, op->result_);
      op->done_.store(true, std::memory_order_release);
      qc->timers_live_.fetch_sub(1, std::memory_order_acq_rel);
    }
    return;
  }
  if (op->kind_ == op_kind_t::close_) {
    if (op->socket_id_ != 0 && op->socket_id_ <= qc->num_sockets_) {
      detail::socket_slot_t *_slot = &qc->sockets_[op->socket_id_ - 1];
      if (_slot->pending_read_.load(std::memory_order_acquire) == nullptr &&
          _slot->pending_write_.load(std::memory_order_acquire) == nullptr) {
        // No queda nada en vuelo: el descriptor se puede cerrar ya.
        if (_slot->fd_ >= 0)
          ::close(_slot->fd_);
        _slot->fd_ = -1;
        _slot->close_pending_.store(false, std::memory_order_release);
      }
    }
    op->done_.store(true, std::memory_order_release);
    return;
  }
  if (op->kind_ == op_kind_t::cancel_) {
    for (std::size_t _index = 0; _index < qc->timers_segment_size_; ++_index) {
      detail::op_t *_timer = &qc->timers_segment_[_index];
      if (_timer->done_.load(std::memory_order_acquire) || _timer->timer_id_ != op->cancel_target_)
        continue;
      if (qc->timers_.remove(_timer)) {
        _timer->timer_id_ = 0;
        _timer->done_.store(true, std::memory_order_release);
        qc->timers_live_.fetch_sub(1, std::memory_order_acq_rel);
      }
      break;
    }
    op->done_.store(true, std::memory_order_release);
    return;
  }
  if (op->kind_ == op_kind_t::custom_) {
    op->result_.ec_ = 0;
    op->result_.bytes_ = 0;
    if (op->fn_ != nullptr)
      op->fn_(op->ctx_, op->result_);
    if (op->strand_id_ != 0) {
      const auto _strand_index_k = static_cast<std::size_t>(op->strand_id_ % qc->num_strands_);
      qc->strands_[_strand_index_k].release();
    }
    release_socket(qc, op->socket_id_);
    op->done_.store(true, std::memory_order_release);
    return;
  }

  if (qc->use_epoll_) {
    reactor_t::arm_epoll_op(qc, op);
    return;
  }

  if (!qc->ring_.is_ready()) {
    // Sin io_uring no hay E/S posible. La operacion se completa con ENOSYS
    // para que el handler se entere, en vez de colgarse.
    finish_op(qc, op, ENOSYS, 0, -1);
    return;
  }
  reactor_t::execute_io_uring_op(qc, op);
}

// Espera hasta el próximo deadline, con un tope para notar el flag de stop.
inline int wait_timeout_ms(detail::queue_control_t *qc) noexcept {
  const std::uint64_t _next_k = qc->timers_.next_deadline();
  if (_next_k == 0)
    return 1;
  const std::uint64_t _now_k = monotonic_now_ns();
  if (_next_k <= _now_k)
    return 0;
  const std::uint64_t _delta_k = _next_k - _now_k;
  // epoll espera en milisegundos: se redondea hacia arriba.
  const std::uint64_t _ms_k = (_delta_k + 999999ULL) / 1000000ULL;
  return (_ms_k > 1000ULL) ? 1000 : static_cast<int>(_ms_k);
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

    // 4. Esperar eventos del backend activo.
    if (_qc->use_epoll_) {
      const int _count_k = _qc->epoll_.wait(wait_timeout_ms(_qc));
      for (int _index_k = 0; _index_k < _count_k; ++_index_k)
        reactor_t::handle_epoll_event(_qc,
                                      _qc->epoll_.event_at(static_cast<std::size_t>(_index_k)));
      continue;
    }

    if (!_qc->ring_.is_ready()) {
      // Sin io_uring no hay completaciones que esperar: la queue solo procesa
      // timers y operaciones locales. Duerme hasta el proximo deadline.
      const std::uint64_t _next_k = _qc->timers_.next_deadline();
      if (_next_k == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        const std::uint64_t _now_k = monotonic_now_ns();
        const auto _wait_ns_k = (_next_k > _now_k) ? (_next_k - _now_k) : 0;
        std::this_thread::sleep_for(std::chrono::nanoseconds(_wait_ns_k));
      }
      continue;
    }

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
  auto *_op = acquire_op(ops_segment_, ops_segment_size_);
  if (_op != nullptr) {
    {
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
      _op->next_ = nullptr;
      if (!control_->op_queue_.push(_op)) {
        _op->done_.store(true, std::memory_order_release);
        return false;
      }
      return true;
    }
  }
  return false; // pool de ops de esta queue agotado
}

inline void queue_t::run() noexcept {
  if (owner_ == nullptr || control_ == nullptr)
    return;
  control_->running_ = true;
  owner_->run_queue(index_);
}

inline void queue_t::stop() noexcept {
  if (control_ != nullptr)
    control_->stop_.store(true, std::memory_order_release);
}

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

inline bool socket_t::close() noexcept {
  if (fd_ < 0)
    return true;
  detail::socket_slot_t *_slot = slot();
  if (_slot == nullptr) {
    ::close(fd_);
    fd_ = -1;
    return true;
  }
  // Cerrar dos veces la misma socket no debe cerrar un descriptor ajeno.
  if (_slot->closed_.exchange(true, std::memory_order_acq_rel)) {
    fd_ = -1;
    return true;
  }
  fd_ = -1;

  // El descriptor lo cierra el hilo de la queue: cerrarlo desde aqui
  // competiria con la queue que lo esta usando, y el slot se modificaria
  // desde dos hilos a la vez.
  detail::queue_control_t *_qc = queue_->control_;
  detail::op_t *_op = acquire_op(_qc->ops_segment_, _qc->ops_segment_size_);
  if (_op == nullptr)
    return false;
  _op->kind_ = op_kind_t::close_;
  _op->socket_id_ = id_;
  _op->fn_ = nullptr;
  _op->ctx_ = nullptr;
  _op->strand_id_ = 0;
  _op->next_ = nullptr;
  if (!_qc->op_queue_.push(_op)) {
    _op->done_.store(true, std::memory_order_release);
    return false;
  }
  _slot->close_pending_.store(true, std::memory_order_release);
  return true;
}

} // namespace frameworkcpp::reactor
