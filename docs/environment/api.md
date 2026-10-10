# The API

The public surface is four functions and one configuration type. Everything else
is internal.

```cpp
namespace frameworkcpp::environment {

struct environment_config_t { /* ... */ };

void init(arena::arena_t &arena, const char *dotenv_path,
          const environment_config_t &config = {});
void shutdown() noexcept;

[[nodiscard]] std::string_view get_env(std::string_view key,
                                       std::string_view fallback);
bool set_env(std::string_view key, std::string_view value);

std::size_t environment_footprint(const environment_config_t &config);

}
```

## get_env

```cpp
[[nodiscard]] std::string_view get_env(std::string_view key, std::string_view fallback);
```

Reads the current value of `key`.

- Returns a `std::string_view` **pointing straight at the arena cell** — no copy
  and no allocation.
- The view is valid **until the next `get_env`/`set_env` on the same thread**.
  Values are recycled, so the cell behind a view can be reused once your thread
  moves on; read the value, copy it, or compare it, but do not stash the view
  across calls.
- Returns `fallback` when the key is missing, the value has not been published
  yet, or the module is not initialized. `fallback` may be empty.
- The key is compared as raw bytes (the store is case-sensitive).

```cpp
const std::string_view level = get_env("LOG_LEVEL", "info");
if (level == "debug") { /* ... */ }
```

The call is `[[nodiscard]]` on purpose: silently dropping the result usually
means you forgot to read it.

## set_env

```cpp
bool set_env(std::string_view key, std::string_view value);
```

Publishes a new value for `key`, creating the key if it does not exist yet.

- Returns `true` on success.
- Returns `false` — **never throws** — when the module is uninitialized, when
  the key is empty or longer than `max_key_len`, when the value is longer than
  `max_value_len`, when the key space is full, or when the value pool is
  momentarily pinned by lagging readers. Treat `false` as "your write did not
  happen".
- Writes are last-writer-wins: two concurrent `set_env` calls on the same key
  leave whichever committed last.

```cpp
if (!set_env("WORKERS", "8")) {
    // out of capacity or bad input: decide your own fallback
}
```

Updates are effectively unbounded: a superseded value cell is retired and later
recycled once no live reader can still see it, so repeated writes reuse the same
fixed pool of cells instead of draining a budget.

## environment_footprint

```cpp
std::size_t environment_footprint(const environment_config_t &config);
```

Returns how many bytes the module's shard will occupy for a given
configuration. Use it to size the arena so the module is guaranteed to fit:

```cpp
environment::environment_config_t config;
arena::arena_config_t arena_config;
arena_config.size = environment::environment_footprint(config) + (64 * 1024);
arena::arena_t arena{arena_config};
environment::init(arena, "app.env", config);
```

The `+ 64 KiB` is headroom for anything else you carve into the same arena; the
footprint itself is exact.

## What you do not get (by design)

- **No deletion**: there is no `unset_env`. Removing a key would need
  reclamation, and reclamation fights immutability. Model "absent" with a value
  instead.
- **No enumeration**: `get_env` is the iterator. A real-time system should know
  its keys by name, not walk a map.
- **No exceptions on the hot path**: `get_env` and `set_env` never throw; only
  `init` does, and only at setup.
