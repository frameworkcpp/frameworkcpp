// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Umbrella header: pulls in the arena primitives and defines arena_t, the
// owner of the reserved memory and the shard carver. Grab just the pieces you
// need with <frameworkcpp/arena/shard.hpp>, <frameworkcpp/arena/bucket.hpp>,
// <frameworkcpp/arena/unique_ptr.hpp>, <frameworkcpp/arena/shared_ptr.hpp> or
// <frameworkcpp/arena/arena_allocator.hpp>.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <utility>

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
  friend class arena_shard_t;

  struct shard_entry_t {
    std::uint64_t tag_;
    std::uint32_t region_index_;
    std::uint32_t slice_offset_;
  }; // struct shard_entry_t

  mutable std::mutex registry_mutex_;
  arena_config_t config_;
  std::array<detail::region_t, max_arena_regions_k> regions_ = {};
  std::array<shard_entry_t, max_arena_shards_k> shards_ = {};
  std::size_t region_count_ = 0;
  std::atomic<std::size_t> shard_count_{0};
  std::atomic<std::size_t> capacity_{0};
  std::atomic<std::size_t> used_{0};
  std::atomic<bool> soft_exceeded_{false};

  public:
  arena_t() = default;

  explicit arena_t(const arena_config_t &config) : config_(config) {
    if (config_.hard_limit_ != 0 && config_.soft_limit_ > config_.hard_limit_)
      throw std::invalid_argument("arena soft_limit must not exceed hard_limit");
    if (config_.max_shards_ > max_arena_shards_k || config_.max_regions_ > max_arena_regions_k)
      throw std::invalid_argument("arena registry capacity exceeds hard caps");
  }

  arena_shard_t create_shard(std::uint64_t tag, std::initializer_list<object_spec_t> specs);
  arena_shard_t create_shard(std::uint64_t tag, std::span<const object_spec_t> specs);
  arena_shard_t get_shard(std::uint64_t tag) const;

  [[nodiscard]] std::size_t get_shard_count() const noexcept;
  [[nodiscard]] std::size_t get_capacity() const noexcept;
  [[nodiscard]] std::size_t get_used() const noexcept;
  [[nodiscard]] std::size_t get_live() const noexcept;

  [[nodiscard]] std::size_t get_soft_limit() const noexcept { return config_.soft_limit_; }
  [[nodiscard]] std::size_t get_hard_limit() const noexcept { return config_.hard_limit_; }
  [[nodiscard]] bool is_soft_limit_exceeded() const noexcept {
    return soft_exceeded_.load(std::memory_order_relaxed);
  }

  private:
  detail::region_t *region_for(std::size_t bytes);
  arena_shard_t build_shard(std::uint64_t tag, std::span<const object_spec_t> specs);
  [[noreturn]] void hard_overflow(std::size_t requested) const;
  void check_soft_limit() noexcept;
  std::byte *get_shard_base(std::uint32_t index) const;
}; // class arena_t

inline arena_shard_t arena_t::create_shard(std::uint64_t tag,
                                           std::initializer_list<object_spec_t> specs) {
  return create_shard(tag, std::span<const object_spec_t>(specs.begin(), specs.size()));
}

inline arena_shard_t arena_t::create_shard(std::uint64_t tag,
                                           std::span<const object_spec_t> specs) {
  const std::lock_guard _lock_k{registry_mutex_};
  const auto _count_k = shard_count_.load(std::memory_order_relaxed);
  for (std::size_t _index = 0; _index < _count_k; ++_index) {
    if (shards_[_index].tag_ == tag) {
      arena_shard_t _view;
      _view.hdr_ = reinterpret_cast<detail::shard_header_t *>(get_shard_base(_index));
      return _view;
    }
  }
  return build_shard(tag, specs);
}

inline arena_shard_t arena_t::get_shard(std::uint64_t tag) const {
  const auto _count_k = shard_count_.load(std::memory_order_acquire);
  for (std::size_t _index = 0; _index < _count_k; ++_index) {
    if (shards_[_index].tag_ == tag) {
      arena_shard_t _view;
      _view.hdr_ = reinterpret_cast<detail::shard_header_t *>(get_shard_base(_index));
      return _view;
    }
  }
  return arena_shard_t{};
}

