// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <utility>

#include "frameworkcpp/memory/memory.hpp"

namespace frameworkcpp::memory {
namespace {
struct tracked_t {
  static std::atomic<int> destroyed_;
  int value_;

  explicit tracked_t(int value) : value_(value) {}
  tracked_t(const tracked_t &) = delete;
  tracked_t &operator=(const tracked_t &) = delete;
  tracked_t(tracked_t &&) = delete;
  tracked_t &operator=(tracked_t &&) = delete;
  ~tracked_t() { destroyed_.fetch_add(1, std::memory_order_relaxed); }
};
std::atomic<int> tracked_t::destroyed_{0};

struct shareable_t : memory_enable_shared_from_this_t<shareable_t> {
  int value_;
  explicit shareable_t(int value) : value_(value) {}
};

TEST(MemoryPtr, UniqueConstructsAndReturnsMemoryOnDestroy) {
  memory_config_t _config;
  _config.size_ = 4096;
  memory_t _memory{_config};
  auto _shard = _memory.create_shard(1, {{0, sizeof(tracked_t), alignof(tracked_t), 4}});
  {
    auto _unique = make_memory_unique_ptr<tracked_t>(_shard, 11);
    ASSERT_TRUE(_unique);
    EXPECT_EQ(_unique->value_, 11);
  }
  EXPECT_EQ(tracked_t::destroyed_.load(), 1);
  auto *_raw = _shard.allocate_bytes(sizeof(tracked_t), alignof(tracked_t));
  ASSERT_NE(_raw, nullptr);
  _shard.deallocate_bytes(_raw);
}

TEST(MemoryPtr, UniqueMovesOwnership) {
  memory_t _memory;
  auto _shard = _memory.create_shard(1, {{0, sizeof(int), alignof(int), 4}});
  auto _owner = make_memory_unique_ptr<int>(_shard, 5);
  ASSERT_TRUE(_owner);
  auto _moved = std::move(_owner);
  // NOLINTNEXTLINE(bugprone-use-after-move) -- intended: moved-from is empty
  EXPECT_FALSE(_owner);
  ASSERT_TRUE(_moved);
  EXPECT_EQ(*_moved, 5);
}

TEST(MemoryPtr, SharedCountsReferencesAndFreesAtZero) {
  memory_t _memory;
  auto _shard = _memory.create_shard(1, {{0, 128, alignof(std::max_align_t), 4}});
  {
    auto _owner = make_memory_shared_ptr<tracked_t>(_shard, 21);
    ASSERT_TRUE(_owner);
    EXPECT_EQ(_owner->value_, 21);
    {
      auto _copy = _owner; // NOLINT(performance-unnecessary-copy-initialization) -- deliberate
                           // shared-copy refcount exercise
      _copy = _owner;
      EXPECT_EQ(_copy->value_, 21);
    }
    EXPECT_EQ(tracked_t::destroyed_.load(), 0);
  }
  EXPECT_EQ(tracked_t::destroyed_.load(), 1);
}

TEST(MemoryPtr, SharedFromThisReturnsOwner) {
  memory_t _memory;
  auto _shard = _memory.create_shard(1, {{0, 128, alignof(std::max_align_t), 4}});
  auto _owner = make_memory_shared_ptr<shareable_t>(_shard, 33);
  ASSERT_TRUE(_owner);
  auto _shared = _owner->shared_from_this();
  EXPECT_EQ(_shared->value_, 33);
  EXPECT_EQ(_shared.get(), _owner.get());
}

TEST(MemoryPtr, SharedReusesBlockAfterLastRelease) {
  memory_config_t _config;
  _config.size_ = 8192;
  memory_t _memory{_config};
  auto _shard = _memory.create_shard(1, {{0, 64, 8, 8}});
  std::byte *_first = nullptr;
  {
    auto _owner = make_memory_shared_ptr<std::uint64_t>(_shard, 1);
    _first = reinterpret_cast<std::byte *>(_owner.get());
  }
  auto _second = make_memory_shared_ptr<std::uint64_t>(_shard, 2);
  EXPECT_EQ(reinterpret_cast<std::byte *>(_second.get()), _first);
}
} // namespace
} // namespace frameworkcpp::memory
