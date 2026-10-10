# Configuration

The arena is tuned through plain structs, and its memory budget can be decided
at compile time.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

::: tip Vocabulary
A **slot** is one block of memory. A **bucket** is a pool of same-size slots
inside a **shard**, which is a slice of an **arena**. See
[the overview](index#the-vocabulary) if this is your first read.
:::

## Concurrency models

Concurrency is chosen per bucket, when you grab it with
`shard.get_bucket(tag, concurrency_model)`.

```cpp
enum class concurrency_model : std::uint8_t {
    single_thread, // no mutex, no atomics — one thread owns the bucket
    lock_free,     // lock-free ring with sequence numbers
    thread_safe,   // std::shared_mutex around the free-list
};
```

`single_thread` is the fastest; `lock_free` survives concurrent writers without
locks; `thread_safe` is the "do not think" option.

## Describing a bucket

```cpp
struct object_spec_t {
    std::uint64_t tag;                             // identifies the bucket
    std::size_t   object_size;                     // bytes per slot
    std::size_t   align  = alignof(std::max_align_t); // slot alignment
    std::size_t   count;                           // slots to reserve
};
```

`create_shard` rejects a shard when any spec is malformed — `object_size` or
`count` zero, or `align` not a power of two (alignment is capped at 64). A
rejected shard simply returns an empty view.

## Arena settings

```cpp
struct arena_config_t {
    std::size_t    size        = 1024 * 1024; // initial region, in bytes
    std::size_t    soft_limit  = 0;        // warn when reserved bytes reach this
    std::size_t    hard_limit  = 0;        // hard ceiling; 0 = uncapped
    overflow_mode  overflow    = overflow_mode::throw_error; // or abort
    arena_warning_fn on_warning = nullptr; // callback fired once at soft_limit
    bool           touch_pages = false;    // MAP_POPULATE to pre-fault pages
};
```

### The budget policy

- `size` is the first region. Later shards either fit the existing regions or
  trigger one more `mmap`.
- Both limits are enforced on **reserved bytes** (what the mmap segments
  occupy), not on live objects.
- Crossing `soft_limit` calls `on_warning` once and latches
  `arena.soft_limit_exceeded()` — allocation keeps working.
- Crossing `hard_limit` while carving a shard throws `arena_overflow_error`
  (or calls `abort()`, if `overflow == overflow_mode::abort`).
- `soft_limit` must not exceed `hard_limit`; the arena constructor rejects it
  with `std::invalid_argument`.
- `touch_pages = true` touches all pages up front, trading RSS for
  deterministic first-use latency.

Pick a hard limit for real-time deployments: it makes the "stock" explicit and
turns overshoot into a loud, reproducible failure instead of silent paging.

## Observing a bucket

```cpp
struct bucket_stats_t {
    std::uint64_t tag;
    std::size_t   object_size;
    std::size_t   align;
    std::uint32_t capacity; // total slots
    std::int64_t  in_use;   // slots currently in your hands
    std::int64_t  freed;    // slots returned since the bucket was created
};
```

Read through `arena_shard_t::stats(index)`. Counters are read atomically for
the lock-free and thread-safe models, and as plain fields for single-threaded
ones.

## Sizing the arena at compile time

The arena can compute what it will need before it is even built:

```cpp
template <std::size_t N>
constexpr std::size_t shard_footprint(const object_spec_t (&specs)[N]);

constexpr std::size_t round_to_page(std::size_t bytes);
```

Sum the footprints of all your shards, round up to a page, and that is your
budget:

```cpp
constexpr object_spec_t workers[]  = {{0, 8, 8, 512}};
constexpr object_spec_t controls[] = {{0, 256, 64, 32}};

constexpr std::size_t budget =
    round_to_page(shard_footprint(workers) + shard_footprint(controls));

arena_config_t config;
config.size = budget;
config.hard_limit = budget; // the stock: never reserve past this
arena_t arena{config};
arena.create_shard(1, {workers[0]});
arena.create_shard(2, {controls[0]});
// arena.capacity() == budget — nothing extra was ever mmap'd
```