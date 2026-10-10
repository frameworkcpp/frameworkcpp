// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena::detail
{
    struct arena_control_base
    {
        std::atomic<std::int32_t> strong{1};
        void *object = nullptr;
        arena_shard_t shard;
        void (*destroy)(arena_shard_t &, void *) = nullptr;
    };
} // namespace frameworkcpp::arena::detail

namespace frameworkcpp::arena
{
    template <typename T>
    class arena_enable_shared_from_this;

    template <typename T>
    class arena_unique_ptr_t
    {
    public:
        arena_unique_ptr_t() noexcept = default;

        arena_unique_ptr_t(T *p, arena_shard_t shard) noexcept : p_(p), shard_(shard) {}

        arena_unique_ptr_t(arena_unique_ptr_t &&other) noexcept
            : p_(other.p_)
            , shard_(other.shard_)
        {
            other.p_ = nullptr;
        }

        arena_unique_ptr_t &operator=(arena_unique_ptr_t &&other) noexcept
        {
            if (this != &other)
            {
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

        T *get() const noexcept { return p_; }
        T *operator->() const noexcept { return p_; }
        T &operator*() const noexcept { return *p_; }
        explicit operator bool() const noexcept { return p_ != nullptr; }

        void reset() noexcept
        {
            if (p_ == nullptr)
                return;
            p_->~T();
            shard_.deallocate_bytes(p_);
            p_ = nullptr;
        }

    private:
        T *p_ = nullptr;
        arena_shard_t shard_;
    };

    template <typename T>
    class arena_shared_ptr_t
    {
    public:
        arena_shared_ptr_t() noexcept = default;

        arena_shared_ptr_t(const arena_shared_ptr_t &other) noexcept
            : ctl_(other.ctl_)
            , obj_(other.obj_)
        {
            if (ctl_)
                ctl_->strong.fetch_add(1, std::memory_order_relaxed);
        }

        arena_shared_ptr_t &operator=(const arena_shared_ptr_t &other) noexcept
        {
            if (this != &other)
            {
                reset();
                ctl_ = other.ctl_;
                obj_ = other.obj_;
                if (ctl_)
                    ctl_->strong.fetch_add(1, std::memory_order_relaxed);
            }
            return *this;
        }

        arena_shared_ptr_t(arena_shared_ptr_t &&other) noexcept
            : ctl_(other.ctl_)
            , obj_(other.obj_)
        {
            other.ctl_ = nullptr;
            other.obj_ = nullptr;
        }

        arena_shared_ptr_t &operator=(arena_shared_ptr_t &&other) noexcept
        {
            if (this != &other)
            {
                reset();
                ctl_ = other.ctl_;
                obj_ = other.obj_;
                other.ctl_ = nullptr;
                other.obj_ = nullptr;
            }
            return *this;
        }

        template <typename U>
        arena_shared_ptr_t(const arena_shared_ptr_t<U> &other) noexcept
            : ctl_(other.ctl_)
            , obj_(other.obj_)
        {
            if (ctl_)
                ctl_->strong.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename U>
        arena_shared_ptr_t(arena_shared_ptr_t<U> &&other) noexcept
            : ctl_(other.ctl_)
            , obj_(other.obj_)
        {
            other.ctl_ = nullptr;
            other.obj_ = nullptr;
        }

        ~arena_shared_ptr_t() { reset(); }

        T *get() const noexcept { return obj_; }
        T *operator->() const noexcept { return obj_; }
        T &operator*() const noexcept { return *obj_; }
        explicit operator bool() const noexcept { return obj_ != nullptr; }

        void reset() noexcept
        {
            if (ctl_ == nullptr)
                return;
            if (ctl_->strong.fetch_sub(1, std::memory_order_acq_rel) == 1)
                ctl_->destroy(ctl_->shard, ctl_);
            ctl_ = nullptr;
            obj_ = nullptr;
        }

    private:
        template <typename U>
        friend class arena_shared_ptr_t;
        template <typename U, typename... A>
        friend arena_shared_ptr_t<U> make_arena_shared_ptr(arena_shard_t &, A &&...);
        friend class arena_enable_shared_from_this<T>;

        detail::arena_control_base *ctl_ = nullptr;
        T *obj_ = nullptr;
    };

    template <typename T>
    class arena_enable_shared_from_this
    {
    public:
        arena_shared_ptr_t<T> shared_from_this()
        {
            if (owner_ == nullptr)
                throw std::bad_weak_ptr();
            arena_shared_ptr_t<T> result;
            result.ctl_ = owner_;
            result.obj_ = static_cast<T *>(owner_->object);
            owner_->strong.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

    private:
        template <typename U, typename... A>
        friend arena_shared_ptr_t<U> make_arena_shared_ptr(arena_shard_t &, A &&...);
        detail::arena_control_base *owner_ = nullptr;
    };

    template <typename T, typename... Args>
    arena_unique_ptr_t<T> make_arena_unique_ptr(arena_shard_t &shard, Args &&...args)
    {
        auto *raw = shard.allocate_bytes(sizeof(T), alignof(T));
        if (raw == nullptr)
            return arena_unique_ptr_t<T>{};
        T *p = ::new (static_cast<void *>(raw)) T(std::forward<Args>(args)...);
        return arena_unique_ptr_t<T>(p, shard);
    }

    template <typename T, typename... Args>
    arena_shared_ptr_t<T> make_arena_shared_ptr(arena_shard_t &shard, Args &&...args)
    {
        constexpr std::size_t ctl_size = sizeof(detail::arena_control_base);
        constexpr std::size_t ctl_align = alignof(detail::arena_control_base);
        constexpr std::size_t align = alignof(T) > ctl_align ? alignof(T) : ctl_align;
        constexpr std::size_t obj_offset = (ctl_size + alignof(T) - 1) & ~(alignof(T) - 1);
        auto *raw = shard.allocate_bytes(obj_offset + sizeof(T), align);
        if (raw == nullptr)
            return arena_shared_ptr_t<T>{};
        auto *ctl = ::new (static_cast<void *>(raw)) detail::arena_control_base{};
        T *obj = ::new (static_cast<void *>(raw + obj_offset)) T(std::forward<Args>(args)...);
        ctl->object = obj;
        ctl->shard = shard;
        ctl->destroy = +[](arena_shard_t &s, void *p) {
            auto *c = static_cast<detail::arena_control_base *>(p);
            static_cast<T *>(c->object)->~T();
            s.deallocate_bytes(p);
        };
        if constexpr (std::derived_from<T, arena_enable_shared_from_this<T>>)
            obj->owner_ = ctl;
        arena_shared_ptr_t<T> result;
        result.ctl_ = ctl;
        result.obj_ = obj;
        return result;
    }
} // namespace frameworkcpp::arena