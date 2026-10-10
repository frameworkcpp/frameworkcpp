# Configuration

The arena is tuned through plain structs, and its memory budget can be decided
at compile time.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Concurrency models

Concurrency is chosen per bucket, when you create its writer or reader handle.

```cpp
enum class concurrency_model : std::uint32_t
{
    single_thread, // no mutex, no atomics — one writer owns the bucket
    lock_free,     // lock-free ring with sequence numbers
    thread_safe,   // std::shared_mutex around the free-list
};
```

`single_thread` is the fastest; `lock_free` survives concurrent writers without
locks; `thread_safe` is the "do not think" option.

## Describing a bucket

```cpp
struct object_spec
{
    std::uint64_t tag;                             // identifies the bucket
    std::size_t   object_size;                     // payload bytes per block
    std::size_t   align  = alignof(std::max_align_t); // payload alignment
    std::size_t   count;                           // how many blocks to reserve
};
```

`create_shard` rejects a shard when any spec is malformed — `object_size` or
`count` zero, or `align` not a power of two (alignment is capped at 64). A
rejected shard simply returns an empty view.

## Arena settings

```cpp
struct arena_config
{
    std::size_t size        = 1024 * 1024; // initial region, in bytes
    bool        touch_pages = false;       // MAP_POPULATE to pre-fault pages
};
```

`touch_pages = true` touches all pages up front, trading RSS for deterministic
first-use latency. The initial region is `max(size, required_bytes)` rounded up
to a page; anything after that gets one more `mmap` region.

## Observing a bucket

```cpp
struct bucket_stats
{
    std::uint64_t tag;
    std::size_t   object_size;
    std::size_t   align;
    std::uint32_t capacity; // total blocks
    std::int64_t  in_use;   // blocks currently in your hands
    std::int64_t  freed;    // blocks returned since the bucket was created
};
```

Read through `arena_shard_t::stats(index)`. Counters are read atomically for
the lock-free and thread-safe models, and as plain fields for single-threaded
ones.

## Sizing the arena at compile time

The arena can compute what it will need before it is even built:

```cpp
template <std::size_t N>
constexpr std::size_t shard_footprint(const object_spec (&specs)[N]);

constexpr std::size_t round_to_page(std::size_t bytes);
```

Sum the footprints of all your shards, round up to a page, and that is your
budget:

```cpp
constexpr object_spec workers[]  = {{0, 8, 8, 512}};
constexpr object_spec controls[] = {{0, 256, 64, 32}};

constexpr std::size_t budget =
    round_to_page(shard_footprint(workers) + shard_footprint(controls));

arena_config cfg;
cfg.size = budget;
arena_t arena{cfg};
arena.create_shard(1, {workers[0]});
arena.create_shard(2, {controls[0]});
// arena.capacity() == budget — nothing extra was ever mmap'd
```