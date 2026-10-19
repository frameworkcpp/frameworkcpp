// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena {
namespace {
TEST(ArenaShard, CarvesShardAndServesBlocks) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 100}});
  ASSERT_TRUE(_shard);
  EXPECT_EQ(_shard.get_tag(), 1ULL);
  EXPECT_TRUE(_arena.get_capacity() > 0);
  EXPECT_TRUE(_arena.get_used() > 0);
}

TEST(ArenaShard, AcknowledgeEmptyShardCannotBeCreated) {
  arena_t _arena;
  auto _shard = _arena.create_shard(7, {});
  EXPECT_FALSE(_shard);
  auto _missing = _arena.get_shard(7);
  EXPECT_FALSE(_missing);
}

TEST(ArenaShard, AllocateReuseSameBlockAfterFree) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 10}});
  auto *_first = _shard.allocate<std::uint64_t>();
  ASSERT_NE(_first, nullptr);
  *_first = 42;
  _shard.deallocate(_first);
  auto *_second = _shard.allocate<std::uint64_t>();
  ASSERT_NE(_second, nullptr);
  EXPECT_EQ(_second, _first);
}

TEST(ArenaShard, ExhaustionReturnsNullptr) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4}});
  std::vector<std::uint64_t *> _pointers;
  for (int _index = 0; _index < 4; ++_index) {
    auto *_pointer = _shard.allocate<std::uint64_t>();
    ASSERT_NE(_pointer, nullptr);
    _pointers.push_back(_pointer);
  }
  EXPECT_EQ(_shard.allocate<std::uint64_t>(), nullptr);
  _shard.deallocate(_pointers[0]);
  EXPECT_NE(_shard.allocate<std::uint64_t>(), nullptr);
}

TEST(ArenaShard, BucketDeclaredAlignmentIsHonoured) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 16, 32, 64}});
  for (int _index = 0; _index < 64; ++_index) {
    auto *_pointer = _shard.allocate_bytes(16, 32);
    ASSERT_NE(_pointer, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(_pointer) % 32, 0);
  }
}

TEST(ArenaShard, SmallestFitBucketIsSelected) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 16, 8, 10}, {1, 64, 8, 10}, {2, 256, 8, 10}});
  auto *_small = _shard.allocate<std::uint32_t>();
  ASSERT_NE(_small, nullptr);
  auto *_medium = _shard.allocate_bytes(24, 8);
  ASSERT_NE(_medium, nullptr);
  auto *_large = _shard.allocate_bytes(200, 8);
  ASSERT_NE(_large, nullptr);
  _shard.deallocate(_small);
  _shard.deallocate(_medium);
  _shard.deallocate(_large);
}

TEST(ArenaShard, UnsortedSpecsStillPickSmallestFit) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{2, 256, 8, 8}, {0, 16, 8, 8}, {1, 64, 8, 8}});
  ASSERT_TRUE(_shard);
  // Descriptors are published ordered by ascending object_size.
  EXPECT_EQ(_shard.get_stats(0).object_size_, 16U);
  EXPECT_EQ(_shard.get_stats(1).object_size_, 64U);
  EXPECT_EQ(_shard.get_stats(2).object_size_, 256U);

  auto *_small = _shard.allocate<std::uint32_t>();
  auto *_medium = _shard.allocate_bytes(24, 8);
  auto *_large = _shard.allocate_bytes(200, 8);
  ASSERT_NE(_small, nullptr);
  ASSERT_NE(_medium, nullptr);
  ASSERT_NE(_large, nullptr);
  // The smallest fitting bucket won: 16-byte bucket for 4 bytes, 64 for 24,
  // 256 for 200. Distinct buckets => distinct runs, so pointers differ.
  EXPECT_NE(static_cast<const void *>(_small), static_cast<const void *>(_medium));
  EXPECT_NE(static_cast<const void *>(_medium), static_cast<const void *>(_large));
  _shard.deallocate(_small);
  _shard.deallocate(_medium);
  _shard.deallocate(_large);
}

TEST(ArenaShard, AlignmentBeatsSmallerBucket) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{1, 32, 64, 8}, {0, 16, 8, 8}});
  ASSERT_TRUE(_shard);
  // 16 bytes fit the 16-byte bucket, but it only guarantees 8-byte alignment;
  // the 32-byte/64-aligned bucket must be picked instead.
  auto *_pointer = _shard.allocate_bytes(16, 64);
  ASSERT_NE(_pointer, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(_pointer) % 64, 0);
  _shard.deallocate(_pointer);
}

