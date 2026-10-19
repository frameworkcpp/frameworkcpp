// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "frameworkcpp/arena/shard.hpp"

namespace frameworkcpp::arena {
template <typename T> class arena_unique_ptr_t {
  T *p_ = nullptr;
  arena_shard_t shard_;

  public:
  arena_unique_ptr_t() noexcept = default;

  arena_unique_ptr_t(T *pointer, arena_shard_t shard) noexcept : p_(pointer), shard_(shard) {}

  arena_unique_ptr_t(arena_unique_ptr_t &&other) noexcept : p_(other.p_), shard_(other.shard_) {
    other.p_ = nullptr;
  }

  arena_unique_ptr_t &operator=(arena_unique_ptr_t &&other) noexcept {
    if (this != &other) {
      reset();
      p_ = other.p_;
      shard_ = other.shard_;
      other.p_ = nullptr;
    }
    return *this;
  }

  arena_unique_ptr_t(const arena_unique_ptr_t &) = delete;
  arena_unique_ptr_t &operator=(const arena_unique_ptr_t &) = delete;

  ~arena_unique_ptr_t() { reset(); }

  [[nodiscard]] T *get() const noexcept { return p_; }
  T *operator->() const noexcept { return p_; }
  T &operator*() const noexcept { return *p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }

  void reset() noexcept {
    if (p_ == nullptr)
      return;
    p_->~T();
    shard_.deallocate_bytes(p_);
    p_ = nullptr;
  }
}; // class arena_unique_ptr_t

template <typename T, typename... Args>
arena_unique_ptr_t<T> make_arena_unique_ptr(arena_shard_t &shard, Args &&...args) {
  auto *_raw = shard.allocate_bytes(sizeof(T), alignof(T));
  if (_raw == nullptr)
    return arena_unique_ptr_t<T>{};
  T *_pointer = ::new (static_cast<void *>(_raw)) T(std::forward<Args>(args)...);
  return arena_unique_ptr_t<T>(_pointer, shard);
}
} // namespace frameworkcpp::arena
