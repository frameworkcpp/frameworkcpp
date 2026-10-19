# Configuration

The Environment Management module is sized up front. You hand `init` an
`environment_config_t` and it carves a shard with exactly those capacities —
there is nothing to grow and nothing to `malloc` afterwards.

```cpp
#include <frameworkcpp/environment/environment.hpp>

struct environment_config_t {
    std::size_t max_keys        = 256;   // distinct keys
    std::size_t max_key_len     = 256;   // longest key, in bytes
    std::size_t max_value_len   = 4096;  // longest value, in bytes
    std::size_t value_cells     = 1024;  // recyclable value cells (live + in-flight)
    std::size_t journal_size    = 256;   // delta ring length (power of two)
    std::size_t max_threads     = 64;    // concurrent per-thread views
    bool        read_process_env = true; // inherit the process environment
};
```

| Field | What it bounds | Notes |
| --- | --- | --- |
| `max_keys` | Number of distinct keys. | The whole process environment counts; a host easily has 50–100 vars. |
| `max_key_len` | Longest key, in bytes. | `set_env` rejects longer keys. |
| `max_value_len` | Longest value, in bytes. | `set_env` rejects longer values; `PATH` can be long. |
| `value_cells` | Value cells in the pool. | Values are recycled, so this is memory, not a write count. |
| `journal_size` | Updates in the delta ring. | A reader this far behind reseeds instead of replaying. |
| `max_threads` | Per-thread views. | Extra threads still read, just without a private cache. |
| `read_process_env` | Whether `init` inherits the process environment. | Set `false` for a file-only, deterministic store. |

All fields have sensible defaults, so `init(arena, path)` works with no config
at all.

## Unbounded updates, bounded memory

Values are immutable and recycled. A `set_env` retires the previous cell for the
key; the cell is reused once every live view has moved past the update that
retired it. So the number of writes is **not** a budget — only the memory is:

- `value_cells` must hold the keys you have plus the cells retired but not yet
  recycled. In steady state that is small: a handful of updates in flight.
- `journal_size` is how far behind a reader may drift before it reseeds from the
  global slots instead of replaying the delta ring.

A writer only fails when the pool is momentarily exhausted — which happens when
a live view stops advancing and pins every retired cell. That is the bounded
back-pressure, and it clears as soon as the lagging view reads again or its
thread exits.

## Sizing the arena

`environment_footprint(config)` returns the exact number of bytes the module's
shard will occupy, and it is `constexpr` — you can derive the budget at compile
time, exactly like the arena's own `shard_footprint` / `round_to_page`:

```cpp
constexpr environment::environment_config_t kEnv{};

constexpr std::size_t kEnvBytes = environment::environment_footprint(kEnv);
constexpr std::size_t kArenaBytes = environment::environment_arena_bytes(kEnv, 64 * 1024);

static_assert(kArenaBytes % 4096 == 0);   // page aligned
static_assert(kArenaBytes >= kEnvBytes);
```

Or get a ready-made `arena_config_t`:

```cpp
arena::arena_t arena{environment::environment_arena_config(kEnv)};
environment::init(arena, "app.env", kEnv);
```

`environment_arena_bytes(config, extra)` is `round_to_page(footprint + extra)`,
so pass any headroom you want for other shards as `extra`.

## Suggested configurations

**Development** — accept the defaults:

```cpp
environment::init(arena, ".env");
```

**Production, file driven** — ignore the host environment and be explicit:

```cpp
environment_config_t config;
config.read_process_env = false;
config.max_keys = 128;
config.value_cells = 256;
environment::init(arena, "/etc/myapp/app.env", config);
```

**Many short-lived threads** — raise the view pool so each worker keeps its
cache:

```cpp
environment_config_t config;
config.max_threads = 512;
environment::init(arena, "app.env", config);
```

## Observing the store

`set_env` reports success rather than throwing, and `environment_stats()` shows
the live picture — including how many retired cells are still pinned:

```cpp
struct environment_stats_t {
    std::size_t   keys_used;        // distinct keys present
    std::size_t   max_keys;
    std::uint64_t total_updates;    // writes committed so far (monotonic)
    std::size_t   journal_size;
    std::size_t   retired_pending;  // retired cells waiting to be recycled
    std::size_t   max_threads;      // per-thread view pool size
    std::size_t   active_views;     // views currently claimed by live threads
};

auto stats = environment::environment_stats();
if (stats.retired_pending + 16 > config.value_cells) {
    // a view is lagging; likely a thread that stopped reading
}
```

A healthy store keeps `retired_pending` near zero. A number that climbs and
stays high means some live view is not advancing (a thread that reads once and
then idles); once that thread reads again or exits, the reclaimer drains it.

## If a write ever fails

`set_env` returns `false` only for bad input, a full key space, or a value pool
momentarily pinned by lagging readers. Values are recycled, so this is not a
lifetime budget — it is transient back-pressure that clears when the lagging
views move on. There is no need to reload the module; if a long-lived thread
reads rarely, either let it read periodically or give the pool more cells.
