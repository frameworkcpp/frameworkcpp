// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <memory_resource>
#include <type_traits>

#include "frameworkcpp/arena/shard.hpp"

namespace frameworkcpp::arena {
template <typename T> class arena_allocator {
  public:
    using value_type = T;

    arena_allocator() noexcept = default;
    explicit arena_allocator(arena_shard_t shard) noexcept : shard_(shard) {}

    template <typename U>
    arena_allocator(const arena_allocator<U> &other) noexcept : shard_(other.shard_) {}

    T *allocate(std::size_t n) {
        auto *p = shard_.allocate_bytes(n * sizeof(T), alignof(T));
        if (p == nullptr)
            throw std::bad_alloc();
        return reinterpret_cast<T *>(p);
    }

    void deallocate(T *p, [[maybe_unused]] std::size_t count) noexcept {
        shard_.deallocate_bytes(p);
    }

    template <typename U> struct rebind {
        using other = arena_allocator<U>;
    };

    // Standard allocator traits. Shard-backed allocators are interchangeable
    // only when they share a shard, so is_always_equal is false.
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;

    [[nodiscard]] arena_allocator select_on_container_copy_construction() const noexcept {
        return *this;
    }

    template <typename U> friend class arena_allocator;

    // Two empty (default-constructed) allocators compare equal; a shard-backed
    // allocator equals only another allocator over the same shard. An empty
    // allocator is never equal to a shard-backed one.
    bool operator==(const arena_allocator &other) const noexcept {
        const bool self = static_cast<bool>(shard_);
        const bool that = static_cast<bool>(other.shard_);
        if (!self && !that)
            return true;
        return self && that && shard_.shard_id() == other.shard_.shard_id();
    }

    bool operator!=(const arena_allocator &other) const noexcept { return !(*this == other); }

    [[nodiscard]] arena_shard_t shard() const noexcept { return shard_; }

  private:
    arena_shard_t shard_;
};

using arena_byte_allocator = arena_allocator<std::byte>;

class arena_memory_resource : public std::pmr::memory_resource {
  public:
    arena_memory_resource() noexcept = default;
    explicit arena_memory_resource(arena_shard_t shard) noexcept : shard_(shard) {}

    [[nodiscard]] arena_shard_t shard() const noexcept { return shard_; }

  protected:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *p = shard_.allocate_bytes(bytes, alignment);
        if (p == nullptr)
            throw std::bad_alloc();
        return p;
    }

    void do_deallocate(void *p,
                       [[maybe_unused]] std::size_t bytes,
                       [[maybe_unused]] std::size_t alignment) override {
        shard_.deallocate_bytes(p);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        if (auto *o = dynamic_cast<const arena_memory_resource *>(&other))
            return shard_.shard_id() == o->shard_.shard_id();
        return false;
    }

  private:
    arena_shard_t shard_;
};
} // namespace frameworkcpp::arena