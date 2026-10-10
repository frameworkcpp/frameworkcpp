# The Arena

`arena_t` is the **owner of every byte**. It holds the reserved memory, carves
shards out of it, and knows how much of your budget you have used.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Configuring an arena

You build an arena from an `arena_config_t`. Every field is optional; the
defaults are sensible for development, and you can tighten them for real-time:

```cpp
arena_config_t config;

config.size        = 1024 * 1024;  // initial region, in bytes
config.soft_limit  = 0;            // warn when reserved bytes reach this (0 = off)
config.hard_limit  = 0;            // hard ceiling on reserved bytes (0 = uncapped)
config.overflow    = overflow_mode::throw_error; // throw, or abort() instead
config.on_warning  = nullptr;      // callback fired once when soft_limit is crossed
config.touch_pages = false;        // MAP_POPULATE: commit pages up front

arena_t arena{config};
```

- `size` is the first region. The arena may reserve more later, up to
  `hard_limit`.
- With `hard_limit = 0` (the default) the arena grows without a ceiling —
  handy in development, not what you want for a hard deadline.
- `soft_limit` and `hard_limit` are enforced on **reserved bytes** (what the
  mmap segments occupy), not on live objects.
- Crossing the hard limit while carving a shard throws `arena_overflow_error`
  (or aborts, if `overflow == overflow_mode::abort`). Crossing the soft limit
  calls `on_warning` once and latches `arena.soft_limit_exceeded()`.

## Carving shards

```cpp
arena_shard_t create_shard(std::uint64_t tag, std::initializer_list<object_spec_t> specs);
arena_shard_t create_shard(std::uint64_t tag, std::span<const object_spec_t> specs);
```

- `tag` is the shard's identity. Asking for a tag you already created returns
  the same shard again (idempotent). A tag is just a `std::uint64_t` — use an
  enum or named constants so your code reads clearly.
- `specs` are the **buckets** the shard will hold. `create_shard` reserves the
  whole slice up front (headers, free-list metadata and all the slots).
- `from(tag)` fetches an existing shard by tag, or an empty (falsy) view when
  it does not exist.

## The arena, not the shard

Everything about allocating belongs to the shard page. The arena itself is
about ownership and budget:

```cpp
arena_t arena;
auto *slot = arena.create_shard(...).allocate<std::uint32_t>();
arena.deallocate(slot);              // works even if you lost track of the shard
```

`arena.deallocate` finds the owning region and bucket from the pointer alone;
use it when you only hold a raw pointer. If you still have the shard, its
`deallocate` is faster.

## Knowing the budget

```cpp
std::size_t shard_count() const noexcept; // how many shards have been carved
std::size_t capacity()      const noexcept; // total bytes reserved (all regions)
std::size_t used()          const noexcept; // bytes carved so far (high-water mark)
std::size_t live()          const noexcept; // slots currently in your hands
std::size_t hard_limit()    const noexcept;
std::size_t soft_limit()    const noexcept;
bool        soft_limit_exceeded() const noexcept;
```

## Example

```cpp
using namespace frameworkcpp::arena;

// Tag the shard and the buckets so the numbers mean something.
enum class shard_tag : std::uint64_t { workers = 1 };
enum class bucket_tag : std::uint64_t { command = 0, counter = 1 };

arena_config_t config;
config.hard_limit = 4 * 1024 * 1024;   // the stock: never reserve past 4 MiB
arena_t arena{config};

auto workers = arena.create_shard(
    static_cast<std::uint64_t>(shard_tag::workers),
    {
        {static_cast<std::uint64_t>(bucket_tag::command),
         sizeof(std::uint32_t),
         alignof(std::uint32_t),
         512},
        {static_cast<std::uint64_t>(bucket_tag::counter),
         sizeof(std::uint64_t),
         alignof(std::uint64_t),
         512},
    });

std::cout << "reserved: " << arena.capacity() << " bytes\n";
```