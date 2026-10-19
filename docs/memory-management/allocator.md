# Allocators & std::pmr

The arena talks the standard allocator language, so it can power `std::vector`,
`std::pmr::vector` and anything else that honours the C++20 allocator contract —
all backed by arena memory.

```cpp
#include <frameworkcpp/arena/arena_allocator.hpp>   // arena_allocator<T> + arena_memory_resource
#include <frameworkcpp/arena/arena.hpp>             // to build the arena in the examples
```

## The standard allocator

`arena_allocator<T>` satisfies `std::allocator_traits`:

```cpp
arena_allocator<int> alloc{shard};
std::vector<int, arena_allocator<int>> values(alloc);
for (int i = 0; i < 100; ++i) values.push_back(i);
```

- Two shard-backed allocators are equal when they point at the same shard; two
  default (empty) allocators are equal to each other; an empty allocator is
  never equal to a shard-backed one.
- The standard propagation traits are all `false_type` and
  `is_always_equal` is `false_type` (allocators over different shards are not
  interchangeable), so container copy/move/swap never smuggle a different
  shard in silently.
- Allocations are served by the shard's smallest-fit bucket, so give the shard
  a bucket big enough for the largest contiguous block the container will ever
  ask for — say 1 KiB for a growing `std::vector<int>`.
- Exhaustion surfaces as `std::bad_alloc`, the usual STL contract.

## PMR

`arena_memory_resource` is a `std::pmr::memory_resource` over a shard:

```cpp
arena_memory_resource resource{shard};
std::pmr::vector<std::uint64_t> values{
    std::pmr::polymorphic_allocator<std::uint64_t>(&resource)};
for (std::uint64_t i = 0; i < 200; ++i) values.push_back(i);
```

PMR `do_is_equal` compares shard identity, so resources over the same shard are
interchangeable.

## What to remember

- A bucket block is a single fixed-size allocation. A growing container needs a
  block big enough for its peak, or allocation throws `std::bad_alloc`.
- Blocks are reused in place: keep `object_size` large enough for the biggest
  data you will write into any single block.