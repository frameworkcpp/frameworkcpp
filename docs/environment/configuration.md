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
    std::size_t max_updates     = 256;   // total set_env calls
    std::size_t max_threads     = 64;    // concurrent per-thread views
    bool        read_process_env = true; // inherit the process environment
};
```

| Field | What it bounds | Notes |
| --- | --- | --- |
| `max_keys` | Number of distinct keys. | The whole process environment counts; a host easily has 50–100 vars. |
| `max_key_len` | Longest key, in bytes. | `set_env` rejects longer keys. |
| `max_value_len` | Longest value, in bytes. | `set_env` rejects longer values; `PATH` can be long. |
| `max_updates` | Total `set_env` calls for the store's lifetime. | Each write consumes one immutable value cell — it is a budget, not a window. |
| `max_threads` | Per-thread views. | Extra threads still read, just without a private cache. |
| `read_process_env` | Whether `init` inherits the process environment. | Set `false` for a file-only, deterministic store. |

All fields have sensible defaults, so `init(arena, path)` works with no config
at all. Tighten them for real-time: known, fixed capacities turn an eventual
overflow into a loud `false` from `set_env` instead of a surprise.

## Update budget, not a cache

Because values are immutable and never reclaimed, every `set_env` permanently
consumes one cell. `max_updates` is therefore the **number of writes the store
will ever accept**:

```cpp
environment_config_t config;
config.max_updates = 4096;   // room for 4096 publishes over the process lifetime
```

If your workload writes configuration indefinitely, either raise the budget to
cover the process lifetime or reload the module (a fresh `init` after
`shutdown`) when you want a new window.

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
config.max_keys   = 128;
config.max_updates = 1024;
environment::init(arena, "/etc/myapp/app.env", config);
```

**Many short-lived threads** — raise the view pool so each worker keeps its
cache:

```cpp
environment_config_t config;
config.max_threads = 512;
environment::init(arena, "app.env", config);
```

## Observing the budget

The store reports success rather than throwing, so the cheapest way to know you
sized it right is to check the return of your writes:

```cpp
if (!environment::set_env("TUNING", value)) {
    // max_updates, max_keys or a length limit was reached
}
```

## When the updates fill up

`max_updates` is a lifetime budget: every `set_env` publishes a new immutable
cell and nothing is ever reclaimed, so once the budget is spent, `set_env`
returns `false` — loudly and reproducibly, never by allocating behind your back.
`environment_stats()` tells you how close you are *before* that happens:

```cpp
struct environment_stats_t {
    std::size_t keys_used;
    std::size_t max_keys;
    std::size_t updates_used;
    std::size_t max_updates;
    std::size_t key_room()    const;   // max_keys - keys_used
    std::size_t update_room() const;   // max_updates - updates_used
};

auto stats = environment::environment_stats();
if (stats.update_room() < 16) {
    // plan a reload, or log a warning, before writes start failing
}
```

When the budget is genuinely full you have three options:

1. **Size for the lifetime.** Environment variables change rarely — a config
   reload here and there — so `max_updates` in the thousands is usually
   permanent. Because the budget is `constexpr`, you pay for it once in the
   arena size and never think about it again.
2. **Reload at a quiescent point.** `shutdown()` then `init()` with a fresh
   arena gives you a brand-new store. Do it only when no reader or writer is
   running (`init`/`shutdown` are single-threaded), the same discipline as
   startup. The old shard is returned when its arena is destroyed.
3. **Fail loudly.** Treat `set_env == false` as back-pressure: log it, keep the
   previous value and retry on the next reload window.

What you cannot do — by design — is reclaim a cell while a reader might still
hold it. Values are immutable and never edited in place, so a cheap read never
needs a lock. Reclaiming them safely would require epoch-based reclamation
(RCU), which is deliberately left out of this version; the fixed budget is the
price of the lock-free, heap-free read path.
