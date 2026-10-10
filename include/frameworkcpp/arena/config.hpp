// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

namespace frameworkcpp::arena {
enum class concurrency_model : std::uint8_t {
    single_thread = 0,
    lock_free = 1,
    thread_safe = 2,
};

enum class overflow_mode : std::uint8_t {
    throw_error = 0,
    abort = 1,
};

using arena_warning_fn = void (*)(const char *message);

class arena_overflow_error : public std::bad_alloc {
  public:
    arena_overflow_error(std::size_t requested, std::size_t hard_limit) {
        std::snprintf(msg_,
                      sizeof(msg_),
                      "arena overflow: %zu requested, hard_limit=%zu",
                      requested,
                      hard_limit);
    }

    [[nodiscard]] const char *what() const noexcept override { return msg_; }

  private:
    char msg_[160]{};
};

struct object_spec_t {
    std::uint64_t tag;
    std::size_t object_size;
    std::size_t align = alignof(std::max_align_t);
    std::size_t count;
};

// Bounded registries keep arena_t heap-free: shards and regions live in fixed
// arrays sized at compile time. These constants are the hard caps.
inline constexpr std::size_t kMaxArenaShards = 256;
inline constexpr std::size_t kMaxArenaRegions = 16;

struct arena_config_t {
    std::size_t size = 1024 * 1024;
    std::size_t soft_limit = 0; // warning watermark; 0 disables it
    std::size_t hard_limit = 0; // total reserved budget; 0 = unbounded
    overflow_mode overflow = overflow_mode::throw_error;
    arena_warning_fn on_warning = nullptr;
    bool touch_pages = false;
    std::size_t max_shards = kMaxArenaShards;   // shard/tag registry capacity
    std::size_t max_regions = kMaxArenaRegions; // mmap region capacity
};

struct bucket_stats_t {
    std::uint64_t tag;
    std::size_t object_size;
    std::size_t align;
    std::uint32_t capacity;
    std::int64_t in_use;
    std::int64_t freed;
};
} // namespace frameworkcpp::arena