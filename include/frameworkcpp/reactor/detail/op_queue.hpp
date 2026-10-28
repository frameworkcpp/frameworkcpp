// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

#include "frameworkcpp/concurrency.hpp"
#include "frameworkcpp/reactor/detail/op.hpp"
#include "frameworkcpp/reactor/reactor_config.hpp"

namespace frameworkcpp::reactor::detail {

static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "the operation ring needs lock-free size_t atomics");

// Cola de operaciones pre-reservadas en arena. El modelo se elige al
// inicializarla y tiene dos ejes independientes:
//
//   producers_: cuantos hilos empujan. single_thread_ usa el camino serial;
//   lock_free_ usa CAS; thread_safe_ serializa solo a los productores con un
//   mutex, sin tocar el consumo.
//
//   consumers_: cuantos hilos consumen. Hoy el reactor solo soporta
//   single_thread_ porque cada queue tiene un event loop; el campo se valida
//   para que el contrato no mienta.
//
// La capacidad debe ser potencia de dos. Para producers_ == lock_free_ cada
// slot necesita una secuencia propia (Vyukov) que se reserva junto a la cola.
class op_queue_t {
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  std::size_t capacity_{0};
  std::size_t mask_{0};
  op_t **slots_ = nullptr;
  std::atomic<std::size_t> *sequence_ = nullptr;
  queue_concurrency_t concurrency_ = {};
  mutable std::mutex producer_mutex_;
  mutable std::mutex consumer_mutex_;

  public:
  op_queue_t() = default;
  op_queue_t(const op_queue_t &) = delete;
  op_queue_t &operator=(const op_queue_t &) = delete;
  op_queue_t(op_queue_t &&) = delete;
  op_queue_t &operator=(op_queue_t &&) = delete;
  ~op_queue_t() = default;

  // Pre-reserva slots y secuencias en arena. `capacity` debe ser potencia de
  // dos. `sequence` puede ser nullptr para los modelos que no lo usan.
  bool init(op_t **slots,
            std::atomic<std::size_t> *sequence,
            std::size_t capacity,
            queue_concurrency_t concurrency) noexcept;

  // Cambia el contrato de una cola vacia. La usa get_or_create cuando el
  // usuario pide un modelo por queue distinto del default del reactor.
  bool set_concurrency(queue_concurrency_t concurrency) noexcept;

  // Push un op. Devuelve false si la cola está llena (backpressure).
  bool push(op_t *op) noexcept;

  // Pop un op. Devuelve nullptr si está vacía.
  op_t *pop() noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] queue_concurrency_t get_concurrency() const noexcept { return concurrency_; }

  private:
  bool push_serial(op_t *op) noexcept;
  op_t *pop_serial() noexcept;
  bool push_lock_free(op_t *op) noexcept;
  op_t *pop_lock_free() noexcept;
  [[nodiscard]] bool uses_lock_free_ring() const noexcept {
    return concurrency_.producers_ == concurrency_model_t::lock_free_ ||
           concurrency_.consumers_ == concurrency_model_t::lock_free_;
  }
}; // class op_queue_t

inline bool op_queue_t::init(op_t **slots,
                             std::atomic<std::size_t> *sequence,
                             std::size_t capacity,
                             queue_concurrency_t concurrency) noexcept {
  if (slots == nullptr || capacity == 0 || (capacity & (capacity - 1)) != 0)
    return false;
  switch (concurrency.producers_) {
  case concurrency_model_t::single_thread_:
  case concurrency_model_t::lock_free_:
  case concurrency_model_t::thread_safe_:
    break;
  default:
    return false;
  }
  switch (concurrency.consumers_) {
  case concurrency_model_t::single_thread_:
  case concurrency_model_t::lock_free_:
  case concurrency_model_t::thread_safe_:
    break;
  default:
    return false;
  }
  if ((concurrency.producers_ == concurrency_model_t::lock_free_ ||
       concurrency.consumers_ == concurrency_model_t::lock_free_) &&
      sequence == nullptr)
    return false;
  slots_ = slots;
  sequence_ = sequence;
  capacity_ = capacity;
  mask_ = capacity - 1;
  concurrency_ = concurrency;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  if (sequence_ != nullptr) {
    for (std::size_t _index = 0; _index < capacity_; ++_index)
      ::new (static_cast<void *>(sequence_ + _index)) std::atomic<std::size_t>(_index);
  }
  return true;
}

