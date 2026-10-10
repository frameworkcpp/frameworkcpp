// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frameworkcpp/arena/detail/region.hpp"

namespace frameworkcpp::arena
{
    enum class concurrency_model : std::uint32_t
    {
        single_thread = 0,
        lock_free = 1,
        thread_safe = 2,
    };

    struct object_spec
    {
        std::uint64_t tag;
        std::size_t object_size;
        std::size_t align = alignof(std::max_align_t);
        std::size_t count;
    };

    struct arena_config
    {
        std::size_t size = 1024 * 1024;
        bool touch_pages = false;
    };

    struct bucket_stats
    {
        std::uint64_t tag;
        std::size_t object_size;
        std::size_t align;
        std::uint32_t capacity;
        std::int64_t in_use;
        std::int64_t freed;
    };

    namespace detail
    {
        inline constexpr std::size_t kBlockHeaderSize = 8;
        inline constexpr std::size_t kNextSlotOffset = 8;
        inline constexpr std::size_t kLayoutBytes = 24;
        inline constexpr std::size_t kSlabAlign = 64;
        inline constexpr std::size_t kMaxBucketAlign = 64;
        inline constexpr std::size_t kPageSize = 4096;

        struct block_header
        {
            std::uint32_t bucket;
            std::uint32_t shard;
        };

        struct shard_header
        {
            std::uint32_t shard_id;
            std::uint32_t bucket_count;
            std::uint64_t tag;
            std::size_t slice_bytes;
        };

        struct bucket_descriptor
        {
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

        inline bool is_pow2(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

        inline bool spec_valid(const object_spec &s)
        {
            return s.object_size != 0 && s.count != 0 && is_pow2(s.align) && s.align <= kMaxBucketAlign;
        }

        inline std::size_t head_spacing(std::size_t alignment) { return align_up(kLayoutBytes, alignment); }
        inline std::size_t stride_needed(std::size_t object_size, std::size_t alignment)
        {
            return align_up(head_spacing(alignment) + object_size, alignment);
        }
        inline constexpr std::size_t head_spacing_c(std::size_t alignment)
        {
            return align_up_c(kLayoutBytes, alignment);
        }
        inline constexpr std::size_t stride_needed_c(std::size_t object_size, std::size_t alignment)
        {
            return align_up_c(head_spacing_c(alignment) + object_size, alignment);
        }

        inline std::size_t slice_bytes_runtime(std::span<const object_spec> specs)
        {
            std::size_t off = align_up(sizeof(shard_header), 8);
            off += specs.size() * sizeof(bucket_descriptor);
            off = align_up(off, alignof(std::shared_mutex));
            off += specs.size() * sizeof(std::shared_mutex);
            for (const auto &s : specs)
            {
                off += s.count * sizeof(std::int64_t);
                off += s.count * sizeof(std::int64_t);
            }
            for (const auto &s : specs)
                off += s.count * stride_needed(s.object_size, s.align);
            return align_up(off, kSlabAlign);
        }

        template <std::size_t N>
        inline constexpr std::size_t slice_bytes_c(const object_spec (&specs)[N])
        {
            std::size_t off = align_up_c(sizeof(shard_header), 8);
            off += N * sizeof(bucket_descriptor);
            off = align_up_c(off, alignof(std::shared_mutex));
            off += N * sizeof(std::shared_mutex);
            for (std::size_t i = 0; i < N; ++i)
            {
                off += specs[i].count * sizeof(std::int64_t);
                off += specs[i].count * sizeof(std::int64_t);
            }
            for (std::size_t i = 0; i < N; ++i)
                off += specs[i].count * stride_needed_c(specs[i].object_size, specs[i].align);
            return align_up_c(off, kSlabAlign);
        }

        inline std::byte *run_base(std::byte *base, const bucket_descriptor &d)
        {
            return base + d.run;
        }

        inline std::byte *block_base(std::byte *base, const bucket_descriptor &d, std::int64_t idx)
        {
            return run_base(base, d) + idx * d.stride;
        }

        inline std::byte *next_slot(std::byte *b) { return b + kNextSlotOffset; }

        inline std::int64_t block_next(std::byte *b)
        {
            std::int64_t v;
            __builtin_memcpy(&v, next_slot(b), sizeof(v));
            return v;
        }

        inline void store_block_next(std::byte *b, std::int64_t next)
        {
            __builtin_memcpy(next_slot(b), &next, sizeof(next));
        }

        inline std::byte *payload_of(std::byte *b, const bucket_descriptor &d)
        {
            return b + head_spacing(d.align);
        }

        inline std::byte *block_of_payload(std::byte *p)
        {
            if (p == nullptr)
                return nullptr;
            std::int64_t base;
            __builtin_memcpy(&base, p - 8, sizeof(base));
            return reinterpret_cast<std::byte *>(base);
        }

        inline void store_anchor(std::byte *p, std::byte *b)
        {
            std::int64_t v = reinterpret_cast<std::int64_t>(b);
            __builtin_memcpy(p - 8, &v, sizeof(v));
        }

        inline std::shared_mutex &shard_mutex(std::byte *base, const bucket_descriptor &d)
        {
            return *reinterpret_cast<std::shared_mutex *>(base + d.shm);
        }

        inline std::int64_t *ring_slot(std::byte *base, const bucket_descriptor &d, std::int64_t i)
        {
            return reinterpret_cast<std::int64_t *>(base + d.ring_off) + i;
        }

        inline std::int64_t *seq_slot(std::byte *base, const bucket_descriptor &d, std::int64_t i)
        {
            return reinterpret_cast<std::int64_t *>(base + d.seq_off) + i;
        }

        inline std::int64_t ring_index(std::int64_t value, std::int64_t size)
        {
            return value % size;
        }

        inline std::int64_t fresh_top(std::byte *base, bucket_descriptor &d)
        {
            if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread)
            {
                if (d.top < static_cast<std::int64_t>(d.count))
                    return d.top++;
                return -1;
            }
            std::atomic_ref<std::int64_t> top{d.top};
            for (;;)
            {
                auto t = top.load(std::memory_order_relaxed);
                if (t >= static_cast<std::int64_t>(d.count))
                    return -1;
                if (top.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed))
                    return t;
            }
        }

