# Memory

A real-time system cannot afford a `malloc` here and there, a lock that stalls
under contention, or memory that appears and disappears without a plan. Every
one of those is a source of latency you cannot bound.

`frameworkcpp::arena` is the framework's answer. It reserves memory up front
with `mmap`, hands it out in slices, and recycles freed blocks back into the
arena — so the hot paths never allocate and never take a lock. The whole module
is header-only.

## The vocabulary

Four words explain the entire module:

| Concept | What it is |
| --- | --- |
| **Arena** | The owner of all the memory. It reserves one or more `mmap` segments and carves shards out of them. The arena knows the budget. |
| **Shard** | A contiguous slice of the arena's memory, identified by a tag. Only the slice's contents belong to the shard. |
| **Bucket** | A pool of same-size slots inside a shard, identified by a tag. All its slots share one size and one alignment. |
| **Slot** | One block of memory inside a bucket. Allocating hands you a slot; freeing returns it. |

```
mmap ─► [ region 0 ][ region 1 (only if it grows) ]...
            └─ [ shard A ][ shard B ][ ... ]          every shard = one slice
                   └─ [ bucket: many slots of the same size ]
```

## The mental model

1. You create an **arena** and tell it your budget (or let it grow).
2. You **carve shards** out of it with `create_shard(tag, specs)`. Each shard
   gets its own slice of memory — 64-byte aligned, so high-alignment buckets
   stay correct no matter what came before.
3. Inside a shard you declare **buckets** — pools of same-size slots — with
   `object_spec_t` entries.
4. You allocate **slots** from buckets; when you free a slot it goes back to
   its bucket for reuse. Nothing is returned to the OS until the arena dies.

## Concurrency is a per-bucket choice

Whether a bucket is safe to share is a decision you make when you grab its
handle. You pick one of three models:

| Model | What it means | Cost |
| --- | --- | --- |
| Single-thread | One thread owns the bucket end to end. | nothing — plain fields |
| Lock-free | Several threads allocate/free without locks. | atomics only |
| Thread-safe | Safe from anywhere without thinking. | a shared mutex |

The model is fixed the first time the bucket is used. Asking for a different one
later aborts the program — the choice is not a per-call detail, it is part of
the bucket's contract.

## The budget

An arena can be capped. You set a **soft limit** (a warning watermark) and a
**hard limit** (the hard ceiling). Growing past the soft limit warns; growing
past the hard limit throws `arena_overflow_exception` (or aborts, if you
configure it). See [Configuration](configuration) for the details.

## The headers

The module is split so you include only what you use. Pick the header that
matches the piece you touch:

| You are using | Include |
| --- | --- |
| The arena itself (`arena_t`) and the whole module | `<frameworkcpp/arena/arena.hpp>` |
| A shard view (`arena_shard_t`) | `<frameworkcpp/arena/shard.hpp>` |
| A bucket handle (`arena_bucket_t`) | `<frameworkcpp/arena/bucket.hpp>` |
| Unique pointer (`arena_unique_ptr_t`) | `<frameworkcpp/arena/unique_ptr.hpp>` |
| Shared pointer (`arena_shared_ptr_t`) | `<frameworkcpp/arena/shared_ptr.hpp>` |
| Standard allocator / PMR bridge | `<frameworkcpp/arena/arena_allocator.hpp>` |
| Configuration types only | `<frameworkcpp/arena/config.hpp>` |

`arena.hpp` is the umbrella: it includes everything, so snippets that need to
*build* an arena just include it. The granular headers matter when the arena
already exists and you only touch one piece.

## The pieces

Read them top to bottom — each page builds on the previous one:

- [The Arena](arena-t) — owns the memory, your budget, and how to carve shards.
- [Shards](arena-shard-t) — the slices and how allocation finds the right bucket.
- [Buckets](arena-bucket) — the pools of same-size slots and how to grab one directly.
- [Smart Pointers](smart-pointers) — owning and shared pointers that return memory on their own.
- [Allocators & std::pmr](allocator) — the standard allocator bridge for `std::vector`.
- [Configuration](configuration) — the knobs and the compile-time footprints.
