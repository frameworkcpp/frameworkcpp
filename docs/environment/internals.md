# Internals

This page is the "inside" of the module: the structures carved into the arena,
the version protocol that makes reads cheap, and the memory ordering that keeps
it correct. You do not need any of it to *use* the store — read it when you want
to reason about performance, footprint, or the concurrency guarantees.

Everything below lives in `namespace frameworkcpp::environment::detail`, in
`<frameworkcpp/environment/detail/digest.hpp>` and
`<frameworkcpp/environment/environment.hpp>`.

## The shard layout

`init` carves **one shard** with nine buckets. Fixed arrays all around; nothing
grows, nothing is heap-allocated.

```
control_t                    the module's root: pointers + capacities (1)
key_table   [ slots ]        open-addressing array of key_cell_t*        (slots)
global      [ slots ]        open-addressing array of value_cell_t*      (slots)
journal     [ journal_size ] delta ring of patch_t (power-of-two length)
retired     [ retire_cap ]   FIFO of retired cells awaiting recycle
key_cell    [ max_keys ]     immutable key bytes
value_cell  [ max_keys + value_cells ]   immutable value bytes (recycled)
view        [ max_threads ]  one view_t per thread
view_reg    [ max_threads ]  pointers to the pre-allocated views
```

`slots` is `next_pow2(max_keys * 2)`, so the key table stays at or below a 0.5
load factor and open addressing stays short. The journal is a ring indexed by
`version & (journal_size - 1)`.

## The structures

```cpp
// detail/digest.hpp

struct key_cell_t {            // immutable once published; id == its table slot
    std::uint32_t hash_;       // FNV-1a of the key bytes
    std::uint32_t len_;
    // key bytes follow the struct
};

struct value_cell_t {          // immutable once published, recycled when retired
    std::uint32_t len_;
    std::uint32_t pad_;
    // value bytes follow the struct
};

struct patch_t {               // one delta-ring slot per update
    std::atomic<std::uint64_t> tag_;     // version + 1 when valid, 0 while writing
    std::atomic<std::uint32_t> key_id_;
    std::atomic<value_cell_t *> value_;
};

struct retired_t {             // a superseded cell waiting to be recycled
    value_cell_t *cell_;
    std::uint64_t retired_version_;
};

struct view_t {                // one per thread
    std::uint64_t my_version_; // how far this cache has caught up
    std::uint64_t published_;  // my_version, published to the reclaimer
    std::uint32_t active_;     // 1 while a thread owns the slot, 0 when free
    // a trailing value_cell_t* cache (one per key_id) follows in memory and is
    // reached through cached_slots(view); no [0]/[1] member in C++20
};
```

The key table and the global-slot array are the same size and are indexed by the
**same slot id**: `key_table[i]` holds a key, `global[i]` holds that key's
current value. A thread that resolves a key to slot `i` can cache the value at
`cached[i]` and never hash again until its version goes stale.

```cpp
// environment.hpp

struct control_t {
    arena::arena_shard_t shard_;
    std::atomic<key_cell_t *>   *key_table_;   // [slots]
    std::atomic<value_cell_t *> *global_;      // [slots]
    patch_t                     *journal_;     // [journal_size]
    retired_t                   *retired_;     // [retire_cap], FIFO
    view_t                     **views_;       // [max_threads]
    std::atomic<std::uint64_t>   version_;     // committed updates (monotonic)
    std::atomic<std::size_t>     retire_head_; // FIFO cursors
    std::atomic<std::size_t>     retire_tail_;
    // sizes + masks..., std::mutex writer_
};
```

## Key lookup

Keys are found by hash-and-probe. `fnv1a` hashes the bytes; the slot is
`hash & slot_mask`; collisions walk forward. Because `global[i]` shares the slot
with `key_table[i]`, resolving a key gives you the value's home for free.

```cpp
std::size_t index = hash & control->slot_mask_;
while (cell != nullptr && !matches(cell, name))
    index = (index + 1) & control->slot_mask_;   // linear probe
```

