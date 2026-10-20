// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "frameworkcpp/reactor/detail/op.hpp"

namespace frameworkcpp::reactor::detail {

// Heap binario de deadlines para una queue.
//
// Los timers se ordenan por deadline en un heap sobre punteros a op_t. Insertar
// y expirar son O(log n), sin asignar nada: el array del heap y los ops se
// reservan en el arena.
//
// El op guarda su posición en el heap, así que cancelar un timer por id es
// también O(log n) en vez de un barrido.
class timer_wheel_t {
  op_t **heap_ = nullptr; // [capacity]
  std::size_t capacity_ = 0;
  std::size_t count_ = 0;

  public:
  timer_wheel_t() = default;

  // Pre-reserva el array del heap en arena. `capacity` debe ser potencia de dos.
  bool init(op_t **heap, std::size_t capacity) noexcept;

  // Inserta un timer con deadline absoluto (nanosegundos monotónicos).
  // Devuelve false si el heap está lleno.
  bool insert(op_t *op) noexcept;

  // Extrae el timer con el deadline más cercano. Devuelve nullptr si está vacío.
  op_t *pop() noexcept;

  // Quita un timer del heap. Devuelve true si estaba dentro.
  bool remove(op_t *op) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // Deadline del timer más cercano, o 0 si no hay timers.
  [[nodiscard]] std::uint64_t next_deadline() const noexcept;

  private:
  void sift_up(std::size_t index_k) noexcept;
  void sift_down(std::size_t index_k) noexcept;
  void swap_entries(std::size_t left_k, std::size_t right_k) noexcept;
  [[nodiscard]] static bool less(const op_t *left, const op_t *right) noexcept;
}; // class timer_wheel_t

inline bool timer_wheel_t::init(op_t **heap, std::size_t capacity) noexcept {
  heap_ = heap;
  capacity_ = capacity;
  count_ = 0;
  return true;
}

inline bool timer_wheel_t::less(const op_t *left, const op_t *right) noexcept {
  if (left->deadline_ns_ != right->deadline_ns_)
    return left->deadline_ns_ < right->deadline_ns_;
  // Empate por deadline: el id mantiene un orden estable y determinista.
  return left->timer_id_ < right->timer_id_;
}

inline void timer_wheel_t::swap_entries(std::size_t left_k, std::size_t right_k) noexcept {
  op_t *_temp = heap_[left_k];
  heap_[left_k] = heap_[right_k];
  heap_[right_k] = _temp;
  heap_[left_k]->heap_index_ = left_k;
  heap_[right_k]->heap_index_ = right_k;
}

inline void timer_wheel_t::sift_up(std::size_t index_k) noexcept {
  while (index_k > 0) {
    const std::size_t _parent_k = (index_k - 1) / 2;
    if (!less(heap_[index_k], heap_[_parent_k]))
      break;
    swap_entries(index_k, _parent_k);
    index_k = _parent_k;
  }
}

inline void timer_wheel_t::sift_down(std::size_t index_k) noexcept {
  for (;;) {
    const std::size_t _left_k = (2 * index_k) + 1;
    const std::size_t _right_k = _left_k + 1;
    std::size_t _smallest_k = index_k;
    if (_left_k < count_ && less(heap_[_left_k], heap_[_smallest_k]))
      _smallest_k = _left_k;
    if (_right_k < count_ && less(heap_[_right_k], heap_[_smallest_k]))
      _smallest_k = _right_k;
    if (_smallest_k == index_k)
      break;
    swap_entries(index_k, _smallest_k);
    index_k = _smallest_k;
  }
}

inline bool timer_wheel_t::insert(op_t *op) noexcept {
  if (heap_ == nullptr || count_ >= capacity_)
    return false;
  heap_[count_] = op;
  op->heap_index_ = count_;
  ++count_;
  sift_up(count_ - 1);
  return true;
}

inline op_t *timer_wheel_t::pop() noexcept {
  if (count_ == 0)
    return nullptr;
  op_t *_top = heap_[0];
  --count_;
  if (count_ > 0) {
    heap_[0] = heap_[count_];
    heap_[0]->heap_index_ = 0;
    sift_down(0);
  }
  _top->heap_index_ = 0;
  return _top;
}

inline bool timer_wheel_t::remove(op_t *op) noexcept {
  if (heap_ == nullptr || op == nullptr)
    return false;
  const std::size_t _index_k = op->heap_index_;
  if (_index_k >= count_ || heap_[_index_k] != op)
    return false;
  --count_;
  if (_index_k != count_) {
    heap_[_index_k] = heap_[count_];
    heap_[_index_k]->heap_index_ = _index_k;
    sift_up(_index_k);
    sift_down(_index_k);
  }
  op->heap_index_ = 0;
  return true;
}

inline std::uint64_t timer_wheel_t::next_deadline() const noexcept {
  if (count_ == 0)
    return 0;
  return heap_[0]->deadline_ns_;
}

} // namespace frameworkcpp::reactor::detail
