// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <gtest/gtest.h>

#include "frameworkcpp/arena/arena.hpp"

namespace {

std::atomic<std::uint64_t> g_allocations{0};

} // namespace

// Detect sanitizer builds without touching __has_feature when the compiler
// does not provide it (GCC would fail on __has_feature(...) as an undefined
// function-like macro).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FRAMEWORKCPP_SANITIZED 1
#elif defined(__clang__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define FRAMEWORKCPP_SANITIZED 1
#endif
#endif
#endif

#if defined(FRAMEWORKCPP_SANITIZED)
namespace frameworkcpp::arena::test {
#else
// Count every heap allocation in this TU so the arena can prove it never
// touches the heap once it is up: create_shard, bucket allocate/deallocate and
// the ptr/allocator helpers must all be heap-free. Disabled under ASan/TSan,
// whose runtimes define their own operator new/delete.
void *operator new(std::size_t size) {
    ++g_allocations;
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook forwards to the real heap
    if (void *p = std::malloc(size); p != nullptr)
        return p;
    throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
    ++g_allocations;
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook forwards to the real heap
    if (void *p = std::malloc(size); p != nullptr)
        return p;
    throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook mirrors operator new
    std::free(p);
}
void operator delete[](void *p) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook mirrors operator new[]
    std::free(p);
}

namespace frameworkcpp::arena::test {
#endif

struct heap_holder {
    std::uint32_t value = 0;
};

TEST(ArenaHeap, NoHeapOnShardCarveAndOps) {
    const auto before = g_allocations.load(std::memory_order_relaxed);

    arena_config_t config;
    config.size = 256 * 1024;
    arena_t arena{config};

    auto shard = arena.create_shard(1,
                                    {{0, 64, alignof(std::max_align_t), 128},
                                     {1, sizeof(heap_holder), alignof(heap_holder), 8}});

    ASSERT_TRUE(static_cast<bool>(shard));

    auto *a = shard.allocate<std::uint32_t>();
    ASSERT_NE(a, nullptr);
    *a = 42;
    shard.deallocate(a);

    auto p = make_arena_unique_ptr<heap_holder>(shard);
    ASSERT_TRUE(static_cast<bool>(p));

    auto s = make_arena_shared_ptr<int>(shard, 7);
    ASSERT_TRUE(static_cast<bool>(s));
    EXPECT_EQ(*s, 7);

    arena_allocator<int> alloc{shard};
    auto *vec_mem = alloc.allocate(3);
    EXPECT_NE(vec_mem, nullptr);
    alloc.deallocate(vec_mem, 3);

    EXPECT_EQ(arena.soft_limit_exceeded(), false);
    EXPECT_EQ(g_allocations.load(std::memory_order_relaxed), before);
}

TEST(ArenaHeap, NoHeapOnMultiRegionGrowth) {
    const auto before = g_allocations.load(std::memory_order_relaxed);

    arena_config_t config;
    config.size = 4096; // tiny first region -> each big shard forces a new mmap
    arena_t arena{config};

    for (std::uint64_t i = 0; i < 16; ++i) {
        auto shard = arena.create_shard(i, {{0, 256, alignof(std::uint32_t), 256}});
        ASSERT_TRUE(static_cast<bool>(shard));
    }

    EXPECT_EQ(arena.shard_count(), 16U);
    EXPECT_GT(arena.capacity(), config.size);
    EXPECT_EQ(g_allocations.load(std::memory_order_relaxed), before);
}

} // namespace frameworkcpp::arena::test