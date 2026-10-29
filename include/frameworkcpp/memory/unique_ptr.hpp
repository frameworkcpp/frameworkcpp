// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "frameworkcpp/memory/shard.hpp"

namespace frameworkcpp::memory {
template <typename T> class memory_unique_ptr_t {
  T *p_ = nullptr;
  memory_shard_t shard_;

  public:
  memory_unique_ptr_t() noexcept = default;

  memory_unique_ptr_t(T *pointer, memory_shard_t shard) noexcept : p_(pointer), shard_(shard) {}

  memory_unique_ptr_t(memory_unique_ptr_t &&other) noexcept : p_(other.p_), shard_(other.shard_) {
    other.p_ = nullptr;
  }

  memory_unique_ptr_t &operator=(memory_unique_ptr_t &&other) noexcept {
    if (this != &other) {
      reset();
      p_ = other.p_;
      shard_ = other.shard_;
      other.p_ = nullptr;
    }
    return *this;
  }

  memory_unique_ptr_t(const memory_unique_ptr_t &) = delete;
  memory_unique_ptr_t &operator=(const memory_unique_ptr_t &) = delete;

  ~memory_unique_ptr_t() { reset(); }

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
}; // class memory_unique_ptr_t

template <typename T, typename... Args>
memory_unique_ptr_t<T> make_memory_unique_ptr(memory_shard_t &shard, Args &&...args) {
  auto *_raw = shard.allocate_bytes(sizeof(T), alignof(T));
  if (_raw == nullptr)
    return memory_unique_ptr_t<T>{};
  T *_pointer = ::new (static_cast<void *>(_raw)) T(std::forward<Args>(args)...);
  return memory_unique_ptr_t<T>(_pointer, shard);
}
} // namespace frameworkcpp::memory