TEST(ArenaShard, ShardsAreIsolatedSlices) {
  arena_t _arena;
  auto _a = _arena.create_shard(1, {{0, 8, 8, 2}});
  auto _b = _arena.create_shard(2, {{0, 8, 8, 2}});
  EXPECT_NE(_a.get_shard_id(), _b.get_shard_id());
  auto *_pointer_a = _a.allocate<std::uint64_t>();
  auto *_pointer_b = _b.allocate<std::uint64_t>();
  ASSERT_NE(_pointer_a, nullptr);
  ASSERT_NE(_pointer_b, nullptr);
  EXPECT_NE(_pointer_a, _pointer_b);
  _a.deallocate(_pointer_a);
  _b.deallocate(_pointer_b);
}

TEST(ArenaShard, ArenaGrowsWithAnotherMmapWhenShardDoesNotFit) {
  arena_config_t _config;
  _config.size_ = 4096;
  arena_t _arena{_config};
  auto _small = _arena.create_shard(1, {{0, 8, 8, 100}});
  ASSERT_TRUE(_small);
  const auto _capacity_after_small_k = _arena.get_capacity();
  auto _big = _arena.create_shard(2, {{0, 1024, 8, 100}});
  ASSERT_TRUE(_big);
  EXPECT_GT(_arena.get_capacity(), _capacity_after_small_k);
  auto *_pointer = _big.allocate_bytes(1024, 8);
  ASSERT_NE(_pointer, nullptr);
  _big.deallocate(_pointer);
}

TEST(ArenaShard, TaggedBucketAllocation) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{99, 32, 8, 16}});
  auto *_pointer = _shard.allocate_tagged(99, 32, 8);
  ASSERT_NE(_pointer, nullptr);
  _shard.deallocate(_pointer);
  auto *_wrong = _shard.allocate_tagged(98, 32, 8);
  EXPECT_EQ(_wrong, nullptr);
}

TEST(ArenaModels, SingleThreadWritesItsOwnBucket) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint32_t), alignof(std::uint32_t), 256}});
  auto _bucket = _shard.get_bucket(0, concurrency_model_t::single_thread_);
  ASSERT_TRUE(_bucket);
  auto *_pointer = _bucket.allocate<std::uint32_t>();
  ASSERT_NE(_pointer, nullptr);
  *_pointer = 9;
  EXPECT_EQ(*_pointer, 9U);
  _bucket.deallocate(_pointer);
}

TEST(ArenaModels, LockFreeBucketSurvivesSharedWriters) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4096}});
  auto _bucket = _shard.get_bucket(0, concurrency_model_t::lock_free_);
  ASSERT_TRUE(_bucket);
  std::atomic<bool> _start{false};
  std::vector<std::thread> _threads;
  _threads.reserve(8);
  std::atomic<std::size_t> _done{0};
  for (int _thread = 0; _thread < 8; ++_thread) {
    _threads.emplace_back([&] {
      while (!_start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (int _iteration = 0; _iteration < 2000; ++_iteration) {
        auto *_pointer = _bucket.allocate<std::uint64_t>();
        if (_pointer != nullptr) {
          *_pointer = _iteration;
          _bucket.deallocate(_pointer);
        }
      }
      _done.fetch_add(1, std::memory_order_release);
    });
  }
  _start.store(true, std::memory_order_release);
  for (auto &_thread : _threads)
    _thread.join();
  EXPECT_EQ(_done.load(std::memory_order_acquire), 8U);
}

TEST(ArenaModels, ThreadSafeBucketWithSharedWriters) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4096}});
  auto _bucket = _shard.get_bucket(0, concurrency_model_t::thread_safe_);
  ASSERT_TRUE(_bucket);
  std::atomic<bool> _start{false};
  std::vector<std::thread> _threads;
  _threads.reserve(8);
  std::atomic<std::size_t> _done{0};
  for (int _thread = 0; _thread < 8; ++_thread) {
    _threads.emplace_back([&] {
      while (!_start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (int _iteration = 0; _iteration < 2000; ++_iteration) {
        auto *_pointer = _bucket.allocate<std::uint64_t>();
        if (_pointer != nullptr) {
          *_pointer = _iteration;
          _bucket.deallocate(_pointer);
        }
      }
      _done.fetch_add(1, std::memory_order_release);
    });
  }
  _start.store(true, std::memory_order_release);
  for (auto &_thread : _threads)
    _thread.join();
  EXPECT_EQ(_done.load(std::memory_order_acquire), 8U);
}

