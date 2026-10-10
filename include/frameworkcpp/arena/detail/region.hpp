// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace frameworkcpp::arena::detail
{
    class region
    {
    public:
        region() = default;
        region(const region &) = delete;
        region &operator=(const region &) = delete;

        region(region &&other) noexcept
            : base_(other.base_)
            , bytes_(other.bytes_)
            , free_top_(other.free_top_)
        {
            other.base_ = nullptr;
            other.bytes_ = 0;
            other.free_top_ = 0;
        }

        region &operator=(region &&other) noexcept
        {
            if (this != &other)
            {
                release();
                base_ = other.base_;
                bytes_ = other.bytes_;
                free_top_ = other.free_top_;
                other.base_ = nullptr;
                other.bytes_ = 0;
                other.free_top_ = 0;
            }
            return *this;
        }

        ~region() { release(); }

        bool reserve(std::size_t bytes, bool touch_pages)
        {
            if (base_ != nullptr)
                return false;
            const int flags = MAP_PRIVATE | MAP_ANONYMOUS | (touch_pages ? MAP_POPULATE : 0);
            auto *p = static_cast<std::byte *>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0));
            if (p == MAP_FAILED)
                return false;
            base_ = p;
            bytes_ = bytes;
            free_top_ = 0;
            return true;
        }

        std::byte *base() const { return base_; }
        std::size_t bytes() const { return bytes_; }
        std::size_t free_top() const { return free_top_; }

        bool can_fit(std::size_t bytes) const { return free_top_ + bytes <= bytes_; }

        std::byte *take(std::size_t bytes)
        {
            if (!can_fit(bytes))
                return nullptr;
            auto *p = base_ + free_top_;
            free_top_ += bytes;
            return p;
        }

    private:
        void release()
        {
            if (base_ != nullptr)
            {
                ::munmap(base_, bytes_);
                base_ = nullptr;
                bytes_ = 0;
                free_top_ = 0;
            }
        }

        std::byte *base_ = nullptr;
        std::size_t bytes_ = 0;
        std::size_t free_top_ = 0;
    };

    inline std::size_t align_up(std::size_t value, std::size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    constexpr std::size_t align_up_c(std::size_t value, std::size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }
} // namespace frameworkcpp::arena::detail