        inline std::int64_t reuse_top(std::byte *base, bucket_descriptor &d)
        {
            if (d.head >= 0)
            {
                const auto idx = d.head;
                d.head = block_next(block_base(base, d, idx));
                return idx;
            }
            return -1;
        }

        inline void reuse_push(std::byte *base, bucket_descriptor &d, std::int64_t idx)
        {
            store_block_next(block_base(base, d, idx), d.head);
            d.head = idx;
        }

        inline std::int64_t ring_pop(std::byte *base, bucket_descriptor &d)
        {
            const std::int64_t wrap = d.count;
            std::atomic_ref<std::int64_t> cval{d.c};
            std::atomic_ref<std::int64_t> pval{d.p};
            for (;;)
            {
                auto c = cval.load(std::memory_order_relaxed);
                if (c == pval.load(std::memory_order_acquire))
                    return -1;
                const auto slot = ring_index(c, wrap);
                if (std::atomic_ref<std::int64_t>(*seq_slot(base, d, slot)).load(std::memory_order_acquire) != c + 1)
                {
                    std::this_thread::yield();
                    continue;
                }
                const auto idx = std::atomic_ref<std::int64_t>(*ring_slot(base, d, slot)).load(std::memory_order_relaxed);
                if (cval.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                    return idx;
            }
        }

        inline bool ring_push(std::byte *base, bucket_descriptor &d, std::int64_t idx)
        {
            const std::int64_t wrap = d.count;
            std::atomic_ref<std::int64_t> cval{d.c};
            std::atomic_ref<std::int64_t> pval{d.p};
            for (;;)
            {
                auto p = pval.load(std::memory_order_relaxed);
                if (p - cval.load(std::memory_order_acquire) >= wrap)
                {
                    std::this_thread::yield();
                    continue;
                }
                if (!pval.compare_exchange_weak(p, p + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                    continue;
                const auto slot = ring_index(p, wrap);
                std::atomic_ref<std::int64_t>(*ring_slot(base, d, slot)).store(idx, std::memory_order_relaxed);
                std::atomic_ref<std::int64_t>(*seq_slot(base, d, slot)).store(p + 1, std::memory_order_release);
                return true;
            }
        }

        inline std::int64_t alloc_block(std::byte *base, bucket_descriptor &d)
        {
            switch (static_cast<concurrency_model>(d.model))
            {
            case concurrency_model::single_thread:
            {
                const auto idx = reuse_top(base, d);
                return idx >= 0 ? idx : fresh_top(base, d);
            }
            case concurrency_model::lock_free:
            {
                const auto idx = ring_pop(base, d);
                return idx >= 0 ? idx : fresh_top(base, d);
            }
            case concurrency_model::thread_safe:
            {
                std::scoped_lock lock{shard_mutex(base, d)};
                const auto idx = reuse_top(base, d);
                return idx >= 0 ? idx : fresh_top(base, d);
            }
            }
            return -1;
        }

        inline void note_alloc(bucket_descriptor &d)
        {
            if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread)
            {
                ++d.in_use;
                return;
            }
            std::atomic_ref<std::int64_t>(d.in_use).fetch_add(1, std::memory_order_relaxed);
        }

        inline void note_free(bucket_descriptor &d)
        {
            if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread)
            {
                --d.in_use;
                ++d.freed;
                return;
            }
            std::atomic_ref<std::int64_t>(d.in_use).fetch_sub(1, std::memory_order_relaxed);
            std::atomic_ref<std::int64_t>(d.freed).fetch_add(1, std::memory_order_relaxed);
        }

        inline void free_block(std::byte *base, bucket_descriptor &d, std::int64_t idx)
        {
            switch (static_cast<concurrency_model>(d.model))
            {
            case concurrency_model::single_thread:
                reuse_push(base, d, idx);
                return;
            case concurrency_model::lock_free:
                ring_push(base, d, idx);
                return;
            case concurrency_model::thread_safe:
            {
                std::scoped_lock lock{shard_mutex(base, d)};
                reuse_push(base, d, idx);
                return;
            }
            }
        }

        inline void adopt_bucket(std::byte *base, bucket_descriptor &d, concurrency_model model)
        {
            if (d.model == static_cast<std::uint32_t>(model))
                return;
            if (d.model != static_cast<std::uint32_t>(concurrency_model::single_thread) || d.head != -1 || d.top != 0 || d.p != 0 || d.c != 0)
                std::abort();
            d.model = static_cast<std::uint32_t>(model);
        }
    } // namespace detail

