# Configuration

The arena is tuned through plain structs, and — because it never grows by
surprise — you can decide its whole memory budget at compile time. This page is
the reference for the knobs: the concurrency models, the bucket spec, the arena
settings and the compile-time footprints.

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
`shard.get_bucket(tag, concurrency_model_t)`. The three models are:

```cpp
enum class concurrency_model_t : std::uint8_t {
    single_thread_, // no mutex, no atomics — one thread owns the bucket
    lock_free_,     // lock-free ring with sequence numbers
    thread_safe_,   // std::shared_mutex around the free-list
};
```

`single_thread_` is the fastest; `lock_free_` survives concurrent writers
without locks; `thread_safe_` is the "do not think" option.

## Describing a bucket

```cpp
struct object_spec_t {
    std::uint64_t tag_;                               // identifies the bucket
    std::size_t   object_size_;                       // bytes per slot
    std::size_t   align_ = alignof(std::max_align_t); // slot alignment
    std::size_t   count_;                             // slots to reserve
};
```

`create_shard` rejects a shard when any spec is malformed — `object_size_` or
`count_` zero, or `align_` not a power of two (alignment is capped at 64). A
rejected shard simply returns an empty view, so a typo fails loudly at setup
instead of corrupting memory later.

## Arena settings

```cpp
struct arena_config_t {
    std::size_t    size_        = 1024 * 1024;  // initial region, in bytes
    std::size_t    soft_limit_  = 0;            // warn when reserved bytes reach this
    std::size_t    hard_limit_  = 0;            // hard ceiling; 0 = uncapped
    overflow_mode_t overflow_   = overflow_mode_t::throw_error_; // or abort_
    arena_warning_fn on_warning_ = nullptr;     // callback fired once at soft_limit
    bool           touch_pages_ = false;        // MAP_POPULATE to pre-fault pages
    std::size_t    max_shards_  = max_arena_shards_k;  // shard registry capacity
    std::size_t    max_regions_ = max_arena_regions_k; // mmap segment capacity
};
```

### The registry, without the heap

`arena_t` tracks its shards and `mmap` segments in **fixed arrays sized at
compile time** (`max_arena_shards_k`, `max_arena_regions_k`), so creating a
shard never allocates. `max_shards_` and `max_regions_` just carve a smaller
slice of that fixed registry — raise them, but never above the constants (the
constructor rejects that with `std::invalid_argument`). All the memory your
system touches — buckets, object pools, even the shard bookkeeping — lives in
the arena's own `mmap` segments. There is no heap anywhere in the store.

### The budget policy

- `size_` is the first region. Later shards either fit the existing regions or
  trigger one more `mmap`.
- Both limits are enforced on **reserved bytes** (what the mmap segments
  occupy), not on live objects.
- Crossing `soft_limit_` calls `on_warning_` once and latches
  `arena.is_soft_limit_exceeded()` — allocation keeps working.
- Crossing `hard_limit_` while carving a shard throws `arena_overflow_exception`
  (or calls `abort()`, if `overflow_ == overflow_mode_t::abort_`).
- `soft_limit_` must not exceed `hard_limit_`; the arena constructor rejects it
  with `std::invalid_argument`.
- `touch_pages_ = true` touches all pages up front, trading RSS for
  deterministic first-use latency.

Pick a hard limit for real-time deployments: it makes the "stock" explicit and
turns overshoot into a loud, reproducible failure instead of silent paging.

## Observing a bucket

```cpp
struct bucket_stats_t {
    std::uint64_t tag_;
    std::size_t   object_size_;
    std::size_t   align_;
    std::uint32_t capacity_; // total slots
    std::int64_t  in_use_;   // slots currently in your hands
    std::int64_t  freed_;    // slots returned since the bucket was created
};
```

Read through `arena_shard_t::get_stats(index)`. Counters are read atomically
for the lock-free and thread-safe models, and as plain fields for
single-threaded ones.

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
constexpr object_spec_t workers_k[]  = {{0, 8, 8, 512}};
constexpr object_spec_t controls_k[] = {{0, 256, 64, 32}};

constexpr std::size_t budget_k =
    round_to_page(shard_footprint(workers_k) + shard_footprint(controls_k));

arena_config_t config;
config.size_ = budget_k;
config.hard_limit_ = budget_k; // the stock: never reserve past this
arena_t arena{config};
arena.create_shard(1, {workers_k[0]});
arena.create_shard(2, {controls_k[0]});
// arena.get_capacity() == budget_k — nothing extra was ever mmap'd
```
