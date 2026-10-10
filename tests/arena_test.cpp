// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena {
namespace {
TEST(ArenaShard, CarvesShardAndServesBlocks) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 100}});
    ASSERT_TRUE(shard);
    EXPECT_EQ(shard.tag(), 1ULL);
    EXPECT_TRUE(arena.capacity() > 0);
    EXPECT_TRUE(arena.used() > 0);
}

TEST(ArenaShard, AcknowledgeEmptyShardCannotBeCreated) {
    arena_t arena;
    auto shard = arena.create_shard(7, {});
    EXPECT_FALSE(shard);
    auto missing = arena.from(7);
    EXPECT_FALSE(missing);
}

TEST(ArenaShard, AllocateReuseSameBlockAfterFree) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 10}});
    auto *first = shard.allocate<std::uint64_t>();
    ASSERT_NE(first, nullptr);
    *first = 42;
    shard.deallocate(first);
    auto *second = shard.allocate<std::uint64_t>();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second, first);
}

TEST(ArenaShard, ExhaustionReturnsNullptr) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4}});
    std::vector<std::uint64_t *> ptrs;
    for (int i = 0; i < 4; ++i) {
        auto *p = shard.allocate<std::uint64_t>();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(shard.allocate<std::uint64_t>(), nullptr);
    shard.deallocate(ptrs[0]);
    EXPECT_NE(shard.allocate<std::uint64_t>(), nullptr);
}

TEST(ArenaShard, BucketDeclaredAlignmentIsHonoured) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 16, 32, 64}});
    for (int i = 0; i < 64; ++i) {
        auto *p = shard.allocate_bytes(16, 32);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 32, 0);
    }
}

TEST(ArenaShard, SmallestFitBucketIsSelected) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 16, 8, 10}, {1, 64, 8, 10}, {2, 256, 8, 10}});
    auto *small = shard.allocate<std::uint32_t>();
    ASSERT_NE(small, nullptr);
    auto *mid = shard.allocate_bytes(24, 8);
    ASSERT_NE(mid, nullptr);
    auto *large = shard.allocate_bytes(200, 8);
    ASSERT_NE(large, nullptr);
    shard.deallocate(small);
    shard.deallocate(mid);
    shard.deallocate(large);
}

TEST(ArenaShard, ShardsAreIsolatedSlices) {
    arena_t arena;
    auto a = arena.create_shard(1, {{0, 8, 8, 2}});
    auto b = arena.create_shard(2, {{0, 8, 8, 2}});
    EXPECT_NE(a.shard_id(), b.shard_id());
    auto *pa1 = a.allocate<std::uint64_t>();
    auto *pb1 = b.allocate<std::uint64_t>();
    ASSERT_NE(pa1, nullptr);
    ASSERT_NE(pb1, nullptr);
    EXPECT_NE(pa1, pb1);
    a.deallocate(pa1);
    b.deallocate(pb1);
}

TEST(ArenaShard, ArenaGrowsWithAnotherMmapWhenShardDoesNotFit) {
    arena_config_t config;
    config.size = 4096;
    arena_t arena{config};
    auto small = arena.create_shard(1, {{0, 8, 8, 100}});
    ASSERT_TRUE(small);
    const auto capacity_after_small = arena.capacity();
    auto big = arena.create_shard(2, {{0, 1024, 8, 100}});
    ASSERT_TRUE(big);
    EXPECT_GT(arena.capacity(), capacity_after_small);
    auto *p = big.allocate_bytes(1024, 8);
    ASSERT_NE(p, nullptr);
    arena.deallocate(p);
}

TEST(ArenaShard, TaggedBucketAllocation) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{99, 32, 8, 16}});
    auto *p = shard.allocate_tagged(99, 32, 8);
    ASSERT_NE(p, nullptr);
    shard.deallocate(p);
    auto *wrong = shard.allocate_tagged(98, 32, 8);
    EXPECT_EQ(wrong, nullptr);
}

