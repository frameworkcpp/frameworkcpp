// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <vector>

#include "frameworkcpp/arena/arena.hpp"

namespace frameworkcpp::arena {
namespace {
TEST(ArenaAllocator, SatisfiesStdAllocatorTraits) {
  static_assert(std::is_same_v<arena_allocator_t<int>::value_type, int>);
  static_assert(std::is_same_v<std::allocator_traits<arena_allocator_t<int>>::value_type, int>);
  static_assert(
      std::is_same_v<arena_allocator_t<int>::rebind<double>::other, arena_allocator_t<double>>);
  static_assert(!arena_allocator_t<int>::propagate_on_container_copy_assignment::value);
  static_assert(!arena_allocator_t<int>::propagate_on_container_move_assignment::value);
  static_assert(!arena_allocator_t<int>::propagate_on_container_swap::value);
  static_assert(!arena_allocator_t<int>::is_always_equal::value);
  static_assert(std::is_same_v<decltype(std::declval<const arena_allocator_t<int> &>()
                                            .select_on_container_copy_construction()),
                               arena_allocator_t<int>>);
}

TEST(ArenaAllocator, PowersStdVector) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 1024, alignof(std::max_align_t), 64}});
  const arena_allocator_t<int> _allocator_k{_shard};
  std::vector<int, arena_allocator_t<int>> _values(_allocator_k);
  for (int _index = 0; _index < 100; ++_index)
    _values.push_back(_index);
  ASSERT_EQ(_values.size(), 100U);
  for (int _index = 0; _index < 100; ++_index)
    EXPECT_EQ(_values[static_cast<std::size_t>(_index)], _index);
}

TEST(ArenaAllocator, AllocatorsFromSameShardAreEqual) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 64, 8, 64}});
  const arena_allocator_t<int> _allocator_a_k{_shard};
  const arena_allocator_t<int> _allocator_b_k{_shard};
  const arena_allocator_t<double> _allocator_c_k{_shard};
  EXPECT_TRUE(_allocator_a_k == _allocator_a_k);
  EXPECT_TRUE(_allocator_a_k == _allocator_b_k);
  EXPECT_TRUE(_allocator_a_k == _allocator_c_k);
  const arena_allocator_t<int> _empty_k;
  EXPECT_TRUE(_empty_k == _empty_k);
  EXPECT_FALSE(_empty_k == _allocator_a_k);
  EXPECT_FALSE(_allocator_a_k == _empty_k);
}

TEST(ArenaAllocator, EmptyAllocatorDiffersFromShardZeroAllocator) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 64, 8, 64}});
  const arena_allocator_t<int> _first_shard_k{_shard};
  ASSERT_EQ(_shard.get_shard_id(), 0U);
  const arena_allocator_t<int> _empty_k;
  EXPECT_FALSE(_first_shard_k == _empty_k);
  EXPECT_FALSE(_empty_k == _first_shard_k);
  EXPECT_NE(_first_shard_k, _empty_k);
}

TEST(ArenaAllocator, PmrVectorRunsOnArenaResource) {
  arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, 2048, 8, 64}});
  arena_memory_resource_t _resource{_shard};
  std::pmr::vector<std::uint64_t> _values{
      std::pmr::polymorphic_allocator<std::uint64_t>(&_resource)};
  for (std::uint64_t _index = 0; _index < 200; ++_index)
    _values.push_back(_index);
  ASSERT_EQ(_values.size(), 200U);
  for (std::uint64_t _index = 0; _index < 200; ++_index)
    EXPECT_EQ(_values[static_cast<std::size_t>(_index)], _index);
}
} // namespace
} // namespace frameworkcpp::arena
