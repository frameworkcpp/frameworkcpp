# Memory Management

Real-time systems cannot afford a `malloc` here and there, a lock under
contention, or memory that appears and disappears without a plan.
`frameworkcpp::arena` is the framework's answer: all blocks come from
pre-reserved `mmap` segments, freed blocks are recycled back into the arena,
and the fast paths never take a lock. The whole module is header-only.

## The idea

An arena starts as one or more raw memory segments. You carve **shards** out of
them — each shard is a private, contiguous slice that only it sees — and inside
a shard you declare **buckets**: runs of same-size blocks with their own
free-list.

```
mmap ─► [ region 0 ][ region 1 (only if it grows) ]...
            └─ [ shard A ][ shard B ][ ... ]          every shard = one slice
                   └─ bucket(tag, size, align, count) = block run + free-list
```

- Creating a shard carves a slice from the arena and returns a view to it.
  Slices are kept 64-byte aligned, so high-alignment buckets stay correct no
  matter how many shards came before.
- If a new shard does not fit, the arena reserves one more `mmap`. That is the
  only system call the module makes at runtime.
- Freeing a block returns it to its bucket for **reuse**. Nothing goes back to
  the operating system until the arena is destroyed.

## Concurrency

Whether a shard is safe to share is a decision you make per bucket. When you
create a writer or reader handle for a bucket, you pick the concurrency model:

| Model | What it means |
| --- | --- |
| Single-thread | No mutex, no atomics. One thread owns the bucket end to end — the fastest path. |
| Lock-free | Atomics and a lock-free ring; several threads can allocate and free the same bucket without locks. |
| Thread-safe | A shared mutex guards the bucket; safe from anywhere without thinking. |

The model is fixed the first time the bucket is used. Asking for a different one
later aborts the program.

## What you get

- Raw, uninitialized allocation that is free on the single-thread path, with
  blocks aligned exactly as the bucket declares.
- Safe deallocation: the arena verifies a pointer really belongs to it before
  touching anything, so foreign pointers are harmless no-ops.
- Per-bucket counters so you can observe churn.
- Compile-time footprints, so an arena can be sized to the exact byte.

## The pieces

- [The Arena](arena-t) — owns the memory, carves shards, sizes the budget.
- [Shards](arena-shard-t) — the shard view; where allocation and reuse happen.
- [Buckets](arena-bucket) — a handle bound to one bucket.
- [Smart Pointers](smart-pointers) — owning and shared pointers that return
  memory to the arena on their own.
- [Allocators & std::pmr](allocator) — the standard allocator bridge for
  `std::vector` and PMR containers.
- [Configuration](configuration) — the knobs and the compile-time footprints.