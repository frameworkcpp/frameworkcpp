# Environment

Software is configured through the environment: what the process inherited when
it started, and what a `.env` file declares. `frameworkcpp::environment` is a
dotenv-style store for exactly that, built on the arena. Every key and value
lives in a single arena shard, reads never touch the heap or take a lock, and
values are immutable once published — a write publishes a new cell instead of
editing the old one.

## The vocabulary

| Concept | What it is |
| --- | --- |
| **Store** | The module's arena shard: one key table, one value pool, one journal, one view per thread. |
| **Key** | A name (up to `max_key_len` bytes) that maps to the latest published value. |
| **Value** | An immutable cell holding the value bytes. `set_env` always builds a fresh cell. |
| **View** | A per-thread cache of value pointers plus the version that cache is current to. |
| **Journal** | The append-only list of updates a behind thread replays to catch up. |

## The mental model

1. You build an arena and call `init(arena, "my.env")`. It carves a shard and
   loads the configuration: first the process environment, then the file — and
   the **process wins**, the file only fills keys that are still unset.
2. `get_env(key, fallback)` reads. A warmed-up thread compares its cached
   version against the global counter and, if they match, reads its own cache —
   no shared atomic beyond that one counter, no lock, no heap.
3. `set_env(key, value)` publishes a new immutable value cell, appends the
   update to the journal and bumps the global version. Writers serialize on a
   short mutex; readers never take it.

```cpp
#include <frameworkcpp/arena/arena.hpp>
#include <frameworkcpp/environment/environment.hpp>

using namespace frameworkcpp;

int main()
{
    arena::arena_t arena{arena::arena_config_t{}};
    environment::init(arena, "app.env");     // process env, then the file

    environment::get_env("LOG_LEVEL", "info"); // read (never allocates)
    environment::set_env("LOG_LEVEL", "debug"); // publish a new value

    environment::shutdown();                   // drop the module handle
}
```

## The version trick

The interesting part is how a multithreaded read stays cheap. Each thread keeps
a private cache and a version number. A read compares that version with a single
global counter:

- **same version** → read straight from the private cache;
- **behind** → replay only the journal entries the thread missed, then update
  the cache and its version. It never clones the whole map.

Because published cells are immutable, replay is race-free: a cell a thread is
reading can never change under it. When a thread falls too far behind, or a key
is new to it, it simply reads the key's current cell from the global table and
caches it.

## Capacities (and why there is no heap)

The module is sized up front. There is nothing to grow and nothing to `malloc`
after `init`:

```cpp
struct environment_config_t {
    std::size_t max_keys = 256;       // distinct keys
    std::size_t max_key_len = 256;    // longest key, in bytes
    std::size_t max_value_len = 4096; // longest value, in bytes
    std::size_t max_updates = 256;    // total set_env calls
    std::size_t max_threads = 64;     // concurrent per-thread views
    bool read_process_env = true;     // inherit the process environment
};
```

- `set_env` returns `false` when the update budget (`max_updates`), the key
  space (`max_keys`) or a value length is exceeded — it never throws on the
  fast path.
- Values are immutable, so each `set_env` permanently consumes one value cell.
  Budget accordingly, or reload the module if you need a fresh window.
- `read_process_env = false` gives a clean, file-only store — handy for tests
  and for deployments that want the file to be the single source of truth.

Sizing the arena is straightforward: `environment_footprint(config)` returns
the bytes the module's shard will occupy.

```cpp
environment::environment_config_t config;
arena::arena_config_t arena_config;
arena_config.size = environment::environment_footprint(config) + (64 * 1024);
arena::arena_t arena{arena_config};
environment::init(arena, "app.env", config);
```

## Concurrency

`get_env` is safe to call from any number of threads once the module is
initialized. `set_env` is safe from multiple threads too: writers are
serialized, readers stay lock-free. `init` and `shutdown` are not concurrent —
call them from a single thread, at startup and teardown, with the arena
outliving every reader and writer.

## The header

```cpp
#include <frameworkcpp/environment/environment.hpp>
```

That is the whole module: `init`, `get_env`, `set_env`, `shutdown` and
`environment_footprint`.
