# Smart Pointers

Raw blocks are fast, but sometimes you want the lifetime handled for you.
The arena ships two RAII owners that construct the object, run its destructor,
and hand the block back to the arena — no new, no delete.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Unique ownership

`arena_unique_ptr_t<T>` is a move-only owner. When it dies or is reset, it
destroys the object and returns the block to the arena.

```cpp
template <typename T, typename... Args>
arena_unique_ptr_t<T> make_arena_unique_ptr(arena_shard_t& shard, Args&&... args);
```

```cpp
arena_t arena;
auto shard = arena.create_shard(1, {{0, 64, alignof(std::max_align_t), 32}});

{
    auto p = make_arena_unique_ptr<Widget>(shard, /* ctor args */);
    if (!p) { /* shard had no bucket large enough */ }
    p->run();
} // ~Widget() runs and the block goes back to the arena
```

`get()`, `operator->`, `operator*`, `operator bool` and `reset()` behave like
`std::unique_ptr`, except the pointer is always created through the factory so
the arena that owns the block is never out of sync. It is move-only: there is
no copy, and no way to build it from a raw pointer.

## Shared ownership

`arena_shared_ptr_t<T>` keeps a reference count **inside the same arena
block** as the object, so sharing costs no extra allocation. The block returns
to the arena when the last owner lets go.

```cpp
template <typename T, typename... Args>
arena_shared_ptr_t<T> make_arena_shared_ptr(arena_shard_t& shard, Args&&... args);
```

```cpp
struct Job : arena_enable_shared_from_this<Job>
{
    int id;
};
```

```cpp
arena_t arena;
auto shard = arena.create_shard(1, {{0, 128, alignof(std::max_align_t), 16}});
{
    auto a = make_arena_shared_ptr<Job>(shard, 42);
    auto b = a;              // both own the same Job
}                            // last owner gone → ~Job + back to the arena
```

It supports the usual shared semantics — copy, move, reset, `operator->`,
converting from a derived type — plus `shared_from_this()` when `T` derives
from `arena_enable_shared_from_this<T>`:

```cpp
auto a = make_arena_shared_ptr<Job>(shard, 1);
auto b = a->shared_from_this();   // another owner of the same object
```

`shared_from_this()` throws `std::bad_weak_ptr` when the object was not created
through `make_arena_shared_ptr`, matching the standard behaviour. Weak
references are not part of the arena module yet.

## Which to use

- One owner, no sharing: **unique**.
- The object outlives its creator or is handed around: **shared**.
- Neither, and you just need a scratch buffer: allocate raw through the
  [shard](arena-shard-t) and free it by hand.