# Configuration

The store is sized up front. You hand `init` an `environment_config_t` and it
carves a shard with exactly those capacities — there is nothing to grow and
nothing to `malloc` afterwards. This page explains each knob and how to derive
the arena budget from them.

```cpp
#include <frameworkcpp/environment/environment.hpp>

struct environment_config_t {
    std::size_t max_keys_        = 256;   // distinct keys
    std::size_t max_key_len_     = 256;   // longest key, in bytes
    std::size_t max_value_len_   = 4096;  // longest value, in bytes
    std::size_t value_cells_     = 1024;  // recyclable value cells (live + in-flight)
    std::size_t journal_size_    = 256;   // delta ring length (power of two)
    std::size_t max_threads_     = 64;    // concurrent per-thread views
    bool        read_process_env_ = true; // inherit the process environment
    const char *dotenv_path_       = nullptr; // optional .env file to load
};
```

| Field | What it bounds | Notes |
| --- | --- | --- |
| `max_keys_` | Number of distinct keys. | The whole process environment counts; a host easily has 50–100 vars. |
| `max_key_len_` | Longest key, in bytes. | `set_env` rejects longer keys. |
| `max_value_len_` | Longest value, in bytes. | `set_env` rejects longer values; `PATH` can be long. |
| `value_cells_` | Value cells in the pool. | Values are recycled, so this is memory, not a write count. |
| `journal_size_` | Updates in the delta ring. | A reader this far behind reseeds instead of replaying. |
| `max_threads_` | Per-thread views. | Extra threads still read, just without a private cache. |
| `read_process_env_` | Whether `init` inherits the process environment. | Set `false` for a file-only, deterministic store. |
| `dotenv_path_` | The `.env` file to load at `init`. | `nullptr` means "process environment only". |

All fields have sensible defaults, so `init(arena)` works with no config at all:
the store then holds the process environment and nothing else.

## Unbounded updates, bounded memory

Values are immutable and recycled. A `set_env` retires the previous cell for the
key; the cell is reused once every live view has moved past the update that
retired it. So the number of writes is **not** a budget — only the memory is:

- `value_cells_` must hold the keys you have plus the cells retired but not yet
  recycled. In steady state that is small: a handful of updates in flight.
- `journal_size_` is how far behind a reader may drift before it reseeds from
  the global slots instead of replaying the delta ring.

A writer only fails when the pool is momentarily exhausted — which happens when
a live view stops advancing and pins every retired cell. That is the bounded
back-pressure, and it clears as soon as the lagging view reads again or its
thread exits.

## Sizing the arena

`get_environment_footprint(config)` returns the exact number of bytes the
module's shard will occupy, and it is `constexpr` — you can derive the budget at
compile time, exactly like the arena's own `shard_footprint` / `round_to_page`:

```cpp
constexpr environment::environment_config_t env_k{};

constexpr std::size_t env_bytes_k = environment::get_environment_footprint(env_k);
constexpr std::size_t arena_bytes_k = environment::get_environment_arena_bytes(env_k, 64 * 1024);

static_assert(arena_bytes_k % 4096 == 0);   // page aligned
static_assert(arena_bytes_k >= env_bytes_k);
```

Or get a ready-made `arena_config_t`:

```cpp
arena::arena_t arena{environment::get_environment_arena_config(env_k)};
environment::init(arena, env_k);
```

`get_environment_arena_bytes(config, extra)` is
`round_to_page(footprint + extra)`, so pass any headroom you want for other
shards as `extra`.

## Suggested configurations

**Development** — accept the defaults:

```cpp
environment_config_t config;
config.dotenv_path_ = ".env";
environment::init(arena, config);
```

**Production, file driven** — ignore the host environment and be explicit:

```cpp
environment_config_t config;
config.read_process_env_ = false;
config.max_keys_ = 128;
config.value_cells_ = 256;
config.dotenv_path_ = "/etc/myapp/app.env";
environment::init(arena, config);
```

**Many short-lived threads** — raise the view pool so each worker keeps its
cache:

```cpp
environment_config_t config;
config.max_threads_ = 512;
config.dotenv_path_ = "app.env";
environment::init(arena, config);
```

## Observing the store

`set_env` reports success rather than throwing, and `get_environment_stats()`
shows the live picture — including how many retired cells are still pinned:

```cpp
struct environment_stats_t {
    std::size_t   keys_used_;        // distinct keys present
    std::size_t   max_keys_;
    std::uint64_t total_updates_;    // writes committed so far (monotonic)
    std::size_t   journal_size_;
    std::size_t   retired_pending_;  // retired cells waiting to be recycled
    std::size_t   max_threads_;      // per-thread view pool size
    std::size_t   active_views_;     // views currently claimed by live threads
};

auto stats = environment::get_environment_stats();
if (stats.retired_pending_ + 16 > config.value_cells_) {
    // a view is lagging; likely a thread that stopped reading
}
```

A healthy store keeps `retired_pending_` near zero. A number that climbs and
stays high means some live view is not advancing (a thread that reads once and
then idles); once that thread reads again or exits, the reclaimer drains it.

## If a write ever fails

`set_env` returns `false` only for bad input, a full key space, or a value pool
momentarily pinned by lagging readers. Values are recycled, so this is not a
lifetime budget — it is transient back-pressure that clears when the lagging
views move on. There is no need to reload the module; if a long-lived thread
reads rarely, either let it read periodically or give the pool more cells.
