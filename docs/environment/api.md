# The API

The public surface is small: four functions and a couple of types. Everything
else is internal. This page is the reference for what each one does and the
rules you have to respect when you call it.

```cpp
namespace frameworkcpp::environment {

struct environment_config_t { /* ... */ };

void init(memory::memory_t &memory, const environment_config_t &config = {});
void shutdown() noexcept;

[[nodiscard]] std::string_view get_env(std::string_view key,
                                       std::string_view fallback);
bool set_env(std::string_view key, std::string_view value);

struct environment_entry_t { std::string_view key_; std::string_view value_; };
[[nodiscard]] std::size_t get_environment_keys(environment_entry_t *out,
                                               std::size_t capacity) noexcept;

std::size_t get_environment_footprint(const environment_config_t &config);
std::size_t precompute(const environment_config_t &config) noexcept;

}
```

## get_env

```cpp
[[nodiscard]] std::string_view get_env(std::string_view key, std::string_view fallback);
```

Reads the current value of `key`.

- Returns a `std::string_view` **pointing straight at the memory cell** — no copy
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
  the key is empty or longer than `max_key_len_`, when the value is longer than
  `max_value_len_`, when the key space is full, or when the value pool is
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

## get_environment_keys

```cpp
[[nodiscard]] std::size_t get_environment_keys(environment_entry_t *out,
                                               std::size_t capacity) noexcept;
```

Copies up to `capacity` key/value pairs into `out` and returns how many were
written. Both `std::string_view`s point straight at immutable memory cells, so
they stay valid while the module lives and the entry is not overwritten by a
later `set_env`. Returns `0` when the module is uninitialized or `out` is null.

```cpp
environment_entry_t entries[64];
const std::size_t n = get_environment_keys(entries, 64);
for (std::size_t i = 0; i < n; ++i) std::printf("%.*s=%.*s\n",
    (int)entries[i].key_.size(), entries[i].key_.data(),
    (int)entries[i].value_.size(), entries[i].value_.data());
```

This is an introspection helper, not a hot path: it walks the whole key table,
so a real-time loop should still know its keys by name.

## get_environment_footprint

```cpp
std::size_t get_environment_footprint(const environment_config_t &config);
std::size_t precompute(const environment_config_t &config) noexcept;
```

Returns how many bytes the module's shard will occupy for a given
configuration. Use it to size the memory so the module is guaranteed to fit:

```cpp
environment::environment_config_t config;
memory::memory_config_t memory_config;
memory_config.size_ = environment::get_environment_footprint(config) + (64 * 1024);
memory::memory_t memory{memory_config};
environment::init(memory, config);
```

The `+ 64 KiB` is headroom for anything else you carve into the same memory; the
footprint itself is exact.

## What you do not get (by design)

- **No deletion**: there is no `unset_env`. Removing a key would need
  reclamation, and reclamation fights immutability. Model "absent" with a value
  instead.
- **No enumeration on the hot path**: `get_environment_keys` exists for debugging
  and export, but it walks the whole table. A real-time loop should know its keys
  by name, not walk a map.
- **No exceptions on the hot path**: `get_env` and `set_env` never throw; only
  `init` does, and only at setup.
