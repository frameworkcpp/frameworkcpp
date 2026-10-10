// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena
{
    namespace
    {
        TEST(ArenaShard, CarvesShardAndServesBlocks)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 100}});
            ASSERT_TRUE(shard);
            EXPECT_EQ(shard.tag(), 1ULL);
            EXPECT_TRUE(arena.capacity() > 0);
            EXPECT_TRUE(arena.used() > 0);
        }

        TEST(ArenaShard, AcknowledgeEmptyShardCannotBeCreated)
        {
            arena_t arena;
            auto shard = arena.create_shard(7, {});
            EXPECT_FALSE(shard);
            auto missing = arena.from(7);
            EXPECT_FALSE(missing);
        }

        TEST(ArenaShard, AllocateReuseSameBlockAfterFree)
        {
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

        TEST(ArenaShard, ExhaustionReturnsNullptr)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4}});
            std::vector<std::uint64_t *> ptrs;
            for (int i = 0; i < 4; ++i)
            {
                auto *p = shard.allocate<std::uint64_t>();
                ASSERT_NE(p, nullptr);
                ptrs.push_back(p);
            }
            EXPECT_EQ(shard.allocate<std::uint64_t>(), nullptr);
            shard.deallocate(ptrs[0]);
            EXPECT_NE(shard.allocate<std::uint64_t>(), nullptr);
        }

        TEST(ArenaShard, BucketDeclaredAlignmentIsHonoured)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{0, 16, 32, 64}});
            for (int i = 0; i < 64; ++i)
            {
                auto *p = shard.allocate_bytes(16, 32);
                ASSERT_NE(p, nullptr);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 32, 0);
            }
        }

        TEST(ArenaShard, SmallestFitBucketIsSelected)
        {
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

        TEST(ArenaShard, ShardsAreIsolatedSlices)
        {
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

        TEST(ArenaShard, ArenaGrowsWithAnotherMmapWhenShardDoesNotFit)
        {
            arena_config config;
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

        TEST(ArenaShard, TaggedBucketAllocation)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{99, 32, 8, 16}});
            auto *p = shard.allocate_tagged(99, 32, 8);
            ASSERT_NE(p, nullptr);
            shard.deallocate(p);
            auto *wrong = shard.allocate_tagged(98, 32, 8);
            EXPECT_EQ(wrong, nullptr);
        }

        TEST(ArenaModels, SingleThreadWritesItsOwnBucket)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{0, sizeof(std::uint32_t), alignof(std::uint32_t), 256}});
            auto writer = shard.create_writer(0, concurrency_model::single_thread);
            ASSERT_TRUE(writer);
            auto *p = writer.allocate<std::uint32_t>();
            ASSERT_NE(p, nullptr);
            *p = 9;
            EXPECT_EQ(*p, 9U);
            writer.deallocate(p);
        }

        TEST(ArenaModels, LockFreeBucketSurvivesSharedWriters)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4096}});
            auto writer = shard.create_writer(0, concurrency_model::lock_free);
            ASSERT_TRUE(writer);
            std::atomic<bool> start{false};
            std::vector<std::thread> threads;
            std::atomic<std::size_t> done{0};
            for (int t = 0; t < 8; ++t)
            {
                threads.emplace_back([&] {
                    while (!start.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    for (int i = 0; i < 2000; ++i)
                    {
                        auto *p = writer.allocate<std::uint64_t>();
                        if (p != nullptr)
                        {
                            *p = i;
                            writer.deallocate(p);
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

        TEST(ArenaModels, ThreadSafeBucketWithSharedWriters)
        {
            arena_t arena;
            auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 4096}});
            auto writer = shard.create_writer(0, concurrency_model::thread_safe);
            ASSERT_TRUE(writer);
            std::atomic<bool> start{false};
            std::vector<std::thread> threads;
            std::atomic<std::size_t> done{0};
            for (int t = 0; t < 8; ++t)
            {
                threads.emplace_back([&] {
                    while (!start.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    for (int i = 0; i < 2000; ++i)
                    {
                        auto *p = writer.allocate<std::uint64_t>();
                        if (p != nullptr)
                        {
                            *p = i;
                            writer.deallocate(p);
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

        TEST(ArenaFootprint, CompileTimeFootprintMatchesDeclaredShard)
        {
            constexpr object_spec specs[] = {{0, 8, 8, 100}, {1, 64, 32, 16}};
            constexpr auto bytes = shard_footprint(specs);
            static_assert(bytes > 0);
            static_assert(valid_footprint(bytes));
            arena_t arena;
            auto shard = arena.create_shard(5, {specs[0], specs[1]});
            ASSERT_TRUE(shard);
            EXPECT_TRUE(arena.used() <= bytes);
        }
    } // namespace
} // namespace frameworkcpp::arena