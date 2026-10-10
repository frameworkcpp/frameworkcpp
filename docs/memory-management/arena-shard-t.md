# Shards

A shard is a private slice of the arena's memory. Through its view
(`arena_shard_t`) you allocate blocks, free them back, and reach the buckets
that live inside it.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Allocating

```cpp
template <typename T>
T* allocate(std::size_t count = 1);   // raw, uninitialized memory

std::byte* allocate_bytes(std::size_t bytes, std::size_t alignment);
std::byte* allocate_tagged(std::uint64_t tag, std::size_t bytes, std::size_t alignment);
```

- `allocate<T>(count)` picks the **smallest bucket that fits**: it needs
  `object_size >= count * sizeof(T)` and enough alignment for `T`. If nothing
  fits, it returns `nullptr`.
- Allocation does **not** run a constructor — you get raw memory, ready for
  placement-new or a plain store.
- `allocate_tagged` addresses a specific bucket by its tag when you want to
  bypass the smallest-fit search.

## Freeing

```cpp
template <typename T>
void deallocate(T* p) noexcept;
void deallocate_bytes(void* p) noexcept;
```

The block header lives right before the payload, so the shard knows instantly
which bucket a pointer belongs to and pushes it back onto that bucket's
free-list. Pointers from other shards are forwarded to the arena, which
verifies ownership; pointers from nowhere are ignored.

## Handles and diagnostics

```cpp
arena_bucket create_writer(std::uint64_t tag, concurrency_model model);
arena_bucket create_reader(std::uint64_t tag, concurrency_model model);

std::uint32_t bucket_count() const noexcept;
bucket_stats   stats(std::uint32_t index) const noexcept;  // in_use / freed
std::uint64_t  tag() const noexcept;
std::uint32_t  shard_id() const noexcept;
explicit operator bool() const noexcept; // false = the shard does not exist
```

`create_writer` and `create_reader` hand you a handle to one specific bucket,
fixing its [concurrency model](index#concurrency). Both names return the same
kind of handle; they exist so the call site says what it intends.

## Example

```cpp
using namespace frameworkcpp::arena;

arena_t arena;
auto shard = arena.create_shard(7, {{0, sizeof(std::uint32_t), alignof(std::uint32_t), 256}});

auto* slot = shard.allocate<std::uint32_t>();
*slot = 42;

shard.deallocate(slot);                     // back to the bucket, reusable
std::cout << shard.stats(0).freed << "\n";  // 1
```