    class arena_t;
    class arena_shard_t;
    class arena_bucket;

    class arena_shard_t
    {
    public:
        arena_shard_t() = default;

        explicit operator bool() const noexcept { return hdr_ != nullptr; }

        template <typename T>
        T *allocate(std::size_t count = 1)
        {
            return reinterpret_cast<T *>(allocate_bytes(count * sizeof(T), alignof(T)));
        }

        template <typename T>
        void deallocate(T *p) noexcept
        {
            deallocate_bytes(p);
        }

        std::byte *allocate_bytes(std::size_t bytes, std::size_t alignment)
        {
            if (!hdr_)
                return nullptr;
            const auto count = static_cast<std::uint32_t>(hdr_->bucket_count);
            std::int32_t best = -1;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto &d = buckets()[i];
                if (d.object_size >= bytes && d.align >= alignment)
                {
                    if (best < 0 || d.object_size < buckets()[best].object_size)
                        best = static_cast<std::int32_t>(i);
                }
            }
            if (best < 0)
                return nullptr;
            return take_block(static_cast<std::uint32_t>(best));
        }

        std::byte *allocate_tagged(std::uint64_t tag, std::size_t bytes, std::size_t alignment)
        {
            if (!hdr_)
                return nullptr;
            const auto count = static_cast<std::uint32_t>(hdr_->bucket_count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto &d = buckets()[i];
                if (d.tag == tag && d.object_size >= bytes && d.align >= alignment)
                    return take_block(i);
            }
            return nullptr;
        }

