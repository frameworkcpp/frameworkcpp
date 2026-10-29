// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

#include "frameworkcpp/concurrency.hpp"

namespace frameworkcpp::memory {

// The memory and the reactor share one concurrency vocabulary. Keeping the
// alias here preserves the public `memory::concurrency_model_t` spelling.
using concurrency_model_t = frameworkcpp::concurrency_model_t;

enum class overflow_mode_t : std::uint8_t {
  throw_error_ = 0,
  abort_ = 1,
};

using memory_warning_fn = void (*)(const char *message);

class memory_overflow_exception : public std::bad_alloc {
  char message_[160]{};

  public:
  memory_overflow_exception(std::size_t requested, std::size_t hard_limit) {
    std::snprintf(message_,
                  sizeof(message_),
                  "memory overflow: %zu requested, hard_limit=%zu",
                  requested,
                  hard_limit);
  }

  [[nodiscard]] const char *what() const noexcept override { return message_; }
}; // class memory_overflow_exception

struct object_spec_t {
  std::uint64_t tag_ = 0;
  std::size_t object_size_ = 0;
  std::size_t align_ = alignof(std::max_align_t);
  std::size_t count_ = 0;
}; // struct object_spec_t

// Bounded registries keep memory_t heap-free: shards and regions live in fixed
// arrays sized at compile time. These constants are the hard caps.
inline constexpr std::size_t max_memory_shards_k = 256;
inline constexpr std::size_t max_memory_regions_k = 16;

struct memory_config_t {
  std::size_t size_ = 1024 * 1024;
  std::size_t soft_limit_ = 0; // warning watermark; 0 disables it
  std::size_t hard_limit_ = 0; // total reserved budget; 0 = unbounded
  overflow_mode_t overflow_ = overflow_mode_t::throw_error_;
  memory_warning_fn on_warning_ = nullptr;
  bool touch_pages_ = false;
  std::size_t max_shards_ = max_memory_shards_k;   // shard/tag registry capacity
  std::size_t max_regions_ = max_memory_regions_k; // mmap region capacity
}; // struct memory_config_t

struct bucket_stats_t {
  std::uint64_t tag_;
  std::size_t object_size_;
  std::size_t align_;
  std::uint32_t capacity_;
  std::int64_t in_use_;
  std::int64_t freed_;
}; // struct bucket_stats_t
} // namespace frameworkcpp::memory
