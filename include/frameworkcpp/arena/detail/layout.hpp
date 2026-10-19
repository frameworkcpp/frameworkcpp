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
inline constexpr std::size_t block_header_size_k = 8;
inline constexpr std::size_t next_slot_offset_k = 8;
inline constexpr std::size_t layout_bytes_k = 24;
inline constexpr std::size_t slab_align_k = 64;
inline constexpr std::size_t max_bucket_align_k = 64;
inline constexpr std::size_t page_size_k = 4096;

struct block_header_t {
  std::uint32_t bucket_;
  std::uint32_t shard_;
}; // struct block_header_t

struct shard_header_t {
  std::uint32_t shard_id_;
  std::uint32_t bucket_count_;
  std::uint64_t tag_;
  std::size_t slice_bytes_;
}; // struct shard_header_t

struct bucket_descriptor_t {
  std::uint64_t tag_;
  std::size_t object_size_;
  std::size_t align_;
  std::size_t stride_;
  std::uint32_t count_;
  std::uint32_t run_;
  std::int64_t head_;
  std::int64_t top_;
  std::uint32_t model_;
  std::size_t shm_;
  std::int64_t p_;
  std::int64_t c_;
  std::size_t ring_off_;
  std::size_t seq_off_;
  std::int64_t in_use_;
  std::int64_t freed_;
}; // struct bucket_descriptor_t

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

