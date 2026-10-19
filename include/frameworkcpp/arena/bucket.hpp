// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "frameworkcpp/arena/detail/layout.hpp"

namespace frameworkcpp::arena {
class arena_bucket_t {
  std::byte *slice_ = nullptr;
  std::uint32_t shard_id_ = 0;
  std::uint32_t bucket_index_ = 0;
  detail::bucket_descriptor_t *desc_ = nullptr;

  public:
  arena_bucket_t() = default;

  explicit operator bool() const noexcept { return desc_ != nullptr; }

  template <typename T> T *allocate(std::size_t count = 1) {
    if (desc_ == nullptr || count * sizeof(T) > desc_->object_size_ || alignof(T) > desc_->align_)
      return nullptr;
    return reinterpret_cast<T *>(detail::take_block(slice_, *desc_, bucket_index_, shard_id_));
  }

  template <typename T> void deallocate(T *pointer) noexcept { deallocate_bytes(pointer); }

  void deallocate_bytes(void *pointer) noexcept {
    if (desc_ == nullptr || pointer == nullptr)
      return;
    auto *_block = detail::block_of_payload(slice_, static_cast<std::byte *>(pointer));
    const auto _index_k =
        static_cast<std::int64_t>((_block - detail::run_base(slice_, *desc_)) / desc_->stride_);
    if (!detail::is_valid_block_index(*desc_, _index_k))
      return;
    detail::note_free(*desc_);
    detail::free_block(slice_, *desc_, _index_k);
  }

  [[nodiscard]] std::uint64_t get_tag() const noexcept {
    return (desc_ != nullptr) ? desc_->tag_ : 0;
  }

  private:
  friend class arena_shard_t;
  arena_bucket_t(std::byte *slice,
                 std::uint32_t shard_id,
                 std::uint32_t bucket_index,
                 detail::bucket_descriptor_t *desc)
      : slice_(slice), shard_id_(shard_id), bucket_index_(bucket_index), desc_(desc) {}
}; // class arena_bucket_t
} // namespace frameworkcpp::arena