Keys are never deleted, so an empty slot is a real terminator — no tombstones.
Insertion (`find_or_insert`) is only ever called by `set_env`, under the writer
mutex, so the table has a single writer.

## The version protocol

This is the heart of the design. `version_` counts committed updates; the ring is
the ordered list of what each update changed. `set_env` on a key that already
exists also **retires** the previous cell.

**Writer** (`set_env`, holding the mutex):

1. Recycle any retired cells no live view can still see (see below).
2. Resolve or insert the key → `key_id`.
3. Carve a fresh `value_cell_t`, fill it, swap it into `global[key_id]`
   (release); push the old cell onto the retirement FIFO.
4. Write `journal[version & mask] = {tag_: version + 1, key_id, cell}` — the tag
   is bumped last with a release store, so the slot is only valid once complete.
5. `version_.store(version + 1, release)` — the commit point.

**Reader** (`get_env`):

```cpp
const std::uint64_t v = control->version_.load(acquire);
if (view->my_version_ != v)
    sync_view(control, view, v);          // catch up
value_cell_t **cache = cached_slots(view);
value_cell_t *cell = cache[id];
if (cell == nullptr) {                    // key new to this thread
    cell = control->global_[id].load(acquire);
    cache[id] = cell;
}
publish(view);                            // store published_ only if it moved
```

`sync_view` replays only the missed slots, using the tag as a commit gate:

```cpp
if (target - my >= control->journal_size_) { reseed(view); return; } // fell off the ring
for (v in [my, target)) {
    t1 = slot.tag_.load(acquire);
    if (t1 != v + 1) { reseed(view); return; }   // slot was overwritten by a wrap
    read key_id, value;
    if (slot.tag_.load(acquire) != t1) { reseed(view); return; } // raced a writer
    cached_slots(view)[key_id] = value;
}
view->my_version_ = target;
```

The double tag read is a seqlock: a slot is only trusted if it is stable across
the field reads, which also makes rewriting a wrapped slot safe.

## Epoch reclamation

Immutable cells would leak forever, so retired cells are recycled once no reader
can still reference them. A cell retired at version `r` is safe to reuse when
**every live view has published a version greater than `r`** — at that point no
view's cache holds the retired cell, and no view will replay the slot that
introduced it. The reclaimer (the single writer, under its mutex) takes the
minimum `published_` across all active views and frees every FIFO entry older
than it:

```cpp
min = version;                           // no active views -> everything is free
for (slot in views[0..max_threads))      // scan the pool; active flag filters
    if (slot.active_) min = min(min, slot.published_);
while (fifo not empty && fifo.front().retired_version_ < min)
    free(fifo.pop_front());
```

Views are a fixed pool: a thread claims a free slot (`active_` 0→1) with a CAS
the first time it touches the store, and the slot is marked inactive again when
its thread exits, so slots are recycled rather than burned. A view re-publishes
its version to the reclaimer only when it actually advanced, so the steady-state
read path touches no shared atomic beyond the `version_` load. The `thread_local`
guard is keyed by an init/shutdown epoch, so it never touches a dead arena.

## Lifetime of a returned view

Because cells are recycled, a `get_env` result is valid **until the next
`get_env`/`set_env` on the same thread** — not for the arena's lifetime. The
reclaimer honors this exactly: a cell cannot be recycled while the thread that
holds a view of it has not advanced, so the value is alive for as long as it was
promised. Read it, copy it, or compare it — but do not stash it across calls.


## Heap-free, like the arena

`init` uses `arena::create_shard`, `allocate_tagged` and `mmap` — never
`new`/`malloc`. `get_env` and `set_env` only touch arena memory and atomics.
The test suite proves it: `tests/environment_heap_test.cpp` overrides
`operator new`/`delete` and asserts that `init`, `get_env` and `set_env`
allocate **zero** bytes on the heap.