        void deallocate_bytes(void *p) noexcept;

        arena_bucket create_writer(std::uint64_t tag, concurrency_model model);
        arena_bucket create_reader(std::uint64_t tag, concurrency_model model);

        std::uint32_t bucket_count() const noexcept { return hdr_ ? hdr_->bucket_count : 0; }
        bucket_stats stats(std::uint32_t index) const noexcept;
        std::uint64_t tag() const noexcept { return hdr_ ? hdr_->tag : 0; }
        std::uint32_t shard_id() const noexcept { return hdr_ ? hdr_->shard_id : 0; }

    private:
        friend class arena_t;
        friend class arena_bucket;

        std::byte *bytes() const noexcept { return reinterpret_cast<std::byte *>(hdr_); }
        std::byte *slice_end() const noexcept { return bytes() + hdr_->slice_bytes; }

        detail::bucket_descriptor *buckets() noexcept
        {
            return reinterpret_cast<detail::bucket_descriptor *>(hdr_ + 1);
        }

        const detail::bucket_descriptor *buckets() const noexcept
        {
            return reinterpret_cast<const detail::bucket_descriptor *>(hdr_ + 1);
        }

        std::byte *take_block(std::uint32_t bucket)
        {
            auto *d = &buckets()[bucket];
            const auto idx = detail::alloc_block(bytes(), *d);
            if (idx < 0)
                return nullptr;
            detail::note_alloc(*d);
            auto *b = detail::block_base(bytes(), *d, idx);
            detail::block_header hdr;
            hdr.bucket = bucket;
            hdr.shard = hdr_->shard_id;
            __builtin_memcpy(b, &hdr, sizeof(hdr));
            auto *payload = detail::payload_of(b, *d);
            detail::store_anchor(payload, b);
            return payload;
        }

        arena_t *arena_ = nullptr;
        std::uint32_t shard_id_ = 0;
        detail::shard_header *hdr_ = nullptr;
    };

    class arena_bucket
    {
    public:
        arena_bucket() = default;

        explicit operator bool() const noexcept { return desc_ != nullptr; }

        template <typename T>
        T *allocate(std::size_t count = 1)
        {
            if (!desc_ || count * sizeof(T) > desc_->object_size || alignof(T) > desc_->align)
                return nullptr;
            return reinterpret_cast<T *>(take_block_desc(desc_));
        }

        template <typename T>
        void deallocate(T *p) noexcept
        {
            deallocate_bytes(p);
        }

        void deallocate_bytes(void *p) noexcept
        {
            if (!desc_ || p == nullptr)
                return;
            auto *b = detail::block_of_payload(static_cast<std::byte *>(p));
            const auto index = (b - detail::run_base(shard_.bytes(), *desc_)) / desc_->stride;
            detail::note_free(*desc_);
            detail::free_block(shard_.bytes(), *desc_, index);
        }

        std::uint64_t tag() const noexcept { return desc_ ? desc_->tag : 0; }

    private:
        friend class arena_shard_t;
        arena_bucket(arena_shard_t shard, detail::bucket_descriptor *desc) : shard_(shard), desc_(desc) {}

        std::byte *take_block_desc(detail::bucket_descriptor *desc)
        {
            const auto idx = detail::alloc_block(shard_.bytes(), *desc);
            if (idx < 0)
                return nullptr;
            detail::note_alloc(*desc);
            auto *b = detail::block_base(shard_.bytes(), *desc, idx);
            detail::block_header hdr;
            hdr.bucket = static_cast<std::uint32_t>(desc - shard_.buckets());
            hdr.shard = shard_.hdr_->shard_id;
            __builtin_memcpy(b, &hdr, sizeof(hdr));
            auto *payload = detail::payload_of(b, *desc);
            detail::store_anchor(payload, b);
            return payload;
        }

