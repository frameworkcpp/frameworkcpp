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
shard will occupy. Add it your arena budget:

```cpp
environment::environment_config_t config;
arena::arena_config_t arena_config;
arena_config.size = environment::environment_footprint(config) + (64 * 1024);
arena::arena_t arena{arena_config};
environment::init(arena, "app.env", config);
```

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
