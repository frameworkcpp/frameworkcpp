// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "frameworkcpp/memory/shard.hpp"

namespace frameworkcpp::memory::detail {
struct memory_control_base_t {
  std::atomic<std::int32_t> strong_{1};
  void *object_ = nullptr;
  memory_shard_t shard_;
  void (*destroy_)(memory_shard_t &, void *) = nullptr;
}; // struct memory_control_base_t
} // namespace frameworkcpp::memory::detail

namespace frameworkcpp::memory {
template <typename T> class memory_enable_shared_from_this_t;

template <typename T> class memory_shared_ptr_t {
  detail::memory_control_base_t *ctl_ = nullptr;
  T *obj_ = nullptr;

  public:
  memory_shared_ptr_t() noexcept = default;

  memory_shared_ptr_t(const memory_shared_ptr_t &other) noexcept
      : ctl_(other.ctl_), obj_(other.obj_) {
    if (ctl_ != nullptr)
      ctl_->strong_.fetch_add(1, std::memory_order_relaxed);
  }

  memory_shared_ptr_t &operator=(const memory_shared_ptr_t &other) noexcept {
    if (this != &other) {
      reset();
      ctl_ = other.ctl_;
      obj_ = other.obj_;
      if (ctl_ != nullptr)
        ctl_->strong_.fetch_add(1, std::memory_order_relaxed);
    }
    return *this;
  }

  memory_shared_ptr_t(memory_shared_ptr_t &&other) noexcept : ctl_(other.ctl_), obj_(other.obj_) {
    other.ctl_ = nullptr;
    other.obj_ = nullptr;
  }

  memory_shared_ptr_t &operator=(memory_shared_ptr_t &&other) noexcept {
    if (this != &other) {
      reset();
      ctl_ = other.ctl_;
      obj_ = other.obj_;
      other.ctl_ = nullptr;
      other.obj_ = nullptr;
    }
    return *this;
  }

  template <typename U>
  memory_shared_ptr_t(const memory_shared_ptr_t<U> &other) noexcept
      : ctl_(other.ctl_), obj_(other.obj_) {
    if (ctl_ != nullptr)
      ctl_->strong_.fetch_add(1, std::memory_order_relaxed);
  }

  template <typename U>
  memory_shared_ptr_t(memory_shared_ptr_t<U> &&other) noexcept
      : ctl_(other.ctl_), obj_(other.obj_) {
    other.ctl_ = nullptr;
    other.obj_ = nullptr;
  }

  ~memory_shared_ptr_t() { reset(); }

  [[nodiscard]] T *get() const noexcept { return obj_; }
  T *operator->() const noexcept { return obj_; }
  T &operator*() const noexcept { return *obj_; }
  explicit operator bool() const noexcept { return obj_ != nullptr; }

  void reset() noexcept {
    if (ctl_ == nullptr)
      return;
    if (ctl_->strong_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      ctl_->destroy_(ctl_->shard_, ctl_);
    ctl_ = nullptr;
    obj_ = nullptr;
  }

  private:
  template <typename U> friend class memory_shared_ptr_t;
  template <typename U, typename... Args>
  friend memory_shared_ptr_t<U> make_memory_shared_ptr(memory_shard_t &, Args &&...);
  friend class memory_enable_shared_from_this_t<T>;
}; // class memory_shared_ptr_t

template <typename T> class memory_enable_shared_from_this_t {
  detail::memory_control_base_t *owner_ = nullptr;

  protected:
  memory_enable_shared_from_this_t() = default;

  public:
  memory_shared_ptr_t<T> shared_from_this() {
    if (owner_ == nullptr)
      throw std::bad_weak_ptr();
    memory_shared_ptr_t<T> _result;
    _result.ctl_ = owner_;
    _result.obj_ = static_cast<T *>(owner_->object_);
    owner_->strong_.fetch_add(1, std::memory_order_relaxed);
    return _result;
  }

  private:
  template <typename U, typename... Args>
  friend memory_shared_ptr_t<U> make_memory_shared_ptr(memory_shard_t &, Args &&...);
}; // class memory_enable_shared_from_this_t

template <typename T, typename... Args>
memory_shared_ptr_t<T> make_memory_shared_ptr(memory_shard_t &shard, Args &&...args) {
  constexpr std::size_t _control_size_k = sizeof(detail::memory_control_base_t);
  constexpr std::size_t _control_align_k = alignof(detail::memory_control_base_t);
  constexpr std::size_t _align_k = alignof(T) > _control_align_k ? alignof(T) : _control_align_k;
  constexpr std::size_t _object_offset_k = (_control_size_k + alignof(T) - 1) & ~(alignof(T) - 1);
  auto *_raw = shard.allocate_bytes(_object_offset_k + sizeof(T), _align_k);
  if (_raw == nullptr)
    return memory_shared_ptr_t<T>{};
  auto *_control = ::new (static_cast<void *>(_raw)) detail::memory_control_base_t{};
  T *_object = ::new (static_cast<void *>(_raw + _object_offset_k)) T(std::forward<Args>(args)...);
  _control->object_ = _object;
  _control->shard_ = shard;
  _control->destroy_ = +[](memory_shard_t &owning_shard, void *pointer) {
    auto *_control = static_cast<detail::memory_control_base_t *>(pointer);
    static_cast<T *>(_control->object_)->~T();
    owning_shard.deallocate_bytes(pointer);
  };
  if constexpr (std::derived_from<T, memory_enable_shared_from_this_t<T>>)
    _object->owner_ = _control;
  memory_shared_ptr_t<T> _result;
  _result.ctl_ = _control;
  _result.obj_ = _object;
  return _result;
}
} // namespace frameworkcpp::memory
