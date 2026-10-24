// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

#include "frameworkcpp/concurrency.hpp"
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
  concurrency_model_t model_ = concurrency_model_t::single_thread_;
#ifndef NDEBUG
  std::atomic<bool> producer_claimed_{false};
  std::atomic<std::uint64_t> producer_thread_{0};
#endif

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

  void set_concurrency_model(concurrency_model_t model) noexcept { model_ = model; }

  [[nodiscard]] concurrency_model_t get_concurrency_model() const noexcept { return model_; }

  // El modelo del strand es el contrato de submission: single_thread_ dice
  // que un solo hilo postea ops para esta clave. En debug se verifica.
  void assert_submission_thread() noexcept {
#ifndef NDEBUG
    if (model_ != concurrency_model_t::single_thread_)
      return;
    const auto _token_k =
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (!producer_claimed_.exchange(true, std::memory_order_acq_rel)) {
      producer_thread_.store(_token_k, std::memory_order_release);
      return;
    }
    assert(producer_thread_.load(std::memory_order_acquire) == _token_k &&
           "single_thread_ strand posted from more than one thread");
#endif
  }
}; // class strand_t

} // namespace frameworkcpp::reactor::detail