        arena_shard_t shard_;
        detail::bucket_descriptor *desc_ = nullptr;
    };

    class arena_t
    {
    public:
        arena_t() = default;

        explicit arena_t(const arena_config &config) : config_(config) {}

        arena_shard_t create_shard(std::uint64_t tag, std::initializer_list<object_spec> specs);
        arena_shard_t create_shard(std::uint64_t tag, std::span<const object_spec> specs);
        arena_shard_t from(std::uint64_t tag) const;

        void deallocate_bytes(void *p) noexcept;
        void deallocate(void *p) noexcept { deallocate_bytes(p); }

        std::size_t shard_count() const noexcept;
        std::size_t capacity() const noexcept;
        std::size_t used() const noexcept;
        std::size_t live() const noexcept;

    private:
        friend class arena_shard_t;
        friend class arena_bucket;

        detail::region *region_for(std::size_t bytes);
        arena_shard_t build_shard(std::uint64_t tag, std::span<const object_spec> specs);

        mutable std::mutex registry_mutex_;
        arena_config config_;
        std::vector<detail::region> regions_;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> shards_;
        std::unordered_map<std::uint64_t, std::uint32_t> shard_index_;
        std::uint32_t next_shard_id_ = 0;
        std::byte *shard_base_of(std::uint32_t index) const;
    };

    inline arena_shard_t arena_t::create_shard(std::uint64_t tag, std::initializer_list<object_spec> specs)
    {
        std::vector<object_spec> copy(specs);
        return create_shard(tag, std::span<const object_spec>(copy.data(), copy.size()));
    }

    inline arena_shard_t arena_t::create_shard(std::uint64_t tag, std::span<const object_spec> specs)
    {
        std::lock_guard lock{registry_mutex_};
        auto it = shard_index_.find(tag);
        if (it != shard_index_.end())
        {
            arena_shard_t view;
            view.arena_ = this;
            view.shard_id_ = shards_[it->second].second;
            view.hdr_ = reinterpret_cast<detail::shard_header *>(shard_base_of(it->second));
            return view;
        }
        return build_shard(tag, specs);
    }

    inline arena_shard_t arena_t::from(std::uint64_t tag) const
    {
        std::lock_guard lock{registry_mutex_};
        auto it = shard_index_.find(tag);
        if (it == shard_index_.end())
            return arena_shard_t{};
        arena_shard_t view;
        view.arena_ = const_cast<arena_t *>(this);
        view.shard_id_ = shards_[it->second].second;
        view.hdr_ = reinterpret_cast<detail::shard_header *>(shard_base_of(it->second));
        return view;
    }

    inline detail::region *arena_t::region_for(std::size_t bytes)
    {
        for (auto &r : regions_)
        {
            if (r.can_fit(bytes))
                return &r;
        }
        detail::region r;
        const auto grow = detail::align_up(bytes > config_.size ? bytes : config_.size, detail::kPageSize);
        if (!r.reserve(grow, config_.touch_pages))
            return nullptr;
        regions_.push_back(std::move(r));
        return &regions_.back();
    }

    inline std::byte *arena_t::shard_base_of(std::uint32_t index) const
    {
        const auto &entry = shards_[index];
        return regions_[entry.first].base() + entry.second;
    }