TEST(ArenaModels, SingleThreadWritesItsOwnBucket) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint32_t), alignof(std::uint32_t), 256}});
    auto bucket = shard.get_bucket(0, concurrency_model::single_thread);
    ASSERT_TRUE(bucket);
    auto *p = bucket.allocate<std::uint32_t>();
    ASSERT_NE(p, nullptr);
    *p = 9;
    EXPECT_EQ(*p, 9U);
    bucket.deallocate(p);
}

TEST(ArenaModels, LockFreeBucketSurvivesSharedWriters) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4096}});
    auto bucket = shard.get_bucket(0, concurrency_model::lock_free);
    ASSERT_TRUE(bucket);
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(8);
    std::atomic<std::size_t> done{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 2000; ++i) {
                auto *p = bucket.allocate<std::uint64_t>();
                if (p != nullptr) {
                    *p = i;
                    bucket.deallocate(p);
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    EXPECT_EQ(done.load(std::memory_order_acquire), 8U);
}

TEST(ArenaModels, ThreadSafeBucketWithSharedWriters) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4096}});
    auto bucket = shard.get_bucket(0, concurrency_model::thread_safe);
    ASSERT_TRUE(bucket);
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(8);
    std::atomic<std::size_t> done{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 2000; ++i) {
                auto *p = bucket.allocate<std::uint64_t>();
                if (p != nullptr) {
                    *p = i;
                    bucket.deallocate(p);
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    EXPECT_EQ(done.load(std::memory_order_acquire), 8U);
}

TEST(ArenaFootprint, CompileTimeFootprintMatchesDeclaredShard) {
    constexpr object_spec_t specs[] = {{0, 8, 8, 100}, {1, 64, 32, 16}};
    constexpr auto bytes = shard_footprint(specs);
    static_assert(bytes > 0);
    static_assert(valid_footprint(bytes));
    arena_t arena;
    auto shard = arena.create_shard(5, {specs[0], specs[1]});
    ASSERT_TRUE(shard);
    EXPECT_TRUE(arena.used() <= bytes);
}

TEST(ArenaShard, SecondShardKeepsHighAlignment) {
    arena_t arena;
    auto a = arena.create_shard(1, {{0, 11, 4, 3}});
    ASSERT_TRUE(a);
    auto b = arena.create_shard(2, {{0, 16, 64, 8}, {1, 16, 32, 8}});
    ASSERT_TRUE(b);
    auto *p64 = b.allocate_bytes(16, 64);
    ASSERT_NE(p64, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p64) % 64, 0);
    auto *p32 = b.allocate_bytes(16, 32);
    ASSERT_NE(p32, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p32) % 32, 0);
    auto *p16 = b.allocate_bytes(16, 16);
    ASSERT_NE(p16, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p16) % 16, 0);
    b.deallocate(p64);
    b.deallocate(p32);
    b.deallocate(p16);
}

TEST(ArenaShard, RejectsInvalidSpecs) {
    arena_t arena;
    auto bad_align = arena.create_shard(1, {{0, 16, 3, 8}});
    auto bad_zero = arena.create_shard(2, {{0, 16, 0, 8}});
    auto bad_size = arena.create_shard(3, {{0, 0, 8, 8}});
    auto bad_count = arena.create_shard(4, {{0, 8, 8, 0}});
    auto bad_overalign = arena.create_shard(5, {{0, 8, 512, 8}});
    EXPECT_FALSE(bad_align);
    EXPECT_FALSE(bad_zero);
    EXPECT_FALSE(bad_size);
    EXPECT_FALSE(bad_count);
    EXPECT_FALSE(bad_overalign);
    auto ok = arena.create_shard(6, {{0, 16, 8, 4}});
    ASSERT_TRUE(ok);
}

