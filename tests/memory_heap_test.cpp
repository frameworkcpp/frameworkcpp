// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <gtest/gtest.h>

#include "frameworkcpp/memory/memory.hpp"

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
namespace frameworkcpp::memory::test {
#else
// Count every heap allocation in this TU so the memory can prove it never
// touches the heap once it is up: create_shard, bucket allocate/deallocate and
// the ptr/allocator helpers must all be heap-free. Disabled under ASan/TSan,
// whose runtimes define their own operator new/delete.
void *operator new(std::size_t size) {
  ++g_allocations;
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook forwards to the real heap
  if (void *_pointer = std::malloc(size); _pointer != nullptr)
    return _pointer;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  ++g_allocations;
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook forwards to the real heap
  if (void *_pointer = std::malloc(size); _pointer != nullptr)
    return _pointer;
  throw std::bad_alloc();
}

void operator delete(void *pointer) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook mirrors operator new
  std::free(pointer);
}
void operator delete[](void *pointer) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook mirrors operator new[]
  std::free(pointer);
}
void operator delete(void *pointer, [[maybe_unused]] std::size_t size) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook mirrors operator new
  std::free(pointer);
}
void operator delete[](void *pointer, [[maybe_unused]] std::size_t size) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) -- counting hook mirrors operator new[]
  std::free(pointer);
}

namespace frameworkcpp::memory::test {
#endif

struct heap_holder_t {
  std::uint32_t value_ = 0;
}; // struct heap_holder_t

TEST(MemoryHeap, NoHeapOnShardCarveAndOps) {
  const auto _before_k = g_allocations.load(std::memory_order_relaxed);

  memory_config_t _config;
  _config.size_ = 256 * 1024;
  memory_t _memory{_config};

  auto _shard = _memory.create_shard(1,
                                     {{0, 64, alignof(std::max_align_t), 128},
                                      {1, sizeof(heap_holder_t), alignof(heap_holder_t), 8}});

  ASSERT_TRUE(static_cast<bool>(_shard));

  auto *_pointer = _shard.allocate<std::uint32_t>();
  ASSERT_NE(_pointer, nullptr);
  *_pointer = 42;
  _shard.deallocate(_pointer);

  auto _unique = make_memory_unique_ptr<heap_holder_t>(_shard);
  ASSERT_TRUE(static_cast<bool>(_unique));

  auto _shared = make_memory_shared_ptr<int>(_shard, 7);
  ASSERT_TRUE(static_cast<bool>(_shared));
  EXPECT_EQ(*_shared, 7);

  memory_allocator_t<int> _allocator{_shard};
  auto *_vector_memory = _allocator.allocate(3);
  EXPECT_NE(_vector_memory, nullptr);
  _allocator.deallocate(_vector_memory, 3);

  EXPECT_EQ(_memory.is_soft_limit_exceeded(), false);
  EXPECT_EQ(g_allocations.load(std::memory_order_relaxed), _before_k);
}

TEST(MemoryHeap, NoHeapOnMultiRegionGrowth) {
  const auto _before_k = g_allocations.load(std::memory_order_relaxed);

  memory_config_t _config;
  _config.size_ = 4096; // tiny first region -> each big shard forces a new mmap
  memory_t _memory{_config};

  for (std::uint64_t _index = 0; _index < 16; ++_index) {
    auto _shard = _memory.create_shard(_index, {{0, 256, alignof(std::uint32_t), 256}});
    ASSERT_TRUE(static_cast<bool>(_shard));
  }

  EXPECT_EQ(_memory.get_shard_count(), 16U);
  EXPECT_GT(_memory.get_capacity(), _config.size_);
  EXPECT_EQ(g_allocations.load(std::memory_order_relaxed), _before_k);
}

} // namespace frameworkcpp::memory::test