    inline arena_shard_t arena_t::build_shard(std::uint64_t tag, std::span<const object_spec> specs)
    {
        arena_shard_t view;
        if (specs.empty())
            return view;
        for (const auto &s : specs)
        {
            if (!detail::spec_valid(s))
                return view;
        }
        const auto bytes = detail::slice_bytes_runtime(specs);
        auto *region = region_for(bytes);
        if (region == nullptr)
            return view;
        auto *slice = region->take(bytes);
        if (slice == nullptr)
            return view;

        const auto shard_index = static_cast<std::uint32_t>(shards_.size());
        const auto region_index = static_cast<std::uint32_t>(region - regions_.data());
        const auto slice_offset = static_cast<std::size_t>(slice - region->base());

        auto *hdr = ::new (static_cast<void *>(slice)) detail::shard_header{};
        hdr->tag = tag;
        hdr->shard_id = next_shard_id_;
        hdr->bucket_count = static_cast<std::uint32_t>(specs.size());
        hdr->slice_bytes = bytes;

        auto *descs = reinterpret_cast<detail::bucket_descriptor *>(hdr + 1);
        std::size_t offset = detail::align_up(reinterpret_cast<std::byte *>(descs) - slice, 8)
                             + specs.size() * sizeof(detail::bucket_descriptor);

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            auto &d = descs[i];
            d.tag = specs[i].tag;
            d.object_size = specs[i].object_size;
            d.align = specs[i].align;
            d.stride = detail::stride_needed(specs[i].object_size, specs[i].align);
            d.count = static_cast<std::uint32_t>(specs[i].count);
            d.head = -1;
            d.top = 0;
            d.model = static_cast<std::uint32_t>(concurrency_model::single_thread);
            d.shm = 0;
            d.p = 0;
            d.c = 0;
            d.ring_off = 0;
            d.seq_off = 0;
            d.in_use = 0;
            d.freed = 0;
        }