TEST(ArenaShard, DeallocateForeignPointerIsBenign) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 8, 8, 8}});
    auto *p = shard.allocate<std::uint64_t>();
    ASSERT_NE(p, nullptr);
    shard.deallocate(p);
    shard.deallocate_bytes(reinterpret_cast<void *>(0x1234));
    arena.deallocate_bytes(reinterpret_cast<void *>(0x5678));
    auto *p2 = shard.allocate<std::uint64_t>();
    ASSERT_NE(p2, nullptr);
    shard.deallocate(p2);
}

TEST(ArenaShard, BucketStatsTrackUseAndFrees) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 8, 8, 16}});
    ASSERT_TRUE(shard);
    EXPECT_EQ(shard.stats(0).in_use, 0);
    auto *a1 = shard.allocate<std::uint64_t>();
    auto *a2 = shard.allocate<std::uint64_t>();
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);
    EXPECT_EQ(shard.stats(0).in_use, 2);
    shard.deallocate(a1);
    shard.deallocate(a2);
    const auto s = shard.stats(0);
    EXPECT_EQ(s.in_use, 0);
    EXPECT_EQ(s.freed, 2);
    EXPECT_EQ(arena.live(), 0U);
}

TEST(ArenaFootprint, SummedFootprintSizesWholeArenaAtCompileTime) {
    constexpr object_spec_t shard_a[] = {{0, 8, 8, 16}};
    constexpr object_spec_t shard_b[] = {{0, 64, 8, 8}, {1, 32, 32, 8}};
    constexpr auto need = round_to_page(shard_footprint(shard_a) + shard_footprint(shard_b));
    static_assert(need > 0);
    arena_config_t config;
    config.size = need;
    arena_t arena{config};
    auto a = arena.create_shard(1, {shard_a[0]});
    auto b = arena.create_shard(2, {shard_b[0], shard_b[1]});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(arena.shard_count(), 2U);
    EXPECT_EQ(arena.capacity(), need);
}
TEST(ArenaLimits, SoftLimitLatchesAndWarnsOnce) {
    static bool warned = false;
    warned = false;
    arena_config_t cfg;
    cfg.size = 4096;
    cfg.soft_limit = 4096;
    cfg.on_warning = [](const char *) {
        warned = true;
    };
    arena_t arena{cfg};
    EXPECT_FALSE(arena.soft_limit_exceeded());
    auto shard = arena.create_shard(1, {{0, 8, 8, 100}});
    ASSERT_TRUE(shard);
    EXPECT_TRUE(warned);
    EXPECT_TRUE(arena.soft_limit_exceeded());
    arena.create_shard(2, {{0, 8, 8, 100}});
    EXPECT_TRUE(warned);
}

TEST(ArenaLimits, HardLimitThrowsBeyondBudget) {
    arena_config_t cfg;
    cfg.size = 4096;
    cfg.hard_limit = 8192;
    arena_t arena{cfg};
    EXPECT_EQ(arena.hard_limit(), 8192U);
    EXPECT_EQ(arena.capacity(), 0U);
    auto small = arena.create_shard(1, {{0, 8, 8, 16}});
    ASSERT_TRUE(small);
    EXPECT_TRUE(arena.capacity() <= cfg.hard_limit);
    EXPECT_THROW(arena.create_shard(2, {{0, 8192, 8, 64}}), arena_overflow_error);
    EXPECT_EQ(arena.shard_count(), 1U);
}

TEST(ArenaLimits, GrowthStaysWithinHardLimit) {
    arena_config_t cfg;
    cfg.size = 4096;
    cfg.hard_limit = 16384;
    arena_t arena{cfg};
    auto a = arena.create_shard(1, {{0, 512, 8, 8}});
    auto b = arena.create_shard(2, {{0, 512, 8, 8}});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_TRUE(arena.capacity() <= cfg.hard_limit);
    EXPECT_EQ(arena.shard_count(), 2U);
}

TEST(ArenaLimits, RejectsSoftAboveHard) {
    arena_config_t cfg;
    cfg.soft_limit = 1024;
    cfg.hard_limit = 512;
    EXPECT_THROW(arena_t{cfg}, std::invalid_argument);
}
} // namespace
} // namespace frameworkcpp::arena