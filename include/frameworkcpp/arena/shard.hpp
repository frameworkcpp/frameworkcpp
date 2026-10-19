// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "frameworkcpp/arena/bucket.hpp"
#include "frameworkcpp/arena/detail/layout.hpp"

namespace frameworkcpp::arena {
class arena_shard_t {
  detail::shard_header_t *hdr_ = nullptr;

  public:
  arena_shard_t() = default;

  explicit operator bool() const noexcept { return hdr_ != nullptr; }

  template <typename T> T *allocate(std::size_t count = 1) {
    return reinterpret_cast<T *>(allocate_bytes(count * sizeof(T), alignof(T)));
  }

  template <typename T> void deallocate(T *pointer) noexcept { deallocate_bytes(pointer); }

  std::byte *allocate_bytes(std::size_t bytes, std::size_t alignment) {
    if (hdr_ == nullptr)
      return nullptr;
    const auto _count_k = hdr_->bucket_count_;
    // Buckets are ordered by ascending object_size (see build_shard), so the
    // first bucket that fits both size and alignment is the smallest fit.
    for (std::uint32_t _index = 0; _index < _count_k; ++_index) {
      const auto &_descriptor = get_buckets()[_index];
      if (_descriptor.object_size_ >= bytes && _descriptor.align_ >= alignment)
        return take_block(_index);
    }
    return nullptr;
  }

  std::byte *allocate_tagged(std::uint64_t tag, std::size_t bytes, std::size_t alignment) {
    if (hdr_ == nullptr)
      return nullptr;
    const auto _count_k = hdr_->bucket_count_;
    for (std::uint32_t _index = 0; _index < _count_k; ++_index) {
      auto &_descriptor = get_buckets()[_index];
      if (_descriptor.tag_ == tag && _descriptor.object_size_ >= bytes &&
          _descriptor.align_ >= alignment)
        return take_block(_index);
    }
    return nullptr;
  }

  void deallocate_bytes(void *pointer) noexcept {
    if (hdr_ == nullptr || pointer == nullptr)
      return;
    auto *_payload = static_cast<std::byte *>(pointer);
    if (_payload < get_bytes() || _payload >= get_slice_end())
      return;
    auto *_block = detail::block_of_payload(get_bytes(), _payload);
    if (_block < get_bytes() || _block >= get_slice_end())
      return;
    detail::block_header_t _header{};
    __builtin_memcpy(&_header, _block, sizeof(_header));
    if (_header.shard_ != hdr_->shard_id_ || _header.bucket_ >= hdr_->bucket_count_)
      return;
    auto &_descriptor = get_buckets()[_header.bucket_];
    const auto _index_k = static_cast<std::int64_t>(
        (_block - detail::run_base(get_bytes(), _descriptor)) / _descriptor.stride_);
    if (!detail::is_valid_block_index(_descriptor, _index_k))
      return;
    detail::note_free(_descriptor);
    detail::free_block(get_bytes(), _descriptor, _index_k);
  }

  arena_bucket_t get_bucket(std::uint64_t tag, concurrency_model_t model) {
    if (hdr_ == nullptr)
      return {};
    const auto _count_k = hdr_->bucket_count_;
    for (std::uint32_t _index = 0; _index < _count_k; ++_index) {
      auto &_descriptor = get_buckets()[_index];
      if (_descriptor.tag_ != tag)
        continue;
      detail::adopt_bucket(_descriptor, model);
      return {get_bytes(), hdr_->shard_id_, _index, &_descriptor};
    }
    return {};
  }

  [[nodiscard]] std::uint32_t get_bucket_count() const noexcept {
    return (hdr_ != nullptr) ? hdr_->bucket_count_ : 0;
  }

  [[nodiscard]] bucket_stats_t get_stats(std::uint32_t index) const noexcept {
    if (hdr_ == nullptr || index >= hdr_->bucket_count_)
      return bucket_stats_t{};
    const auto &_descriptor = get_buckets()[index];
    bucket_stats_t _result{};
    _result.tag_ = _descriptor.tag_;
    _result.object_size_ = _descriptor.object_size_;
    _result.align_ = _descriptor.align_;
    _result.capacity_ = _descriptor.count_;
    if (static_cast<concurrency_model_t>(_descriptor.model_) ==
        concurrency_model_t::single_thread_) {
      _result.in_use_ = _descriptor.in_use_;
      _result.freed_ = _descriptor.freed_;
    } else {
      _result.in_use_ =
          std::atomic_ref<const std::int64_t>(_descriptor.in_use_).load(std::memory_order_relaxed);
      _result.freed_ =
          std::atomic_ref<const std::int64_t>(_descriptor.freed_).load(std::memory_order_relaxed);
    }
    return _result;
  }

  [[nodiscard]] std::uint64_t get_tag() const noexcept {
    return (hdr_ != nullptr) ? hdr_->tag_ : 0;
  }

  [[nodiscard]] std::uint32_t get_shard_id() const noexcept {
    return (hdr_ != nullptr) ? hdr_->shard_id_ : 0;
  }

  private:
  friend class arena_t;

  [[nodiscard]] std::byte *get_bytes() const noexcept {
    return reinterpret_cast<std::byte *>(hdr_);
  }
  [[nodiscard]] std::byte *get_slice_end() const noexcept {
    return get_bytes() + hdr_->slice_bytes_;
  }

  [[nodiscard]] detail::bucket_descriptor_t *get_buckets() noexcept {
    return reinterpret_cast<detail::bucket_descriptor_t *>(hdr_ + 1);
  }

  [[nodiscard]] const detail::bucket_descriptor_t *get_buckets() const noexcept {
    return reinterpret_cast<const detail::bucket_descriptor_t *>(hdr_ + 1);
  }

  std::byte *take_block(std::uint32_t bucket) {
    auto *_descriptor = &get_buckets()[bucket];
    return detail::take_block(get_bytes(), *_descriptor, bucket, hdr_->shard_id_);
  }
}; // class arena_shard_t

template <std::size_t N> constexpr std::size_t shard_footprint(const object_spec_t (&specs)[N]) {
  return detail::slice_bytes_c(specs);
}

constexpr std::size_t round_to_page(std::size_t bytes) {
  return (bytes + detail::page_size_k - 1) & ~(detail::page_size_k - 1);
}

constexpr bool is_valid_footprint(std::size_t bytes) {
  return bytes > 0;
}
} // namespace frameworkcpp::arena