TEST(ArenaFootprint, CompileTimeFootprintMatchesDeclaredShard) {
  constexpr object_spec_t _specs_k[] = {{0, 8, 8, 100}, {1, 64, 32, 16}};
  constexpr auto _bytes_k = shard_footprint(_specs_k);
  static_assert(_bytes_k > 0);
  static_assert(is_valid_footprint(_bytes_k));
  arena_t _arena;
  auto _shard = _arena.create_shard(5, {_specs_k[0], _specs_k[1]});
  ASSERT_TRUE(_shard);
  EXPECT_TRUE(_arena.get_used() <= _bytes_k);
}

TEST(ArenaShard, SecondShardKeepsHighAlignment) {
  arena_t _arena;
  auto _a = _arena.create_shard(1, {{0, 11, 4, 3}});
  ASSERT_TRUE(_a);
  auto _b = _arena.create_shard(2, {{0, 16, 64, 8}, {1, 16, 32, 8}});
  ASSERT_TRUE(_b);
  auto *_pointer64 = _b.allocate_bytes(16, 64);
  ASSERT_NE(_pointer64, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(_pointer64) % 64, 0);
  auto *_pointer32 = _b.allocate_bytes(16, 32);
  ASSERT_NE(_pointer32, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(_pointer32) % 32, 0);
  auto *_pointer16 = _b.allocate_bytes(16, 16);
  ASSERT_NE(_pointer16, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(_pointer16) % 16, 0);
  _b.deallocate(_pointer64);
  _b.deallocate(_pointer32);
  _b.deallocate(_pointer16);
}

TEST(ArenaShard, RejectsInvalidSpecs) {
  arena_t _arena;
  auto _bad_align = _arena.create_shard(1, {{0, 16, 3, 8}});
  auto _bad_zero = _arena.create_shard(2, {{0, 16, 0, 8}});
  auto _bad_size = _arena.create_shard(3, {{0, 0, 8, 8}});
  auto _bad_count = _arena.create_shard(4, {{0, 8, 8, 0}});
  auto _bad_overalign = _arena.create_shard(5, {{0, 8, 512, 8}});
  EXPECT_FALSE(_bad_align);
  EXPECT_FALSE(_bad_zero);
  EXPECT_FALSE(_bad_size);
  EXPECT_FALSE(_bad_count);
  EXPECT_FALSE(_bad_overalign);
  auto _ok = _arena.create_shard(6, {{0, 16, 8, 4}});
  ASSERT_TRUE(_ok);
}

TEST(ArenaShard, DeallocateForeignPointerIsBenign) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 8, 8, 8}});
  auto *_pointer = _shard.allocate<std::uint64_t>();
  ASSERT_NE(_pointer, nullptr);
  _shard.deallocate(_pointer);
  // A pointer that was never handed out by a shard is ignored, not trusted.
  _shard.deallocate_bytes(reinterpret_cast<void *>(0x1234));
  auto *_again = _shard.allocate<std::uint64_t>();
  ASSERT_NE(_again, nullptr);
  _shard.deallocate(_again);
}

TEST(ArenaShard, DeallocateThroughWrongShardIsBenign) {
  arena_t _arena;
  auto _a = _arena.create_shard(1, {{0, 8, 8, 4}});
  auto _b = _arena.create_shard(2, {{0, 8, 8, 4}});
  auto *_pointer = _a.allocate<std::uint64_t>();
  ASSERT_NE(_pointer, nullptr);
  _b.deallocate_bytes(_pointer); // outside b's slice: ignored
  _a.deallocate(_pointer);       // still freeable through its own shard
  auto *_again = _a.allocate<std::uint64_t>();
  ASSERT_NE(_again, nullptr);
  _a.deallocate(_again);
}

TEST(ArenaType, IsNeitherCopyableNorMovable) {
  static_assert(!std::is_copy_constructible_v<arena_t>);
  static_assert(!std::is_copy_assignable_v<arena_t>);
  static_assert(!std::is_move_constructible_v<arena_t>);
  static_assert(!std::is_move_assignable_v<arena_t>);
}

