# Buckets

A **bucket** is a run of same-size blocks inside a shard. If shards give you
isolation, buckets give you speed: all blocks in a bucket share one size, one
alignment and one free-list, so allocation is a pop and a store.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Holding a bucket

```cpp
arena_bucket writer = shard.create_writer(tag, concurrency_model);
arena_bucket reader = shard.create_reader(tag, concurrency_model);
```

- The **first** call for a tag fixes that bucket's concurrency model.
  Requesting a different model afterwards aborts the program.
- The handle is empty (falsy) if the tag does not exist in the shard.

## Using it

```cpp
template <typename T>
T* allocate(std::size_t count = 1);    // nullptr if it does not fit this bucket

template <typename T>
void deallocate(T* p) noexcept;
void deallocate_bytes(void* p) noexcept;

std::uint64_t tag() const noexcept;
explicit operator bool() const noexcept;
```

A bucket handle only ever hands out memory from its own run, so freed addresses
are reused within the bucket. Allocation fails with `nullptr` when the request
is bigger than `object_size` or more aligned than the bucket allows.

## Example

```cpp
using namespace frameworkcpp::arena;

arena_t arena;
auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 1024}});

auto writer = shard.create_writer(0, concurrency_model::lock_free);
auto reader = shard.create_reader(0, concurrency_model::lock_free);

auto* slot = writer.allocate<std::uint64_t>();
if (slot) *slot = 7;
writer.deallocate(slot);   // reader could free it just the same
```