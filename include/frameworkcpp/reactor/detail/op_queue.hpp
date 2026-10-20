// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "frameworkcpp/reactor/detail/op.hpp"

namespace frameworkcpp::reactor::detail {

// Cola lock-free MPMC para ops. Los slots son punteros a op_t pre-reservados
// en arena. Un productor (cualquier hilo) push; un consumidor (el hilo de la
// queue) pop. Wait-free para un solo consumidor, lock-free para muchos
// productores.
class op_queue_t {
  std::atomic<std::size_t> head_{0}; // consumer index
  std::atomic<std::size_t> tail_{0}; // producer index
  std::size_t capacity_{0};
  op_t **slots_ = nullptr;

  public:
  op_queue_t() = default;
  op_queue_t(const op_queue_t &) = delete;
  op_queue_t &operator=(const op_queue_t &) = delete;
  op_queue_t(op_queue_t &&) = delete;
  op_queue_t &operator=(op_queue_t &&) = delete;
  ~op_queue_t() = default;

  // Pre-reserva slots en arena. capacity debe ser potencia de dos.
  bool init(op_t **slots, std::size_t capacity) noexcept;

  // Push un op. Devuelve false si la cola está llena (backpressure).
  bool push(op_t *op) noexcept;

  // Pop un op. Devuelve nullptr si está vacía.
  op_t *pop() noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
}; // class op_queue_t

inline bool op_queue_t::init(op_t **slots, std::size_t capacity) noexcept {
  slots_ = slots;
  capacity_ = capacity;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  return true;
}

inline bool op_queue_t::push(op_t *op) noexcept {
  const std::size_t _tail_k = tail_.load(std::memory_order_relaxed);
  const std::size_t _head_k = head_.load(std::memory_order_acquire);
  if (_tail_k - _head_k >= capacity_)
    return false;
  slots_[_tail_k & (capacity_ - 1)] = op;
  tail_.store(_tail_k + 1, std::memory_order_release);
  return true;
}

inline op_t *op_queue_t::pop() noexcept {
  const std::size_t _head_k = head_.load(std::memory_order_relaxed);
  const std::size_t _tail_k = tail_.load(std::memory_order_acquire);
  if (_head_k == _tail_k)
    return nullptr;
  auto *_op = slots_[_head_k & (capacity_ - 1)];
  head_.store(_head_k + 1, std::memory_order_release);
  return _op;
}

inline std::size_t op_queue_t::size() const noexcept {
  return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
}

} // namespace frameworkcpp::reactor::detail
