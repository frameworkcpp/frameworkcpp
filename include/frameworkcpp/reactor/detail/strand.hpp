// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "frameworkcpp/reactor/detail/op.hpp"

namespace frameworkcpp::reactor::detail {

// Serializa ops por strand_id. Cada strand tiene una lista intrusiva de ops
// pendientes y un flag `busy`: mientras una op del strand está en vuelo
// (esperando su CQE), ninguna otra del mismo strand puede arrancar.
//
// Solo lo usa el hilo dueño de la queue, así que la sincronización entre
// hilos es únicamente el flag atómico.
class strand_t {
  std::atomic<op_t *> head_{nullptr};
  std::atomic<op_t *> tail_{nullptr};
  std::atomic<bool> busy_{false};

  public:
  strand_t() = default;
  strand_t(const strand_t &) = delete;
  strand_t &operator=(const strand_t &) = delete;
  strand_t(strand_t &&) = delete;
  strand_t &operator=(strand_t &&) = delete;
  ~strand_t() = default;

  // Agrega un op al final de la lista del strand.
  void enqueue(op_t *op) noexcept {
    op->next_ = nullptr;
    op_t *_prev_k = tail_.exchange(op, std::memory_order_acq_rel);
    if (_prev_k == nullptr)
      head_.store(op, std::memory_order_release);
    else
      _prev_k->next_ = op;
  }

  // Extrae el op del frente solo si el strand está libre. Devuelve nullptr si
  // está ocupado o vacío.
  op_t *try_dispatch() noexcept {
    if (busy_.load(std::memory_order_acquire))
      return nullptr;
    op_t *_head_k = head_.load(std::memory_order_acquire);
    if (_head_k == nullptr)
      return nullptr;
    op_t *_next_k = _head_k->next_;
    if (!head_.compare_exchange_strong(
            _head_k, _next_k, std::memory_order_acq_rel, std::memory_order_acquire))
      return nullptr;
    if (_next_k == nullptr)
      tail_.store(nullptr, std::memory_order_release);
    _head_k->next_ = nullptr;
    busy_.store(true, std::memory_order_release);
    return _head_k;
  }

  // Libera el strand: la op en vuelo terminó y la siguiente puede arrancar.
  void release() noexcept { busy_.store(false, std::memory_order_release); }

  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == nullptr;
  }
}; // class strand_t

} // namespace frameworkcpp::reactor::detail