inline bool op_queue_t::set_concurrency(queue_concurrency_t concurrency) noexcept {
  if (slots_ == nullptr || capacity_ == 0)
    return false;
  switch (concurrency.producers_) {
  case concurrency_model_t::single_thread_:
  case concurrency_model_t::lock_free_:
  case concurrency_model_t::thread_safe_:
    break;
  default:
    return false;
  }
  switch (concurrency.consumers_) {
  case concurrency_model_t::single_thread_:
  case concurrency_model_t::lock_free_:
  case concurrency_model_t::thread_safe_:
    break;
  default:
    return false;
  }
  if ((concurrency.producers_ == concurrency_model_t::lock_free_ ||
       concurrency.consumers_ == concurrency_model_t::lock_free_) &&
      sequence_ == nullptr)
    return false;
  concurrency_ = concurrency;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  if (sequence_ != nullptr) {
    for (std::size_t _index = 0; _index < capacity_; ++_index)
      ::new (static_cast<void *>(sequence_ + _index)) std::atomic<std::size_t>(_index);
  }
  return true;
}

// Single producer / single consumer path. Safe only under single_thread_.
inline bool op_queue_t::push_serial(op_t *op) noexcept {
  const std::size_t _tail_k = tail_.load(std::memory_order_relaxed);
  const std::size_t _head_k = head_.load(std::memory_order_acquire);
  if (_tail_k - _head_k >= capacity_)
    return false;
  slots_[_tail_k & mask_] = op;
  tail_.store(_tail_k + 1, std::memory_order_release);
  return true;
}

inline op_t *op_queue_t::pop_serial() noexcept {
  const std::size_t _head_k = head_.load(std::memory_order_relaxed);
  const std::size_t _tail_k = tail_.load(std::memory_order_acquire);
  if (_head_k == _tail_k)
    return nullptr;
  auto *_op = slots_[_head_k & mask_];
  head_.store(_head_k + 1, std::memory_order_release);
  return _op;
}

// Bounded MPMC queue (Vyukov). Each slot publishes its ticket in `sequence_`:
// producers wait for `ticket == position`, consumers for `ticket == position
// + 1`, and a pop republishes the slot for the next lap.
inline bool op_queue_t::push_lock_free(op_t *op) noexcept {
  std::size_t _position = tail_.load(std::memory_order_relaxed);
  for (;;) {
    const std::size_t _ticket_k = sequence_[_position & mask_].load(std::memory_order_acquire);
    const auto _distance_k =
        static_cast<std::intptr_t>(_ticket_k) - static_cast<std::intptr_t>(_position);
    if (_distance_k == 0) {
      if (tail_.compare_exchange_weak(
              _position, _position + 1, std::memory_order_relaxed, std::memory_order_relaxed))
        break;
    } else if (_distance_k < 0) {
      return false;
    } else {
      _position = tail_.load(std::memory_order_relaxed);
    }
  }
  slots_[_position & mask_] = op;
  sequence_[_position & mask_].store(_position + 1, std::memory_order_release);
  return true;
}

inline op_t *op_queue_t::pop_lock_free() noexcept {
  std::size_t _position = head_.load(std::memory_order_relaxed);
  for (;;) {
    const std::size_t _ticket_k = sequence_[_position & mask_].load(std::memory_order_acquire);
    const auto _distance_k =
        static_cast<std::intptr_t>(_ticket_k) - static_cast<std::intptr_t>(_position + 1);
    if (_distance_k == 0) {
      if (head_.compare_exchange_weak(
              _position, _position + 1, std::memory_order_relaxed, std::memory_order_relaxed))
        break;
    } else if (_distance_k < 0) {
      return nullptr;
    } else {
      _position = head_.load(std::memory_order_relaxed);
    }
  }
  auto *_op = slots_[_position & mask_];
  sequence_[_position & mask_].store(_position + capacity_, std::memory_order_release);
  return _op;
}

inline bool op_queue_t::push(op_t *op) noexcept {
  std::unique_lock<std::mutex> _lock_k{producer_mutex_, std::defer_lock};
  if (concurrency_.producers_ == concurrency_model_t::thread_safe_)
    _lock_k.lock();
  return uses_lock_free_ring() ? push_lock_free(op) : push_serial(op);
}

inline op_t *op_queue_t::pop() noexcept {
  std::unique_lock<std::mutex> _lock_k{consumer_mutex_, std::defer_lock};
  if (concurrency_.consumers_ == concurrency_model_t::thread_safe_)
    _lock_k.lock();
  return uses_lock_free_ring() ? pop_lock_free() : pop_serial();
}

inline std::size_t op_queue_t::size() const noexcept {
  std::unique_lock<std::mutex> _producer_lock_k{producer_mutex_, std::defer_lock};
  std::unique_lock<std::mutex> _consumer_lock_k{consumer_mutex_, std::defer_lock};
  if (concurrency_.producers_ == concurrency_model_t::thread_safe_)
    _producer_lock_k.lock();
  if (concurrency_.consumers_ == concurrency_model_t::thread_safe_)
    _consumer_lock_k.lock();
  return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
}

} // namespace frameworkcpp::reactor::detail
