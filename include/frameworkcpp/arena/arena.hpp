// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Umbrella header: pulls in the arena primitives and defines arena_t, the
// owner of the reserved memory and the shard carver. Grab just the pieces you
// need with <frameworkcpp/arena/shard.hpp>, <frameworkcpp/arena/bucket.hpp>,
// <frameworkcpp/arena/unique_ptr.hpp>, <frameworkcpp/arena/shared_ptr.hpp> or
// <frameworkcpp/arena/allocator.hpp>.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frameworkcpp/arena/arena_allocator.hpp"
#include "frameworkcpp/arena/bucket.hpp"
#include "frameworkcpp/arena/config.hpp"
#include "frameworkcpp/arena/detail/layout.hpp"
#include "frameworkcpp/arena/detail/region.hpp"
#include "frameworkcpp/arena/shard.hpp"
#include "frameworkcpp/arena/shared_ptr.hpp"
#include "frameworkcpp/arena/unique_ptr.hpp"

namespace frameworkcpp::arena {
class arena_t {
  public:
    arena_t() = default;

    explicit arena_t(const arena_config_t &config) : config_(config) {
        if (config_.hard_limit != 0 && config_.soft_limit > config_.hard_limit)
            throw std::invalid_argument("arena soft_limit must not exceed hard_limit");
    }

    arena_shard_t create_shard(std::uint64_t tag, std::initializer_list<object_spec_t> specs);
    arena_shard_t create_shard(std::uint64_t tag, std::span<const object_spec_t> specs);
    arena_shard_t from(std::uint64_t tag) const;

    void deallocate_bytes(void *p) noexcept;
    void deallocate(void *p) noexcept { deallocate_bytes(p); }

    std::size_t shard_count() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t used() const noexcept;
    std::size_t live() const noexcept;

    std::size_t soft_limit() const noexcept { return config_.soft_limit; }
    std::size_t hard_limit() const noexcept { return config_.hard_limit; }
    bool soft_limit_exceeded() const noexcept {
        return soft_exceeded_.load(std::memory_order_relaxed);
    }

  private:
    friend class arena_shard_t;

    detail::region *region_for(std::size_t bytes);
    arena_shard_t build_shard(std::uint64_t tag, std::span<const object_spec_t> specs);
    [[noreturn]] void hard_overflow(std::size_t requested) const;
    void check_soft_limit() noexcept;
    std::byte *shard_base_of(std::uint32_t index) const;

