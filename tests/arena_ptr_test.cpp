// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <utility>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena {
namespace {
struct tracked {
    static std::atomic<int> destroyed;
    int value;

    explicit tracked(int v) : value(v) {}
    tracked(const tracked &) = delete;
    tracked &operator=(const tracked &) = delete;
    tracked(tracked &&) = delete;
    tracked &operator=(tracked &&) = delete;
    ~tracked() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};
std::atomic<int> tracked::destroyed{0};

struct shareable : arena_enable_shared_from_this<shareable> {
    int value;
    explicit shareable(int v) : value(v) {}
};

TEST(ArenaPtr, UniqueConstructsAndReturnsMemoryOnDestroy) {
    arena_config_t config;
    config.size = 4096;
    arena_t arena{config};
    auto shard = arena.create_shard(1, {{0, sizeof(tracked), alignof(tracked), 4}});
    {
        auto p = make_arena_unique_ptr<tracked>(shard, 11);
        ASSERT_TRUE(p);
        EXPECT_EQ(p->value, 11);
    }
    EXPECT_EQ(tracked::destroyed.load(), 1);
    auto *raw = shard.allocate_bytes(sizeof(tracked), alignof(tracked));
    ASSERT_NE(raw, nullptr);
    shard.deallocate_bytes(raw);
}

TEST(ArenaPtr, UniqueMovesOwnership) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(int), alignof(int), 4}});
    auto a = make_arena_unique_ptr<int>(shard, 5);
    ASSERT_TRUE(a);
    auto b = std::move(a);
    // NOLINTNEXTLINE(bugprone-use-after-move) -- intended: moved-from is empty
    EXPECT_FALSE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(*b, 5);
}

TEST(ArenaPtr, SharedCountsReferencesAndFreesAtZero) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 128, alignof(std::max_align_t), 4}});
    {
        auto a = make_arena_shared_ptr<tracked>(shard, 21);
        ASSERT_TRUE(a);
        EXPECT_EQ(a->value, 21);
        {
            auto b = a; // NOLINT(performance-unnecessary-copy-initialization) -- deliberate
                        // shared-copy refcount exercise
            b = a;
            EXPECT_EQ(b->value, 21);
        }
        EXPECT_EQ(tracked::destroyed.load(), 0);
    }
    EXPECT_EQ(tracked::destroyed.load(), 1);
}

TEST(ArenaPtr, SharedFromThisReturnsOwner) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 128, alignof(std::max_align_t), 4}});
    auto a = make_arena_shared_ptr<shareable>(shard, 33);
    ASSERT_TRUE(a);
    auto b = a->shared_from_this();
    EXPECT_EQ(b->value, 33);
    EXPECT_EQ(b.get(), a.get());
}

TEST(ArenaPtr, SharedReusesBlockAfterLastRelease) {
    arena_config_t config;
    config.size = 8192;
    arena_t arena{config};
    auto shard = arena.create_shard(1, {{0, 64, 8, 8}});
    std::byte *first = nullptr;
    {
        auto a = make_arena_shared_ptr<std::uint64_t>(shard, 1);
        first = reinterpret_cast<std::byte *>(a.get());
    }
    auto b = make_arena_shared_ptr<std::uint64_t>(shard, 2);
    EXPECT_EQ(reinterpret_cast<std::byte *>(b.get()), first);
}
} // namespace
} // namespace frameworkcpp::arena