        offset = detail::align_up(offset, alignof(std::shared_mutex));
        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            descs[i].shm = offset + i * sizeof(std::shared_mutex);
            ::new (static_cast<void *>(slice + descs[i].shm)) std::shared_mutex;
        }
        offset += specs.size() * sizeof(std::shared_mutex);

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            descs[i].ring_off = offset;
            offset += specs[i].count * sizeof(std::int64_t);
            descs[i].seq_off = offset;
            offset += specs[i].count * sizeof(std::int64_t);
        }

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            offset = detail::align_up(offset, specs[i].align);
            descs[i].run = static_cast<std::uint32_t>(offset);
            offset += static_cast<std::size_t>(descs[i].count) * descs[i].stride;
        }

        shards_.emplace_back(region_index, slice_offset);
        shard_index_.emplace(tag, shard_index);
        ++next_shard_id_;

        view.arena_ = this;
        view.shard_id_ = hdr->shard_id;
        view.hdr_ = hdr;
        return view;
    }

    inline arena_bucket arena_shard_t::create_writer(std::uint64_t tag, concurrency_model model)
    {
        return create_reader(tag, model);
    }

    inline arena_bucket arena_shard_t::create_reader(std::uint64_t tag, concurrency_model model)
    {
        if (!hdr_)
            return arena_bucket{};
        const auto count = static_cast<std::uint32_t>(hdr_->bucket_count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            auto &d = buckets()[i];
            if (d.tag != tag)
                continue;
            detail::adopt_bucket(bytes(), d, model);
            return arena_bucket(*this, &d);
        }
        return arena_bucket{};
    }

    inline void arena_t::deallocate_bytes(void *p) noexcept
    {
        if (p == nullptr)
            return;
        auto *payload = static_cast<std::byte *>(p);
        std::lock_guard lock{registry_mutex_};
        detail::region *owner = nullptr;
        for (auto &r : regions_)
        {
            if (payload >= r.base() && payload < r.base() + r.bytes())
            {
                owner = &r;
                break;
            }
        }
        if (owner == nullptr)
            return;
        auto *b = detail::block_of_payload(payload);
        if (b < owner->base() || b >= owner->base() + owner->bytes())
            return;
        detail::block_header hdr;
        __builtin_memcpy(&hdr, b, sizeof(hdr));
        if (hdr.shard >= shards_.size())
            return;
        auto &entry = shards_[hdr.shard];
        if (entry.first >= regions_.size() || owner != &regions_[entry.first])
            return;
        auto *slice = owner->base() + entry.second;
        auto *shard_hdr = reinterpret_cast<detail::shard_header *>(slice);
        if (hdr.bucket >= shard_hdr->bucket_count)
            return;
        auto &db = reinterpret_cast<detail::bucket_descriptor *>(shard_hdr + 1)[hdr.bucket];
        const auto index = (b - detail::run_base(slice, db)) / db.stride;
        detail::note_free(db);
        detail::free_block(slice, db, index);
    }

    inline void arena_shard_t::deallocate_bytes(void *p) noexcept
    {
        if (hdr_ == nullptr || p == nullptr)
            return;
        auto *payload = static_cast<std::byte *>(p);
        if (payload < bytes() || payload >= slice_end())
        {
            arena_->deallocate_bytes(p);
            return;
        }
        auto *b = detail::block_of_payload(payload);
        if (b < bytes() || b >= slice_end())
            return;
        detail::block_header hdr;
        __builtin_memcpy(&hdr, b, sizeof(hdr));
        if (hdr.shard != hdr_->shard_id)
        {
            arena_->deallocate_bytes(p);
            return;
        }
        if (hdr.bucket >= hdr_->bucket_count)
            return;
        auto &d = buckets()[hdr.bucket];
        const auto index = (b - detail::run_base(bytes(), d)) / d.stride;
        detail::note_free(d);
        detail::free_block(bytes(), d, index);
    }

    inline bucket_stats arena_shard_t::stats(std::uint32_t index) const noexcept
    {
        if (!hdr_ || index >= hdr_->bucket_count)
            return bucket_stats{};
        const auto &d = buckets()[index];
        bucket_stats out;
        out.tag = d.tag;
        out.object_size = d.object_size;
        out.align = d.align;
        out.capacity = d.count;
        if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread)
        {
            out.in_use = d.in_use;
            out.freed = d.freed;
        }
        else
        {
            out.in_use = std::atomic_ref<const std::int64_t>(d.in_use).load(std::memory_order_relaxed);
            out.freed = std::atomic_ref<const std::int64_t>(d.freed).load(std::memory_order_relaxed);
        }
        return out;
    }

    inline std::size_t arena_t::shard_count() const noexcept
    {
        std::lock_guard lock{registry_mutex_};
        return shards_.size();
    }

    inline std::size_t arena_t::capacity() const noexcept
    {
        std::size_t total = 0;
        for (const auto &r : regions_)
            total += r.bytes();
        return total;
    }

    inline std::size_t arena_t::used() const noexcept
    {
        std::size_t total = 0;
        for (const auto &r : regions_)
            total += r.free_top();
        return total;
    }

    inline std::size_t arena_t::live() const noexcept
    {
        std::lock_guard lock{registry_mutex_};
        std::size_t total = 0;
        for (const auto &[region_index, slice_offset] : shards_)
        {
            auto *slice = regions_[region_index].base() + slice_offset;
            auto *shard_hdr = reinterpret_cast<detail::shard_header *>(slice);
            auto *descs = reinterpret_cast<detail::bucket_descriptor *>(shard_hdr + 1);
            for (std::uint32_t i = 0; i < shard_hdr->bucket_count; ++i)
            {
                auto &d = descs[i];
                if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread)
                    total += static_cast<std::size_t>(d.in_use);
                else
                    total += static_cast<std::size_t>(std::atomic_ref<const std::int64_t>(d.in_use).load(std::memory_order_relaxed));
            }
        }
        return total;
    }

    template <std::size_t N>
    inline constexpr std::size_t shard_footprint(const object_spec (&specs)[N])
    {
        return detail::slice_bytes_c(specs);
    }

    inline constexpr std::size_t round_to_page(std::size_t bytes)
    {
        return (bytes + detail::kPageSize - 1) & ~(detail::kPageSize - 1);
    }

    inline constexpr bool valid_footprint(std::size_t bytes)
    {
        return bytes > 0;
    }
} // namespace frameworkcpp::arena