constexpr std::size_t align_up_c(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

inline bool is_pow2(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

inline bool is_spec_valid(const object_spec_t &spec) {
  return spec.object_size_ != 0 && spec.count_ != 0 && is_pow2(spec.align_) &&
         spec.align_ <= max_bucket_align_k;
}

inline std::size_t head_spacing(std::size_t alignment) {
  return align_up(layout_bytes_k, alignment);
}

inline std::size_t stride_needed(std::size_t object_size, std::size_t alignment) {
  return align_up(head_spacing(alignment) + object_size, alignment);
}

constexpr std::size_t head_spacing_c(std::size_t alignment) {
  return align_up_c(layout_bytes_k, alignment);
}

constexpr std::size_t stride_needed_c(std::size_t object_size, std::size_t alignment) {
  return align_up_c(head_spacing_c(alignment) + object_size, alignment);
}

inline std::size_t slice_bytes_runtime(std::span<const object_spec_t> specs) {
  std::size_t _offset = align_up(sizeof(shard_header_t), 8);
  _offset += specs.size() * sizeof(bucket_descriptor_t);
  _offset = align_up(_offset, alignof(std::shared_mutex));
  _offset += specs.size() * sizeof(std::shared_mutex);
  for (const auto &_spec : specs) {
    _offset += _spec.count_ * sizeof(std::int64_t);
    _offset += _spec.count_ * sizeof(std::int64_t);
  }
  for (const auto &_spec : specs)
    _offset += (_spec.count_ * stride_needed(_spec.object_size_, _spec.align_));
  return align_up(_offset, slab_align_k);
}

template <std::size_t N> constexpr std::size_t slice_bytes_c(const object_spec_t (&specs)[N]) {
  std::size_t _offset = align_up_c(sizeof(shard_header_t), 8);
  _offset += N * sizeof(bucket_descriptor_t);
  _offset = align_up_c(_offset, alignof(std::shared_mutex));
  _offset += N * sizeof(std::shared_mutex);
  for (std::size_t _index = 0; _index < N; ++_index) {
    _offset += specs[_index].count_ * sizeof(std::int64_t);
    _offset += specs[_index].count_ * sizeof(std::int64_t);
  }
  for (std::size_t _index = 0; _index < N; ++_index)
    _offset +=
        (specs[_index].count_ * stride_needed_c(specs[_index].object_size_, specs[_index].align_));
  return align_up_c(_offset, slab_align_k);
}

inline std::byte *run_base(std::byte *base, const bucket_descriptor_t &descriptor) {
  return base + descriptor.run_;
}

inline std::byte *
block_base(std::byte *base, const bucket_descriptor_t &descriptor, std::int64_t index) {
  return run_base(base, descriptor) + (index * descriptor.stride_);
}

inline std::byte *next_slot(std::byte *block) {
  return block + next_slot_offset_k;
}

inline std::int64_t block_next(std::byte *block) {
  std::int64_t _value = 0;
  __builtin_memcpy(&_value, next_slot(block), sizeof(_value));
  return _value;
}

inline void store_block_next(std::byte *block, std::int64_t next) {
  __builtin_memcpy(next_slot(block), &next, sizeof(next));
}

inline std::byte *payload_of(std::byte *block, const bucket_descriptor_t &descriptor) {
  return block + head_spacing(descriptor.align_);
}

// The anchor lives in the 8 bytes just before the payload and holds the block's
// offset from the base of its slice. Storing an offset (never a pointer) keeps
// the scheme correct on any pointer width and lets the caller bound-check it.
inline std::byte *block_of_payload(std::byte *slice, std::byte *payload) {
  if (payload == nullptr)
    return nullptr;
  std::uint64_t _offset = 0;
  __builtin_memcpy(&_offset, payload - 8, sizeof(_offset));
  return slice + _offset;
}

inline void store_anchor(std::byte *slice, std::byte *payload, std::byte *block) {
  const auto _offset_k = static_cast<std::uint64_t>(block - slice);
  __builtin_memcpy(payload - 8, &_offset_k, sizeof(_offset_k));
}

inline bool is_valid_block_index(const bucket_descriptor_t &descriptor, std::int64_t index) {
  return index >= 0 && index < static_cast<std::int64_t>(descriptor.count_);
}

inline std::shared_mutex &shard_mutex(std::byte *base, const bucket_descriptor_t &descriptor) {
  return *reinterpret_cast<std::shared_mutex *>(base + descriptor.shm_);
}

inline std::int64_t *
ring_slot(std::byte *base, const bucket_descriptor_t &descriptor, std::int64_t index) {
  return reinterpret_cast<std::int64_t *>(base + descriptor.ring_off_) + index;
}

inline std::int64_t *
seq_slot(std::byte *base, const bucket_descriptor_t &descriptor, std::int64_t index) {
  return reinterpret_cast<std::int64_t *>(base + descriptor.seq_off_) + index;
}

inline std::int64_t ring_index(std::int64_t value, std::int64_t size) {
  return value % size;
}

inline std::int64_t fresh_top(bucket_descriptor_t &descriptor) {
  if (static_cast<concurrency_model_t>(descriptor.model_) == concurrency_model_t::single_thread_) {
    if (descriptor.top_ < static_cast<std::int64_t>(descriptor.count_))
      return descriptor.top_++;
    return -1;
  }
  const std::atomic_ref<std::int64_t> _top_k{descriptor.top_};
  for (;;) {
    auto _value = _top_k.load(std::memory_order_relaxed);
    if (_value >= static_cast<std::int64_t>(descriptor.count_))
      return -1;
    if (_top_k.compare_exchange_weak(
            _value, _value + 1, std::memory_order_relaxed, std::memory_order_relaxed))
      return _value;
  }
}

inline std::int64_t reuse_top(std::byte *base, bucket_descriptor_t &descriptor) {
  if (descriptor.head_ >= 0) {
    const auto _index_k = descriptor.head_;
    descriptor.head_ = block_next(block_base(base, descriptor, _index_k));
    return _index_k;
  }
  return -1;
}

inline void reuse_push(std::byte *base, bucket_descriptor_t &descriptor, std::int64_t index) {
  store_block_next(block_base(base, descriptor, index), descriptor.head_);
  descriptor.head_ = index;
}

inline std::int64_t ring_pop(std::byte *base, bucket_descriptor_t &descriptor) {
  const std::int64_t _wrap_k = descriptor.count_;
  const std::atomic_ref<std::int64_t> _consumer_k{descriptor.c_};
  const std::atomic_ref<std::int64_t> _producer_k{descriptor.p_};
  for (;;) {
    auto _current = _consumer_k.load(std::memory_order_relaxed);
    if (_current == _producer_k.load(std::memory_order_acquire))
      return -1;
    const auto _slot_k = ring_index(_current, _wrap_k);
    if (std::atomic_ref<std::int64_t>(*seq_slot(base, descriptor, _slot_k))
            .load(std::memory_order_acquire) != _current + 1) {
      std::this_thread::yield();
      continue;
    }
    const auto _index_k = std::atomic_ref<std::int64_t>(*ring_slot(base, descriptor, _slot_k))
                              .load(std::memory_order_relaxed);
    if (_consumer_k.compare_exchange_weak(
            _current, _current + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
      return _index_k;
  }
}

inline bool ring_push(std::byte *base, bucket_descriptor_t &descriptor, std::int64_t index) {
  const std::int64_t _wrap_k = descriptor.count_;
  const std::atomic_ref<std::int64_t> _consumer_k{descriptor.c_};
  const std::atomic_ref<std::int64_t> _producer_k{descriptor.p_};
  for (;;) {
    auto _current = _producer_k.load(std::memory_order_relaxed);
    if (_current - _consumer_k.load(std::memory_order_acquire) >= _wrap_k) {
      std::this_thread::yield();
      continue;
    }
    if (!_producer_k.compare_exchange_weak(
            _current, _current + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
      continue;
    const auto _slot_k = ring_index(_current, _wrap_k);
    std::atomic_ref<std::int64_t>(*ring_slot(base, descriptor, _slot_k))
        .store(index, std::memory_order_relaxed);
    std::atomic_ref<std::int64_t>(*seq_slot(base, descriptor, _slot_k))
        .store(_current + 1, std::memory_order_release);
    return true;
  }
}

inline std::int64_t alloc_block(std::byte *base, bucket_descriptor_t &descriptor) {
  switch (static_cast<concurrency_model_t>(descriptor.model_)) {
  case concurrency_model_t::single_thread_: {
    const auto _index_k = reuse_top(base, descriptor);
    return _index_k >= 0 ? _index_k : fresh_top(descriptor);
  }
  case concurrency_model_t::lock_free_: {
    const auto _index_k = ring_pop(base, descriptor);
    return _index_k >= 0 ? _index_k : fresh_top(descriptor);
  }
  case concurrency_model_t::thread_safe_: {
    const std::scoped_lock _lock_k{shard_mutex(base, descriptor)};
    const auto _index_k = reuse_top(base, descriptor);
    return _index_k >= 0 ? _index_k : fresh_top(descriptor);
  }
  }
  return -1;
}

inline void note_alloc(bucket_descriptor_t &descriptor) {
  if (static_cast<concurrency_model_t>(descriptor.model_) == concurrency_model_t::single_thread_) {
    ++descriptor.in_use_;
    return;
  }
  std::atomic_ref<std::int64_t>(descriptor.in_use_).fetch_add(1, std::memory_order_relaxed);
}

inline void note_free(bucket_descriptor_t &descriptor) {
  if (static_cast<concurrency_model_t>(descriptor.model_) == concurrency_model_t::single_thread_) {
    --descriptor.in_use_;
    ++descriptor.freed_;
    return;
  }
  std::atomic_ref<std::int64_t>(descriptor.in_use_).fetch_sub(1, std::memory_order_relaxed);
  std::atomic_ref<std::int64_t>(descriptor.freed_).fetch_add(1, std::memory_order_relaxed);
}

inline std::byte *take_block(std::byte *slice,
                             bucket_descriptor_t &descriptor,
                             std::uint32_t bucket,
                             std::uint32_t shard) {
  const auto _index_k = alloc_block(slice, descriptor);
  if (_index_k < 0)
    return nullptr;
  note_alloc(descriptor);
  auto *_block = block_base(slice, descriptor, _index_k);
  block_header_t _header{};
  _header.bucket_ = bucket;
  _header.shard_ = shard;
  __builtin_memcpy(_block, &_header, sizeof(_header));
  auto *_payload = payload_of(_block, descriptor);
  store_anchor(slice, _payload, _block);
  return _payload;
}

inline void free_block(std::byte *base, bucket_descriptor_t &descriptor, std::int64_t index) {
  switch (static_cast<concurrency_model_t>(descriptor.model_)) {
  case concurrency_model_t::single_thread_:
    reuse_push(base, descriptor, index);
    return;
  case concurrency_model_t::lock_free_:
    ring_push(base, descriptor, index);
    return;
  case concurrency_model_t::thread_safe_: {
    const std::scoped_lock _lock_k{shard_mutex(base, descriptor)};
    reuse_push(base, descriptor, index);
    return;
  }
  }
}

inline void adopt_bucket(bucket_descriptor_t &descriptor, concurrency_model_t model) {
  if (descriptor.model_ == static_cast<std::uint32_t>(model))
    return;
  if (descriptor.model_ != static_cast<std::uint32_t>(concurrency_model_t::single_thread_) ||
      descriptor.head_ != -1 || descriptor.top_ != 0 || descriptor.p_ != 0 || descriptor.c_ != 0)
    std::abort();
  descriptor.model_ = static_cast<std::uint32_t>(model);
}
} // namespace frameworkcpp::arena::detail