inline detail::region_t *arena_t::region_for(std::size_t bytes) {
  for (std::size_t _index = 0; _index < region_count_; ++_index) {
    auto &_region = regions_[_index];
    if (_region.can_fit(bytes))
      return &_region;
  }
  if (region_count_ >= config_.max_regions_ || region_count_ >= regions_.size())
    return nullptr;
  auto _budget = std::max(bytes, config_.size_);
  if (config_.hard_limit_ != 0) {
    if (bytes > config_.hard_limit_)
      hard_overflow(bytes);
    const auto _reserved_k = get_capacity();
    if (_reserved_k + _budget > config_.hard_limit_)
      _budget = config_.hard_limit_ - _reserved_k;
  }
  auto &_region = regions_[region_count_];
  const std::size_t _page_bytes_k = detail::align_up(_budget, detail::page_size_k);
  if (!_region.reserve(_page_bytes_k, config_.touch_pages_))
    return nullptr;
  ++region_count_;
  capacity_.fetch_add(_page_bytes_k, std::memory_order_relaxed);
  check_soft_limit();
  return &_region;
}

[[noreturn]] inline void arena_t::hard_overflow(std::size_t requested) const {
  if (config_.overflow_ == overflow_mode_t::abort_)
    std::abort();
  throw arena_overflow_exception(requested, config_.hard_limit_);
}

inline void arena_t::check_soft_limit() noexcept {
  if (config_.soft_limit_ == 0)
    return;
  const auto _reserved_k = get_capacity();
  if (_reserved_k >= config_.soft_limit_ &&
      !soft_exceeded_.exchange(true, std::memory_order_relaxed)) {
    if (config_.on_warning_ != nullptr)
      config_.on_warning_("arena soft limit reached");
  }
}

inline std::byte *arena_t::get_shard_base(std::uint32_t index) const {
  const auto &_entry = shards_[index];
  return regions_[_entry.region_index_].get_base() + _entry.slice_offset_;
}

