// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>

#include <gtest/gtest.h>

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/environment/environment.hpp"

namespace {

std::atomic<std::uint64_t> g_allocations{0};

} // namespace

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FRAMEWORKCPP_ENV_HEAP_SANITIZED 1
#elif defined(__clang__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define FRAMEWORKCPP_ENV_HEAP_SANITIZED 1
#endif
#endif
#endif

#if !defined(FRAMEWORKCPP_ENV_HEAP_SANITIZED)
// Count every heap allocation in this TU: environment init and its read/write
// paths must be fully arena-backed. Disabled under ASan/TSan, whose runtimes
// own operator new/delete.
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
#endif

namespace frameworkcpp::environment::test {

TEST(EnvironmentHeap, InitAndOpsAreHeapFree) {
  const auto _before_k = g_allocations.load(std::memory_order_relaxed);

  arena::arena_config_t _arena_config;
  _arena_config.size_ = 8 * 1024 * 1024;
  arena::arena_t _arena{_arena_config};

  const environment_config_t _config_k;
  init(_arena, _config_k);

  ASSERT_EQ(get_env("FC_HEAP_MISSING", "fallback"), "fallback");
  ASSERT_TRUE(set_env("FC_HEAP_KEY", "value-one"));
  ASSERT_EQ(get_env("FC_HEAP_KEY", ""), "value-one");
  ASSERT_TRUE(set_env("FC_HEAP_KEY", "value-two"));
  ASSERT_EQ(get_env("FC_HEAP_KEY", ""), "value-two");

  shutdown();
  EXPECT_EQ(g_allocations.load(std::memory_order_relaxed), _before_k);
}

} // namespace frameworkcpp::environment::test