    mutable std::mutex registry_mutex_;
    arena_config_t config_;
    std::vector<detail::region> regions_;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> shards_;
    std::unordered_map<std::uint64_t, std::uint32_t> shard_index_;
    std::uint32_t next_shard_id_ = 0;
    std::atomic<bool> soft_exceeded_{false};
};

inline arena_shard_t arena_t::create_shard(std::uint64_t tag,
                                           std::initializer_list<object_spec_t> specs) {
    std::vector<object_spec_t> copy(specs);
    return create_shard(tag, std::span<const object_spec_t>(copy.data(), copy.size()));
}

inline arena_shard_t arena_t::create_shard(std::uint64_t tag,
                                           std::span<const object_spec_t> specs) {
    const std::lock_guard lock{registry_mutex_};
    auto it = shard_index_.find(tag);
    if (it != shard_index_.end()) {
        arena_shard_t view;
        view.hdr_ = reinterpret_cast<detail::shard_header *>(shard_base_of(it->second));
        return view;
    }
    return build_shard(tag, specs);
}

inline arena_shard_t arena_t::from(std::uint64_t tag) const {
    const std::lock_guard lock{registry_mutex_};
    auto it = shard_index_.find(tag);
    if (it == shard_index_.end())
        return arena_shard_t{};
    arena_shard_t view;
    view.hdr_ = reinterpret_cast<detail::shard_header *>(shard_base_of(it->second));
    return view;
}

inline detail::region *arena_t::region_for(std::size_t bytes) {
    for (auto &r : regions_) {
        if (r.can_fit(bytes))
            return &r;
    }
    auto budget = (bytes > config_.size ? bytes : config_.size);
    if (config_.hard_limit != 0) {
        if (bytes > config_.hard_limit)
            hard_overflow(bytes);
        const auto reserved = capacity();
        if (reserved + budget > config_.hard_limit)
            budget = config_.hard_limit - reserved;
    }
    budget = detail::align_up(budget, detail::kPageSize);
    detail::region r;
    if (!r.reserve(budget, config_.touch_pages))
        return nullptr;
    regions_.push_back(std::move(r));
    check_soft_limit();
    return &regions_.back();
}

[[noreturn]] inline void arena_t::hard_overflow(std::size_t requested) const {
    if (config_.overflow == overflow_mode::abort)
        std::abort();
    throw arena_overflow_error(requested, config_.hard_limit);
}

inline void arena_t::check_soft_limit() noexcept {
    if (config_.soft_limit == 0)
        return;
    const auto reserved = capacity();
    if (reserved >= config_.soft_limit &&
        !soft_exceeded_.exchange(true, std::memory_order_relaxed)) {
        if (config_.on_warning != nullptr)
            config_.on_warning("arena soft limit reached");
    }
}

inline std::byte *arena_t::shard_base_of(std::uint32_t index) const {
    const auto &entry = shards_[index];
    return regions_[entry.first].base() + entry.second;
}

inline arena_shard_t arena_t::build_shard(std::uint64_t tag, std::span<const object_spec_t> specs) {
    arena_shard_t view;
    if (specs.empty())
        return view;
    for (const auto &s : specs) {
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
    std::size_t offset = detail::align_up(reinterpret_cast<std::byte *>(descs) - slice, 8) +
                         (specs.size() * sizeof(detail::bucket_descriptor));

    for (std::size_t i = 0; i < specs.size(); ++i) {
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
    for (std::size_t i = 0; i < specs.size(); ++i) {
        descs[i].shm = offset + i * sizeof(std::shared_mutex);
        ::new (static_cast<void *>(slice + descs[i].shm)) std::shared_mutex;
    }
    offset += specs.size() * sizeof(std::shared_mutex);

    for (std::size_t i = 0; i < specs.size(); ++i) {
        descs[i].ring_off = offset;
        offset += specs[i].count * sizeof(std::int64_t);
        descs[i].seq_off = offset;
        offset += specs[i].count * sizeof(std::int64_t);
    }

    for (std::size_t i = 0; i < specs.size(); ++i) {
        offset = detail::align_up(offset, specs[i].align);
        descs[i].run = static_cast<std::uint32_t>(offset);
        offset += static_cast<std::size_t>(descs[i].count) * descs[i].stride;
    }

    shards_.emplace_back(region_index, slice_offset);
    shard_index_.emplace(tag, shard_index);
    ++next_shard_id_;

    view.hdr_ = hdr;
    return view;
}

inline void arena_t::deallocate_bytes(void *p) noexcept {
    if (p == nullptr)
        return;
    auto *payload = static_cast<std::byte *>(p);
    const std::lock_guard lock{registry_mutex_};
    detail::region *owner = nullptr;
    for (auto &r : regions_) {
        if (payload >= r.base() && payload < r.base() + r.bytes()) {
            owner = &r;
            break;
        }
    }
    if (owner == nullptr)
        return;
    auto *b = detail::block_of_payload(payload);
    if (b < owner->base() || b >= owner->base() + owner->bytes())
        return;
    detail::block_header hdr{};
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
    const auto index = static_cast<std::int64_t>((b - detail::run_base(slice, db)) / db.stride);
    detail::note_free(db);
    detail::free_block(slice, db, index);
}

inline std::size_t arena_t::shard_count() const noexcept {
    const std::lock_guard lock{registry_mutex_};
    return shards_.size();
}

inline std::size_t arena_t::capacity() const noexcept {
    std::size_t total = 0;
    for (const auto &r : regions_)
        total += r.bytes();
    return total;
}

inline std::size_t arena_t::used() const noexcept {
    std::size_t total = 0;
    for (const auto &r : regions_)
        total += r.free_top();
    return total;
}

inline std::size_t arena_t::live() const noexcept {
    const std::lock_guard lock{registry_mutex_};
    std::size_t total = 0;
    for (const auto &[region_index, slice_offset] : shards_) {
        auto *slice = regions_[region_index].base() + slice_offset;
        auto *shard_hdr = reinterpret_cast<detail::shard_header *>(slice);
        auto *descs = reinterpret_cast<detail::bucket_descriptor *>(shard_hdr + 1);
        for (std::uint32_t i = 0; i < shard_hdr->bucket_count; ++i) {
            auto &d = descs[i];
            if (static_cast<concurrency_model>(d.model) == concurrency_model::single_thread)
                total += static_cast<std::size_t>(d.in_use);
            else
                total += static_cast<std::size_t>(
                    std::atomic_ref<const std::int64_t>(d.in_use).load(std::memory_order_relaxed));
        }
    }
    return total;
}
} // namespace frameworkcpp::arena