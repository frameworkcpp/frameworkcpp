// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "frameworkcpp/arena/bucket.hpp"
#include "frameworkcpp/arena/detail/layout.hpp"

namespace frameworkcpp::arena {
class arena_shard_t {
  public:
    arena_shard_t() = default;

    explicit operator bool() const noexcept { return hdr_ != nullptr; }

    template <typename T> T *allocate(std::size_t count = 1) {
        return reinterpret_cast<T *>(allocate_bytes(count * sizeof(T), alignof(T)));
    }

    template <typename T> void deallocate(T *p) noexcept { deallocate_bytes(p); }

    std::byte *allocate_bytes(std::size_t bytes, std::size_t alignment) {
        if (hdr_ == nullptr)
            return nullptr;
        const auto count = hdr_->bucket_count;
        std::int32_t best = -1;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto &d = buckets()[i];
            if (d.object_size >= bytes && d.align >= alignment) {
                if (best < 0 || d.object_size < buckets()[best].object_size)
                    best = static_cast<std::int32_t>(i);
            }
        }
        if (best < 0)
            return nullptr;
        return take_block(static_cast<std::uint32_t>(best));
    }

    std::byte *allocate_tagged(std::uint64_t tag, std::size_t bytes, std::size_t alignment) {
        if (hdr_ == nullptr)
            return nullptr;
        const auto count = hdr_->bucket_count;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto &d = buckets()[i];
            if (d.tag == tag && d.object_size >= bytes && d.align >= alignment)
                return take_block(i);
        }
        return nullptr;
    }

    void deallocate_bytes(void *p) noexcept {
        if (hdr_ == nullptr || p == nullptr)
            return;
        auto *payload = static_cast<std::byte *>(p);
        if (payload < bytes() || payload >= slice_end())
            return;
        auto *b = detail::block_of_payload(payload);
        if (b < bytes() || b >= slice_end())
            return;
        detail::block_header hdr{};
        __builtin_memcpy(&hdr, b, sizeof(hdr));
        if (hdr.shard != hdr_->shard_id || hdr.bucket >= hdr_->bucket_count)
            return;
        auto &d = buckets()[hdr.bucket];
        const auto index = static_cast<std::int64_t>((b - detail::run_base(bytes(), d)) / d.stride);
        detail::note_free(d);
        detail::free_block(bytes(), d, index);
    }

    arena_bucket_t get_bucket(std::uint64_t tag, concurrency_model model) {
        if (hdr_ == nullptr)
            return {};
        const auto count = hdr_->bucket_count;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto &d = buckets()[i];
            if (d.tag != tag)
                continue;
            detail::adopt_bucket(d, model);
            return {bytes(), hdr_->shard_id, i, &d};
        }
        return {};
    }

    [[nodiscard]] std::uint32_t bucket_count() const noexcept {
        return (hdr_ != nullptr) ? hdr_->bucket_count : 0;
    }

    [[nodiscard]] bucket_stats_t stats(std::uint32_t index) const noexcept {
        if (hdr_ == nullptr || index >= hdr_->bucket_count)
            return bucket_stats_t{};
        const auto &d = buckets()[index];
        bucket_stats_t out{};
        out.tag = d.tag;
        out.object_size = d.object_size;
        out.align = d.align;
        out.capacity = d.count;
        if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread) {
            out.in_use = d.in_use;
            out.freed = d.freed;
        } else {
            out.in_use =
                std::atomic_ref<const std::int64_t>(d.in_use).load(std::memory_order_relaxed);
            out.freed =
                std::atomic_ref<const std::int64_t>(d.freed).load(std::memory_order_relaxed);
        }
        return out;
    }

    [[nodiscard]] std::uint64_t tag() const noexcept { return (hdr_ != nullptr) ? hdr_->tag : 0; }

    [[nodiscard]] std::uint32_t shard_id() const noexcept {
        return (hdr_ != nullptr) ? hdr_->shard_id : 0;
    }

  private:
    friend class arena_t;

    [[nodiscard]] std::byte *bytes() const noexcept { return reinterpret_cast<std::byte *>(hdr_); }
    [[nodiscard]] std::byte *slice_end() const noexcept { return bytes() + hdr_->slice_bytes; }

    [[nodiscard]] detail::bucket_descriptor *buckets() noexcept {
        return reinterpret_cast<detail::bucket_descriptor *>(hdr_ + 1);
    }

    [[nodiscard]] const detail::bucket_descriptor *buckets() const noexcept {
        return reinterpret_cast<const detail::bucket_descriptor *>(hdr_ + 1);
    }

    std::byte *take_block(std::uint32_t bucket) {
        auto *d = &buckets()[bucket];
        return detail::take_block(bytes(), *d, bucket, hdr_->shard_id);
    }

    detail::shard_header *hdr_ = nullptr;
};

template <std::size_t N> constexpr std::size_t shard_footprint(const object_spec_t (&specs)[N]) {
    return detail::slice_bytes_c(specs);
}

constexpr std::size_t round_to_page(std::size_t bytes) {
    return (bytes + detail::kPageSize - 1) & ~(detail::kPageSize - 1);
}

constexpr bool valid_footprint(std::size_t bytes) {
    return bytes > 0;
}
} // namespace frameworkcpp::arena