// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <memory_resource>
#include <type_traits>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena
{
    template <typename T>
    class arena_allocator
    {
    public:
        using value_type = T;

        arena_allocator() noexcept = default;
        explicit arena_allocator(arena_shard_t shard) noexcept : shard_(shard) {}

        template <typename U>
        arena_allocator(const arena_allocator<U> &other) noexcept : shard_(other.shard_) {}

        T *allocate(std::size_t n)
        {
            auto *p = shard_.allocate_bytes(n * sizeof(T), alignof(T));
            if (p == nullptr)
                throw std::bad_alloc();
            return reinterpret_cast<T *>(p);
        }

        void deallocate(T *p, std::size_t) noexcept { shard_.deallocate_bytes(p); }

        template <typename U>
        struct rebind
        {
            using other = arena_allocator<U>;
        };

        template <typename U>
        friend class arena_allocator;

        bool operator==(const arena_allocator &other) const noexcept
        {
            return static_cast<bool>(shard_) && other.shard_.shard_id() == shard_.shard_id();
        }

        bool operator!=(const arena_allocator &other) const noexcept { return !(*this == other); }

        arena_shard_t shard() const noexcept { return shard_; }

    private:
        arena_shard_t shard_;
    };

    using arena_byte_allocator = arena_allocator<std::byte>;

    class arena_memory_resource : public std::pmr::memory_resource
    {
    public:
        arena_memory_resource() noexcept = default;
        explicit arena_memory_resource(arena_shard_t shard) noexcept : shard_(shard) {}

        arena_shard_t shard() const noexcept { return shard_; }

    protected:
        void *do_allocate(std::size_t bytes, std::size_t alignment) override
        {
            auto *p = shard_.allocate_bytes(bytes, alignment);
            if (p == nullptr)
                throw std::bad_alloc();
            return p;
        }

        void do_deallocate(void *p, std::size_t, std::size_t) override { shard_.deallocate_bytes(p); }

        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override
        {
            if (auto *o = dynamic_cast<const arena_memory_resource *>(&other))
                return shard_.shard_id() == o->shard_.shard_id();
            return false;
        }

    private:
        arena_shard_t shard_;
    };
} // namespace frameworkcpp::arena