# Internals

This page is the "inside" of the module: the structures carved into the arena,
the version protocol that makes reads cheap, and the memory ordering that keeps
it correct. You do not need any of it to *use* the store — read it when you want
to reason about performance, footprint, or the concurrency guarantees.

Everything below lives in `namespace frameworkcpp::environment::detail`, in
`<frameworkcpp/environment/detail/digest.hpp>` and
`<frameworkcpp/environment/environment.hpp>`.

## The shard layout

`init` carves **one shard** with seven buckets. Fixed arrays all around; nothing
grows, nothing is heap-allocated.

```
control_t                    the module's root: pointers + capacities (1)
key_table   [ slots ]        open-addressing array of key_cell_t*        (slots)
global      [ slots ]        open-addressing array of value_cell_t*      (slots)
journal     [ max_updates ]  append-only patch_t records
key_cell    [ max_keys ]     immutable key bytes
value_cell  [ max_keys + max_updates ]   immutable value bytes
view        [ max_threads ]  one view_t per thread
```

`slots` is `next_pow2(max_keys * 2)`, so the key table stays at or below a 0.5
load factor and open addressing stays short.

## The structures

```cpp
// detail/digest.hpp

struct key_cell_t {            // immutable once published; id == its table slot
    std::uint32_t hash;        // FNV-1a of the key bytes
    std::uint32_t len;
    // key bytes follow the struct
};

struct value_cell_t {          // immutable once published
    std::uint32_t len;
    std::uint32_t pad;
    // value bytes follow the struct
};

struct patch_t {               // one journal entry per set_env
    std::uint32_t key_id;      // which slot changed
    std::uint32_t pad;
    value_cell_t *value;       // the new cell
};

struct view_t {                // one per thread
    std::uint64_t my_version;  // how far this cache has caught up
    value_cell_t *cached[];    // indexed by key_id
};
```

The key table and the global-slot array are the same size and are indexed by the
**same slot id**: `key_table[i]` holds a key, `global[i]` holds that key's
current value. A thread that resolves a key to slot `i` can cache the value at
`cached[i]` and never hash again until its version goes stale.

```cpp
// environment.hpp

struct control_t {
    arena::arena_shard_t shard;
    std::atomic<key_cell_t *>   *key_table;   // [slots]
    std::atomic<value_cell_t *> *global;      // [slots]
    patch_t                     *journal;     // [max_updates]
    std::atomic<std::uint64_t>   version;     // committed updates
    std::size_t slots, slot_mask;
    std::size_t max_key_len, max_value_len, max_updates;
    std::mutex writer;                        // serializes set_env
};
```

## Key lookup

Keys are found by hash-and-probe. `fnv1a` hashes the bytes; the slot is
`hash & slot_mask`; collisions walk forward. Because `global[i]` shares the slot
with `key_table[i]`, resolving a key gives you the value's home for free.

```cpp
std::size_t index = hash & control->slot_mask;
while (cell != nullptr && !matches(cell, name))
    index = (index + 1) & control->slot_mask;   // linear probe
```

Keys are never deleted, so an empty slot is a real terminator — no tombstones.
Insertion (`find_or_insert`) is only ever called by `set_env`, under the writer
mutex, so the table has a single writer.

## The version protocol

This is the heart of the design. `version` counts committed updates; the journal
is the ordered list of what each update changed.

**Writer** (`set_env`, holding the mutex):

1. Resolve or insert the key → `key_id`.
2. Carve a fresh `value_cell_t`, fill it, publish it into `global[key_id]`
   (release).
3. Append `journal[version] = {key_id, cell}`.
4. `version.store(version + 1, release)` — this single store is the commit
   point.

**Reader** (`get_env`):

```cpp
const std::uint64_t v = control->version.load(acquire);
if (view->my_version != v)
    sync_view(control, view, v);          // catch up
value_cell_t *cell = view->cached[id];
if (cell == nullptr) {                    // key new to this thread
    cell = control->global[id].load(acquire);
    view->cached[id] = cell;
}
```

`sync_view` walks only the missed entries and applies them to the cache:

```cpp
for (std::uint64_t w = view->my_version; w < target; ++w)
    view->cached[control->journal[w].key_id] = control->journal[w].value;
view->my_version = target;
```

So a caught-up read costs **one atomic load** (the version) plus array indexing;
a behind read costs the number of missed updates. The map is never cloned.

## Why it is safe

- **Immutable cells.** A `value_cell_t` is filled before it is published and
  never written again, so a thread can read a cell without fear of it changing.
  `set_env` produces a *new* cell; it never edits a live one.
- **Append-only journal.** Entries are written once, in version order. A reader
  replaying a suffix touches only committed entries.
- **Ordering.** The writer fills the cell and the journal entry, then commits
  with a *release* store to `version`. A reader that *acquire*-loads `version`
  and sees the new value is guaranteed to see the matching cell and journal
  entry. The same release/acquire pairing guards `global` and `key_table`.
- **Single writer.** All mutation (key insertion, `global` stores, journal
  appends, version bumps) happens under `control->writer`, so those structures
  have one writer and many readers — the classic lock-free-reader shape.

## Capacities and the memory budget

Every capacity is fixed at `init`; there is no reclamation, which is why the
store is heap-free and conflict-free:

| Capacity | Bounds |
| --- | --- |
| `max_keys` | distinct keys (process env + file + new keys from `set_env`) |
| `max_key_len` | longest key, in bytes |
| `max_value_len` | longest value, in bytes |
| `max_updates` | total `set_env` calls over the store's lifetime |
| `max_threads` | number of per-thread views |

Because each `set_env` publishes a fresh immutable cell, the update budget is
the total number of writes, not a window. When any bound is hit, `set_env`
returns `false` — a loud, reproducible failure instead of a hidden allocation.

## Heap-free, like the arena

`init` uses `arena::create_shard`, `allocate_tagged` and `mmap` — never
`new`/`malloc`. `get_env` and `set_env` only touch arena memory and atomics.
The test suite proves it: `tests/environment_heap_test.cpp` overrides
`operator new`/`delete` and asserts that `init`, `get_env` and `set_env`
allocate **zero** bytes on the heap.
