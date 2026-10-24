// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

#include "frameworkcpp/concurrency.hpp"
#include "frameworkcpp/reactor/detail/op.hpp"

namespace frameworkcpp::reactor::detail {

static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "the operation ring needs lock-free size_t atomics");

// Cola de operaciones pre-reservadas en arena. El modelo de concurrencia se
// elige al inicializarla, igual que un bucket de arena:
//
//   single_thread_ -> un productor y un consumidor, sin sincronizacion.
//   lock_free_     -> varios productores y varios consumidores, sin locks.
//   thread_safe_   -> varios productores y consumidores, serializados por mutex.
//
// La capacidad debe ser potencia de dos. Para lock_free_ cada slot necesita
// una secuencia propia (Vyukov) que se reserva junto a la cola.
class op_queue_t {
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  std::size_t capacity_{0};
  std::size_t mask_{0};
  op_t **slots_ = nullptr;
  std::atomic<std::size_t> *sequence_ = nullptr;
  concurrency_model_t model_ = concurrency_model_t::single_thread_;
  mutable std::mutex mutex_;

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
            concurrency_model_t model) noexcept;

  // Push un op. Devuelve false si la cola está llena (backpressure).
  bool push(op_t *op) noexcept;

  // Pop un op. Devuelve nullptr si está vacía.
  op_t *pop() noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] concurrency_model_t get_concurrency_model() const noexcept { return model_; }

  private:
  bool push_serial(op_t *op) noexcept;
  op_t *pop_serial() noexcept;
  bool push_lock_free(op_t *op) noexcept;
  op_t *pop_lock_free() noexcept;
}; // class op_queue_t

inline bool op_queue_t::init(op_t **slots,
                             std::atomic<std::size_t> *sequence,
                             std::size_t capacity,
                             concurrency_model_t model) noexcept {
  if (slots == nullptr || capacity == 0 || (capacity & (capacity - 1)) != 0)
    return false;
  if (model == concurrency_model_t::lock_free_ && sequence == nullptr)
    return false;
  slots_ = slots;
  sequence_ = sequence;
  capacity_ = capacity;
  mask_ = capacity - 1;
  model_ = model;
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
  switch (model_) {
  case concurrency_model_t::single_thread_:
    return push_serial(op);
  case concurrency_model_t::lock_free_:
    return push_lock_free(op);
  case concurrency_model_t::thread_safe_: {
    const std::lock_guard _lock_k{mutex_};
    return push_serial(op);
  }
  }
  return false;
}

inline op_t *op_queue_t::pop() noexcept {
  switch (model_) {
  case concurrency_model_t::single_thread_:
    return pop_serial();
  case concurrency_model_t::lock_free_:
    return pop_lock_free();
  case concurrency_model_t::thread_safe_: {
    const std::lock_guard _lock_k{mutex_};
    return pop_serial();
  }
  }
  return nullptr;
}

inline std::size_t op_queue_t::size() const noexcept {
  if (model_ == concurrency_model_t::thread_safe_) {
    const std::lock_guard _lock_k{mutex_};
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }
  return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
}

} // namespace frameworkcpp::reactor::detail
