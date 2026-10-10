# Buckets

A **bucket** is a pool of same-size slots inside a shard. Shards give you
isolation; buckets give you speed — every slot in a bucket shares one size,
one alignment and one free-list, so allocation is a pop and a store.

A bucket is never created on its own. It is declared when you create the
shard, as one `object_spec_t` entry, and you later grab a handle to it with
`shard.get_bucket(tag, concurrency_model)`.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Declaring a bucket

A bucket lives inside a shard's `create_shard` call. Each `object_spec_t`
entry becomes one bucket:

```cpp
enum class bucket_tag : std::uint64_t { commands = 0, payloads = 1 };

auto shard = arena.create_shard(
    1, // shard tag
    {
        // { tag,               object_size,        align, count }
        {static_cast<std::uint64_t>(bucket_tag::commands), sizeof(std::uint64_t),
         alignof(std::uint64_t), 1024}, // 1024 slots of 8 bytes
        {static_cast<std::uint64_t>(bucket_tag::payloads), 256, 64, 64},
        //                           256 slots of 256 bytes, aligned to 64
    });
```

## Holding a bucket

```cpp
arena_bucket_t get_bucket(std::uint64_t tag, concurrency_model model);
```

- Pass the **bucket tag** you declared, and the **concurrency model** this
  bucket will use.
- The **first** call for a tag fixes the bucket's model. Requesting a different
  model afterwards aborts the program.
- The handle is empty (falsy) if no bucket with that tag exists in the shard.

## Using it

```cpp
template <typename T>
T* allocate(std::size_t count = 1);    // nullptr if the slot is too small

template <typename T>
void deallocate(T* p) noexcept;
void deallocate_bytes(void* p) noexcept;

std::uint64_t tag() const noexcept;
explicit operator bool() const noexcept;
```

A bucket handle only hands out slots from its own pool — it never goes hunting
through the shard. Freed slots are reused within the bucket. Allocation fails
with `nullptr` when the request is bigger than the bucket's `object_size` or
more aligned than the bucket allows.

## Example

```cpp
using namespace frameworkcpp::arena;

arena_t arena;
auto shard = arena.create_shard(
    1,
    {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 1024}}); // one bucket, tag 0

auto bucket = shard.get_bucket(0, concurrency_model::lock_free);

auto* slot = bucket.allocate<std::uint64_t>(); // only ever from bucket 0
if (slot) *slot = 7;
bucket.deallocate(slot);
```