inline arena_shard_t arena_t::build_shard(std::uint64_t tag, std::span<const object_spec_t> specs) {
  arena_shard_t _view;
  if (specs.empty())
    return _view;
  for (const auto &_spec : specs) {
    if (!detail::is_spec_valid(_spec))
      return _view;
  }
  const auto _shard_count_k = shard_count_.load(std::memory_order_relaxed);
  if (_shard_count_k >= config_.max_shards_)
    return _view;
  const auto _bytes_k = detail::slice_bytes_runtime(specs);
  auto *_region = region_for(_bytes_k);
  if (_region == nullptr)
    return _view;
  auto *_slice = _region->take(_bytes_k);
  if (_slice == nullptr)
    return _view;
  used_.fetch_add(_bytes_k, std::memory_order_relaxed);

  const auto _shard_index_k = static_cast<std::uint32_t>(_shard_count_k);
  const auto _region_index_k = static_cast<std::uint32_t>(_region - regions_.data());
  const auto _slice_offset_k = static_cast<std::size_t>(_slice - _region->get_base());

  auto *_header = ::new (static_cast<void *>(_slice)) detail::shard_header_t{};
  _header->tag_ = tag;
  _header->shard_id_ = _shard_index_k;
  _header->bucket_count_ = static_cast<std::uint32_t>(specs.size());
  _header->slice_bytes_ = _bytes_k;

  auto *_descriptors = reinterpret_cast<detail::bucket_descriptor_t *>(_header + 1);
  std::size_t _offset = detail::align_up(reinterpret_cast<std::byte *>(_descriptors) - _slice, 8) +
                        (specs.size() * sizeof(detail::bucket_descriptor_t));

  for (std::size_t _index = 0; _index < specs.size(); ++_index) {
    auto &_descriptor = _descriptors[_index];
    _descriptor.tag_ = specs[_index].tag_;
    _descriptor.object_size_ = specs[_index].object_size_;
    _descriptor.align_ = specs[_index].align_;
    _descriptor.stride_ = detail::stride_needed(specs[_index].object_size_, specs[_index].align_);
    _descriptor.count_ = static_cast<std::uint32_t>(specs[_index].count_);
    _descriptor.head_ = -1;
    _descriptor.top_ = 0;
    _descriptor.model_ = static_cast<std::uint32_t>(concurrency_model_t::single_thread_);
    _descriptor.shm_ = 0;
    _descriptor.p_ = 0;
    _descriptor.c_ = 0;
    _descriptor.ring_off_ = 0;
    _descriptor.seq_off_ = 0;
    _descriptor.in_use_ = 0;
    _descriptor.freed_ = 0;
  }

  _offset = detail::align_up(_offset, alignof(std::shared_mutex));
  for (std::size_t _index = 0; _index < specs.size(); ++_index) {
    _descriptors[_index].shm_ = _offset + _index * sizeof(std::shared_mutex);
    ::new (static_cast<void *>(_slice + _descriptors[_index].shm_)) std::shared_mutex;
  }
  _offset += specs.size() * sizeof(std::shared_mutex);

  for (std::size_t _index = 0; _index < specs.size(); ++_index) {
    _descriptors[_index].ring_off_ = _offset;
    _offset += specs[_index].count_ * sizeof(std::int64_t);
    _descriptors[_index].seq_off_ = _offset;
    _offset += specs[_index].count_ * sizeof(std::int64_t);
  }

  for (std::size_t _index = 0; _index < specs.size(); ++_index) {
    _offset = detail::align_up(_offset, specs[_index].align_);
    _descriptors[_index].run_ = static_cast<std::uint32_t>(_offset);
    _offset += static_cast<std::size_t>(_descriptors[_index].count_) * _descriptors[_index].stride_;
  }

  // Order descriptors by ascending object_size so allocate_bytes can stop at
  // the first fit. Descriptors are self-contained (each carries its own run,
  // mutex, ring and seq offsets), so reordering them keeps the layout intact.
  for (std::size_t _index = 1; _index < specs.size(); ++_index) {
    const detail::bucket_descriptor_t _key_k = _descriptors[_index];
    std::size_t _position = _index;
    while (_position > 0 && _descriptors[_position - 1].object_size_ > _key_k.object_size_) {
      _descriptors[_position] = _descriptors[_position - 1];
      --_position;
    }
    _descriptors[_position] = _key_k;
  }

  shards_[_shard_index_k] = {tag,
                             static_cast<std::uint32_t>(_region_index_k),
                             static_cast<std::uint32_t>(_slice_offset_k)};
  shard_count_.store(_shard_count_k + 1, std::memory_order_release);

  _view.hdr_ = _header;
  return _view;
}

inline std::size_t arena_t::get_shard_count() const noexcept {
  return shard_count_.load(std::memory_order_acquire);
}

inline std::size_t arena_t::get_capacity() const noexcept {
  return capacity_.load(std::memory_order_acquire);
}

// High-water mark of the bytes carved out of the regions. The arena never
// returns memory to a region, so this only grows.
inline std::size_t arena_t::get_used() const noexcept {
  return used_.load(std::memory_order_acquire);
}

inline std::size_t arena_t::get_live() const noexcept {
  std::size_t _total = 0;
  const auto _count_k = shard_count_.load(std::memory_order_acquire);
  for (std::size_t _index = 0; _index < _count_k; ++_index) {
    const auto &_entry = shards_[_index];
    auto *_slice = regions_[_entry.region_index_].get_base() + _entry.slice_offset_;
    auto *_shard_header = reinterpret_cast<detail::shard_header_t *>(_slice);
    auto *_descriptors = reinterpret_cast<detail::bucket_descriptor_t *>(_shard_header + 1);
    for (std::uint32_t _bucket = 0; _bucket < _shard_header->bucket_count_; ++_bucket) {
      auto &_descriptor = _descriptors[_bucket];
      if (static_cast<concurrency_model_t>(_descriptor.model_) ==
          concurrency_model_t::single_thread_)
        _total += static_cast<std::size_t>(_descriptor.in_use_);
      else
        _total += static_cast<std::size_t>(std::atomic_ref<const std::int64_t>(_descriptor.in_use_)
                                               .load(std::memory_order_relaxed));
    }
  }
  return _total;
}
} // namespace frameworkcpp::arena
