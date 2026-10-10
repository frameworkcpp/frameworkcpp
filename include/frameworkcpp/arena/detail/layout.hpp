// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <shared_mutex>
#include <span>
#include <thread>

#include "frameworkcpp/arena/config.hpp"

namespace frameworkcpp::arena::detail {
inline constexpr std::size_t kBlockHeaderSize = 8;
inline constexpr std::size_t kNextSlotOffset = 8;
inline constexpr std::size_t kLayoutBytes = 24;
inline constexpr std::size_t kSlabAlign = 64;
inline constexpr std::size_t kMaxBucketAlign = 64;
inline constexpr std::size_t kPageSize = 4096;

struct block_header {
    std::uint32_t bucket;
    std::uint32_t shard;
};

struct shard_header {
    std::uint32_t shard_id;
    std::uint32_t bucket_count;
    std::uint64_t tag;
    std::size_t slice_bytes;
};

struct bucket_descriptor {
    std::uint64_t tag;
    std::size_t object_size;
    std::size_t align;
    std::size_t stride;
    std::uint32_t count;
    std::uint32_t run;
    std::int64_t head;
    std::int64_t top;
    std::uint32_t model;
    std::size_t shm;
    std::int64_t p;
    std::int64_t c;
    std::size_t ring_off;
    std::size_t seq_off;
    std::int64_t in_use;
    std::int64_t freed;
};

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr std::size_t align_up_c(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

inline bool is_pow2(std::size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

inline bool spec_valid(const object_spec_t &s) {
    return s.object_size != 0 && s.count != 0 && is_pow2(s.align) && s.align <= kMaxBucketAlign;
}

inline std::size_t head_spacing(std::size_t alignment) {
    return align_up(kLayoutBytes, alignment);
}

inline std::size_t stride_needed(std::size_t object_size, std::size_t alignment) {
    return align_up(head_spacing(alignment) + object_size, alignment);
}

constexpr std::size_t head_spacing_c(std::size_t alignment) {
    return align_up_c(kLayoutBytes, alignment);
}

constexpr std::size_t stride_needed_c(std::size_t object_size, std::size_t alignment) {
    return align_up_c(head_spacing_c(alignment) + object_size, alignment);
}

inline std::size_t slice_bytes_runtime(std::span<const object_spec_t> specs) {
    std::size_t off = align_up(sizeof(shard_header), 8);
    off += specs.size() * sizeof(bucket_descriptor);
    off = align_up(off, alignof(std::shared_mutex));
    off += specs.size() * sizeof(std::shared_mutex);
    for (const auto &s : specs) {
        off += s.count * sizeof(std::int64_t);
        off += s.count * sizeof(std::int64_t);
    }
    for (const auto &s : specs)
        off += (s.count * stride_needed(s.object_size, s.align));
    return align_up(off, kSlabAlign);
}

template <std::size_t N> constexpr std::size_t slice_bytes_c(const object_spec_t (&specs)[N]) {
    std::size_t off = align_up_c(sizeof(shard_header), 8);
    off += N * sizeof(bucket_descriptor);
    off = align_up_c(off, alignof(std::shared_mutex));
    off += N * sizeof(std::shared_mutex);
    for (std::size_t i = 0; i < N; ++i) {
        off += specs[i].count * sizeof(std::int64_t);
        off += specs[i].count * sizeof(std::int64_t);
    }
    for (std::size_t i = 0; i < N; ++i)
        off += (specs[i].count * stride_needed_c(specs[i].object_size, specs[i].align));
    return align_up_c(off, kSlabAlign);
}

inline std::byte *run_base(std::byte *base, const bucket_descriptor &d) {
    return base + d.run;
}

inline std::byte *block_base(std::byte *base, const bucket_descriptor &d, std::int64_t idx) {
    return run_base(base, d) + (idx * d.stride);
}

inline std::byte *next_slot(std::byte *b) {
    return b + kNextSlotOffset;
}

inline std::int64_t block_next(std::byte *b) {
    std::int64_t v = 0;
    __builtin_memcpy(&v, next_slot(b), sizeof(v));
    return v;
}

inline void store_block_next(std::byte *b, std::int64_t next) {
    __builtin_memcpy(next_slot(b), &next, sizeof(next));
}

inline std::byte *payload_of(std::byte *b, const bucket_descriptor &d) {
    return b + head_spacing(d.align);
}

inline std::byte *block_of_payload(std::byte *p) {
    if (p == nullptr)
        return nullptr;
    std::int64_t base = 0;
    __builtin_memcpy(&base, p - 8, sizeof(base));
    // NOLINTNEXTLINE(performance-no-int-to-ptr) -- builtin anchor stores a raw address
    return reinterpret_cast<std::byte *>(base);
}

inline void store_anchor(std::byte *p, std::byte *b) {
    auto v = reinterpret_cast<std::int64_t>(b);
    __builtin_memcpy(p - 8, &v, sizeof(v));
}

inline std::shared_mutex &shard_mutex(std::byte *base, const bucket_descriptor &d) {
    return *reinterpret_cast<std::shared_mutex *>(base + d.shm);
}

inline std::int64_t *ring_slot(std::byte *base, const bucket_descriptor &d, std::int64_t i) {
    return reinterpret_cast<std::int64_t *>(base + d.ring_off) + i;
}

inline std::int64_t *seq_slot(std::byte *base, const bucket_descriptor &d, std::int64_t i) {
    return reinterpret_cast<std::int64_t *>(base + d.seq_off) + i;
}

inline std::int64_t ring_index(std::int64_t value, std::int64_t size) {
    return value % size;
}

inline std::int64_t fresh_top(bucket_descriptor &d) {
    if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread) {
        if (d.top < static_cast<std::int64_t>(d.count))
            return d.top++;
        return -1;
    }
    const std::atomic_ref<std::int64_t> top{d.top};
    for (;;) {
        auto t = top.load(std::memory_order_relaxed);
        if (t >= static_cast<std::int64_t>(d.count))
            return -1;
        if (top.compare_exchange_weak(
                t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed))
            return t;
    }
}

inline std::int64_t reuse_top(std::byte *base, bucket_descriptor &d) {
    if (d.head >= 0) {
        const auto idx = d.head;
        d.head = block_next(block_base(base, d, idx));
        return idx;
    }
    return -1;
}

inline void reuse_push(std::byte *base, bucket_descriptor &d, std::int64_t idx) {
    store_block_next(block_base(base, d, idx), d.head);
    d.head = idx;
}

inline std::int64_t ring_pop(std::byte *base, bucket_descriptor &d) {
    const std::int64_t wrap = d.count;
    const std::atomic_ref<std::int64_t> cval{d.c};
    const std::atomic_ref<std::int64_t> pval{d.p};
    for (;;) {
        auto c = cval.load(std::memory_order_relaxed);
        if (c == pval.load(std::memory_order_acquire))
            return -1;
        const auto slot = ring_index(c, wrap);
        if (std::atomic_ref<std::int64_t>(*seq_slot(base, d, slot))
                .load(std::memory_order_acquire) != c + 1) {
            std::this_thread::yield();
            continue;
        }
        const auto idx = std::atomic_ref<std::int64_t>(*ring_slot(base, d, slot))
                             .load(std::memory_order_relaxed);
        if (cval.compare_exchange_weak(
                c, c + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            return idx;
    }
}

inline bool ring_push(std::byte *base, bucket_descriptor &d, std::int64_t idx) {
    const std::int64_t wrap = d.count;
    const std::atomic_ref<std::int64_t> cval{d.c};
    const std::atomic_ref<std::int64_t> pval{d.p};
    for (;;) {
        auto p = pval.load(std::memory_order_relaxed);
        if (p - cval.load(std::memory_order_acquire) >= wrap) {
            std::this_thread::yield();
            continue;
        }
        if (!pval.compare_exchange_weak(
                p, p + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            continue;
        const auto slot = ring_index(p, wrap);
        std::atomic_ref<std::int64_t>(*ring_slot(base, d, slot))
            .store(idx, std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(*seq_slot(base, d, slot))
            .store(p + 1, std::memory_order_release);
        return true;
    }
}

inline std::int64_t alloc_block(std::byte *base, bucket_descriptor &d) {
    switch (static_cast<concurrency_model>(d.model)) {
    case concurrency_model::single_thread: {
        const auto idx = reuse_top(base, d);
        return idx >= 0 ? idx : fresh_top(d);
    }
    case concurrency_model::lock_free: {
        const auto idx = ring_pop(base, d);
        return idx >= 0 ? idx : fresh_top(d);
    }
    case concurrency_model::thread_safe: {
        const std::scoped_lock lock{shard_mutex(base, d)};
        const auto idx = reuse_top(base, d);
        return idx >= 0 ? idx : fresh_top(d);
    }
    }
    return -1;
}

inline void note_alloc(bucket_descriptor &d) {
    if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread) {
        ++d.in_use;
        return;
    }
    std::atomic_ref<std::int64_t>(d.in_use).fetch_add(1, std::memory_order_relaxed);
}

inline void note_free(bucket_descriptor &d) {
    if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread) {
        --d.in_use;
        ++d.freed;
        return;
    }
    std::atomic_ref<std::int64_t>(d.in_use).fetch_sub(1, std::memory_order_relaxed);
    std::atomic_ref<std::int64_t>(d.freed).fetch_add(1, std::memory_order_relaxed);
}

inline std::byte *
take_block(std::byte *slice, bucket_descriptor &d, std::uint32_t bucket, std::uint32_t shard) {
    const auto idx = alloc_block(slice, d);
    if (idx < 0)
        return nullptr;
    note_alloc(d);
    auto *b = block_base(slice, d, idx);
    block_header hdr{};
    hdr.bucket = bucket;
    hdr.shard = shard;
    __builtin_memcpy(b, &hdr, sizeof(hdr));
    auto *payload = payload_of(b, d);
    store_anchor(payload, b);
    return payload;
}

inline void free_block(std::byte *base, bucket_descriptor &d, std::int64_t idx) {
    switch (static_cast<concurrency_model>(d.model)) {
    case concurrency_model::single_thread:
        reuse_push(base, d, idx);
        return;
    case concurrency_model::lock_free:
        ring_push(base, d, idx);
        return;
    case concurrency_model::thread_safe: {
        const std::scoped_lock lock{shard_mutex(base, d)};
        reuse_push(base, d, idx);
        return;
    }
    }
}

inline void adopt_bucket(bucket_descriptor &d, concurrency_model model) {
    if (d.model == static_cast<std::uint32_t>(model))
        return;
    if (d.model != static_cast<std::uint32_t>(concurrency_model::single_thread) || d.head != -1 ||
        d.top != 0 || d.p != 0 || d.c != 0)
        std::abort();
    d.model = static_cast<std::uint32_t>(model);
}
} // namespace frameworkcpp::arena::detail