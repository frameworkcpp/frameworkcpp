// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena {
namespace {
TEST(ArenaAllocator, SatisfiesStdAllocatorTraits) {
    static_assert(std::is_same_v<arena_allocator<int>::value_type, int>);
    static_assert(std::is_same_v<std::allocator_traits<arena_allocator<int>>::value_type, int>);
    static_assert(
        std::is_same_v<arena_allocator<int>::rebind<double>::other, arena_allocator<double>>);
    static_assert(!arena_allocator<int>::propagate_on_container_copy_assignment::value);
    static_assert(!arena_allocator<int>::propagate_on_container_move_assignment::value);
    static_assert(!arena_allocator<int>::propagate_on_container_swap::value);
    static_assert(!arena_allocator<int>::is_always_equal::value);
    static_assert(std::is_same_v<decltype(std::declval<const arena_allocator<int> &>()
                                              .select_on_container_copy_construction()),
                                 arena_allocator<int>>);
}

TEST(ArenaAllocator, PowersStdVector) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 1024, alignof(std::max_align_t), 64}});
    const arena_allocator<int> alloc{shard};
    std::vector<int, arena_allocator<int>> values(alloc);
    for (int i = 0; i < 100; ++i)
        values.push_back(i);
    ASSERT_EQ(values.size(), 100U);
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i);
}

TEST(ArenaAllocator, AllocatorsFromSameShardAreEqual) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 64, 8, 64}});
    const arena_allocator<int> a{shard};
    const arena_allocator<int> b{shard};
    const arena_allocator<double> c{shard};
    EXPECT_TRUE(a == a);
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a == c);
    const arena_allocator<int> empty;
    EXPECT_TRUE(empty == empty);
    EXPECT_FALSE(empty == a);
    EXPECT_FALSE(a == empty);
}

TEST(ArenaAllocator, EmptyAllocatorDiffersFromShardZeroAllocator) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 64, 8, 64}});
    const arena_allocator<int> first_shard{shard};
    ASSERT_EQ(shard.shard_id(), 0U);
    const arena_allocator<int> empty;
    EXPECT_FALSE(first_shard == empty);
    EXPECT_FALSE(empty == first_shard);
    EXPECT_NE(first_shard, empty);
}

TEST(ArenaAllocator, PmrVectorRunsOnArenaResource) {
    arena_t arena;
    auto shard = arena.create_shard(1, {{0, 2048, 8, 64}});
    arena_memory_resource resource{shard};
    std::pmr::vector<std::uint64_t> values{
        std::pmr::polymorphic_allocator<std::uint64_t>(&resource)};
    for (std::uint64_t i = 0; i < 200; ++i)
        values.push_back(i);
    ASSERT_EQ(values.size(), 200U);
    for (std::uint64_t i = 0; i < 200; ++i)
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i);
}
} // namespace
} // namespace frameworkcpp::arena