TEST(ArenaShard, BucketStatsTrackUseAndFrees) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 8, 8, 16}});
  ASSERT_TRUE(_shard);
  EXPECT_EQ(_shard.get_stats(0).in_use_, 0);
  auto *_first = _shard.allocate<std::uint64_t>();
  auto *_second = _shard.allocate<std::uint64_t>();
  ASSERT_NE(_first, nullptr);
  ASSERT_NE(_second, nullptr);
  EXPECT_EQ(_shard.get_stats(0).in_use_, 2);
  _shard.deallocate(_first);
  _shard.deallocate(_second);
  const auto _stats_k = _shard.get_stats(0);
  EXPECT_EQ(_stats_k.in_use_, 0);
  EXPECT_EQ(_stats_k.freed_, 2);
  EXPECT_EQ(_arena.get_live(), 0U);
}

TEST(ArenaFootprint, SummedFootprintSizesWholeArenaAtCompileTime) {
  constexpr object_spec_t _shard_a_k[] = {{0, 8, 8, 16}};
  constexpr object_spec_t _shard_b_k[] = {{0, 64, 8, 8}, {1, 32, 32, 8}};
  constexpr auto _need_k = round_to_page(shard_footprint(_shard_a_k) + shard_footprint(_shard_b_k));
  static_assert(_need_k > 0);
  arena_config_t _config;
  _config.size_ = _need_k;
  arena_t _arena{_config};
  auto _a = _arena.create_shard(1, {_shard_a_k[0]});
  auto _b = _arena.create_shard(2, {_shard_b_k[0], _shard_b_k[1]});
  ASSERT_TRUE(_a);
  ASSERT_TRUE(_b);
  EXPECT_EQ(_arena.get_shard_count(), 2U);
  EXPECT_EQ(_arena.get_capacity(), _need_k);
}
TEST(ArenaLimits, SoftLimitLatchesAndWarnsOnce) {
  static bool _warned = false;
  _warned = false;
  arena_config_t _config;
  _config.size_ = 4096;
  _config.soft_limit_ = 4096;
  _config.on_warning_ = [](const char *) {
    _warned = true;
  };
  arena_t _arena{_config};
  EXPECT_FALSE(_arena.is_soft_limit_exceeded());
  auto _shard = _arena.create_shard(1, {{0, 8, 8, 100}});
  ASSERT_TRUE(_shard);
  EXPECT_TRUE(_warned);
  EXPECT_TRUE(_arena.is_soft_limit_exceeded());
  _arena.create_shard(2, {{0, 8, 8, 100}});
  EXPECT_TRUE(_warned);
}

TEST(ArenaLimits, HardLimitThrowsBeyondBudget) {
  arena_config_t _config;
  _config.size_ = 4096;
  _config.hard_limit_ = 8192;
  arena_t _arena{_config};
  EXPECT_EQ(_arena.get_hard_limit(), 8192U);
  EXPECT_EQ(_arena.get_capacity(), 0U);
  auto _small = _arena.create_shard(1, {{0, 8, 8, 16}});
  ASSERT_TRUE(_small);
  EXPECT_TRUE(_arena.get_capacity() <= _config.hard_limit_);
  EXPECT_THROW(_arena.create_shard(2, {{0, 8192, 8, 64}}), arena_overflow_exception);
  EXPECT_EQ(_arena.get_shard_count(), 1U);
}

TEST(ArenaLimits, GrowthStaysWithinHardLimit) {
  arena_config_t _config;
  _config.size_ = 4096;
  _config.hard_limit_ = 16384;
  arena_t _arena{_config};
  auto _a = _arena.create_shard(1, {{0, 512, 8, 8}});
  auto _b = _arena.create_shard(2, {{0, 512, 8, 8}});
  ASSERT_TRUE(_a);
  ASSERT_TRUE(_b);
  EXPECT_TRUE(_arena.get_capacity() <= _config.hard_limit_);
  EXPECT_EQ(_arena.get_shard_count(), 2U);
}

TEST(ArenaLimits, RejectsSoftAboveHard) {
  arena_config_t _config;
  _config.soft_limit_ = 1024;
  _config.hard_limit_ = 512;
  EXPECT_THROW(arena_t{_config}, std::invalid_argument);
}
} // namespace
} // namespace frameworkcpp::arena
