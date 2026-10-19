// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

namespace frameworkcpp::arena {
enum class concurrency_model_t : std::uint8_t {
  single_thread_ = 0,
  lock_free_ = 1,
  thread_safe_ = 2,
};

enum class overflow_mode_t : std::uint8_t {
  throw_error_ = 0,
  abort_ = 1,
};

using arena_warning_fn = void (*)(const char *message);

class arena_overflow_exception : public std::bad_alloc {
  char message_[160]{};

  public:
  arena_overflow_exception(std::size_t requested, std::size_t hard_limit) {
    std::snprintf(message_,
                  sizeof(message_),
                  "arena overflow: %zu requested, hard_limit=%zu",
                  requested,
                  hard_limit);
  }

  [[nodiscard]] const char *what() const noexcept override { return message_; }
}; // class arena_overflow_exception

struct object_spec_t {
  std::uint64_t tag_ = 0;
  std::size_t object_size_ = 0;
  std::size_t align_ = alignof(std::max_align_t);
  std::size_t count_ = 0;
}; // struct object_spec_t

// Bounded registries keep arena_t heap-free: shards and regions live in fixed
// arrays sized at compile time. These constants are the hard caps.
inline constexpr std::size_t max_arena_shards_k = 256;
inline constexpr std::size_t max_arena_regions_k = 16;

struct arena_config_t {
  std::size_t size_ = 1024 * 1024;
  std::size_t soft_limit_ = 0; // warning watermark; 0 disables it
  std::size_t hard_limit_ = 0; // total reserved budget; 0 = unbounded
  overflow_mode_t overflow_ = overflow_mode_t::throw_error_;
  arena_warning_fn on_warning_ = nullptr;
  bool touch_pages_ = false;
  std::size_t max_shards_ = max_arena_shards_k;   // shard/tag registry capacity
  std::size_t max_regions_ = max_arena_regions_k; // mmap region capacity
}; // struct arena_config_t

struct bucket_stats_t {
  std::uint64_t tag_;
  std::size_t object_size_;
  std::size_t align_;
  std::uint32_t capacity_;
  std::int64_t in_use_;
  std::int64_t freed_;
}; // struct bucket_stats_t
} // namespace frameworkcpp::arena
