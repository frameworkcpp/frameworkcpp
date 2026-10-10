// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "frameworkcpp/arena/detail/layout.hpp"

namespace frameworkcpp::arena {
class arena_bucket_t {
  public:
    arena_bucket_t() = default;

    explicit operator bool() const noexcept { return desc_ != nullptr; }

    template <typename T> T *allocate(std::size_t count = 1) {
        if (desc_ == nullptr || count * sizeof(T) > desc_->object_size || alignof(T) > desc_->align)
            return nullptr;
        return reinterpret_cast<T *>(detail::take_block(slice_, *desc_, bucket_index_, shard_id_));
    }

    template <typename T> void deallocate(T *p) noexcept { deallocate_bytes(p); }

    void deallocate_bytes(void *p) noexcept {
        if (desc_ == nullptr || p == nullptr)
            return;
        auto *b = detail::block_of_payload(static_cast<std::byte *>(p));
        const auto index =
            static_cast<std::int64_t>((b - detail::run_base(slice_, *desc_)) / desc_->stride);
        detail::note_free(*desc_);
        detail::free_block(slice_, *desc_, index);
    }

    [[nodiscard]] std::uint64_t tag() const noexcept { return (desc_ != nullptr) ? desc_->tag : 0; }

  private:
    friend class arena_shard_t;
    arena_bucket_t(std::byte *slice,
                   std::uint32_t shard_id,
                   std::uint32_t bucket_index,
                   detail::bucket_descriptor *desc)
        : slice_(slice), shard_id_(shard_id), bucket_index_(bucket_index), desc_(desc) {}

    std::byte *slice_ = nullptr;
    std::uint32_t shard_id_ = 0;
    std::uint32_t bucket_index_ = 0;
    detail::bucket_descriptor *desc_ = nullptr;
};
} // namespace frameworkcpp::arena