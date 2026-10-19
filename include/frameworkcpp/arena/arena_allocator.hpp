// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <memory_resource>
#include <type_traits>

#include "frameworkcpp/arena/shard.hpp"

namespace frameworkcpp::arena {
template <typename T> class arena_allocator_t {
  arena_shard_t shard_;

  public:
  using value_type = T;

  arena_allocator_t() noexcept = default;
  explicit arena_allocator_t(arena_shard_t shard) noexcept : shard_(shard) {}

  template <typename U>
  arena_allocator_t(const arena_allocator_t<U> &other) noexcept : shard_(other.shard_) {}

  T *allocate(std::size_t count) {
    auto *_pointer = shard_.allocate_bytes(count * sizeof(T), alignof(T));
    if (_pointer == nullptr)
      throw std::bad_alloc();
    return reinterpret_cast<T *>(_pointer);
  }

  void deallocate(T *pointer, [[maybe_unused]] std::size_t count) noexcept {
    shard_.deallocate_bytes(pointer);
  }

  template <typename U> struct rebind {
    using other = arena_allocator_t<U>;
  };

  // Standard allocator traits. Shard-backed allocators are interchangeable
  // only when they share a shard, so is_always_equal is false.
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::false_type;
  using propagate_on_container_swap = std::false_type;
  using is_always_equal = std::false_type;

  [[nodiscard]] arena_allocator_t select_on_container_copy_construction() const noexcept {
    return *this;
  }

  template <typename U> friend class arena_allocator_t;

  // Two empty (default-constructed) allocators compare equal; a shard-backed
  // allocator equals only another allocator over the same shard. An empty
  // allocator is never equal to a shard-backed one.
  bool operator==(const arena_allocator_t &other) const noexcept {
    const bool _self_k = static_cast<bool>(shard_);
    const bool _that_k = static_cast<bool>(other.shard_);
    if (!_self_k && !_that_k)
      return true;
    return _self_k && _that_k && shard_.get_shard_id() == other.shard_.get_shard_id();
  }

  bool operator!=(const arena_allocator_t &other) const noexcept { return !(*this == other); }

  [[nodiscard]] arena_shard_t get_shard() const noexcept { return shard_; }
}; // class arena_allocator_t

using arena_byte_allocator_t = arena_allocator_t<std::byte>;

class arena_memory_resource_t : public std::pmr::memory_resource {
  arena_shard_t shard_;

  public:
  arena_memory_resource_t() noexcept = default;
  explicit arena_memory_resource_t(arena_shard_t shard) noexcept : shard_(shard) {}

  [[nodiscard]] arena_shard_t get_shard() const noexcept { return shard_; }

  protected:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override {
    auto *_pointer = shard_.allocate_bytes(bytes, alignment);
    if (_pointer == nullptr)
      throw std::bad_alloc();
    return _pointer;
  }

  void do_deallocate(void *pointer,
                     [[maybe_unused]] std::size_t bytes,
                     [[maybe_unused]] std::size_t alignment) override {
    shard_.deallocate_bytes(pointer);
  }

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    if (auto *_other = dynamic_cast<const arena_memory_resource_t *>(&other))
      return shard_.get_shard_id() == _other->shard_.get_shard_id();
    return false;
  }
}; // class arena_memory_resource_t
} // namespace frameworkcpp::arena
