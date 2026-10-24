# Configuration

The reactor is sized up front, like every other module in the framework. You
hand `bind_arena` a `reactor_config_t` and it carves exactly that much space
out of an arena.

```cpp
struct queue_concurrency_t {
    concurrency_model_t producers_ = concurrency_model_t::lock_free_;
    concurrency_model_t consumers_ = concurrency_model_t::single_thread_;
};

struct reactor_config_t {
    std::size_t queues_ = 1;                 // named event loops
    std::size_t max_jobs_per_queue_ = 1024;  // operations in flight per queue
    std::size_t max_sockets_ = 4096;         // registered sockets
    std::size_t max_strands_ = 4096;         // serialisation keys
    std::size_t max_timers_per_queue_ = 256; // pending timers per queue
    bool affinity_ = false;                  // pin each queue to its own core
    backend_t backend_ = backend_t::auto_;   // io_uring, epoll or auto
    queue_concurrency_t concurrency_ = {};   // default for new queues
    std::size_t workers_per_queue_ = 1;      // must be 1 today
};
```

| Field | What it bounds | Notes |
| --- | --- | --- |
| `queues_` | Number of named event loops. | Each one gets its own `io_uring` ring and its own thread. |
| `workers_per_queue_` | Threads per queue. | One thread per queue is the only supported value; `bind_arena` rejects anything else. |
| `max_jobs_per_queue_` | Operations in flight per queue. | The operation pool. When it is empty, `post` returns `false`. Must be a power of two. |
| `max_sockets_` | Registered sockets. | A socket table entry is small, but the table is fixed. |
| `max_strands_` | Distinct strand keys. | Strands are addressed modulo this number. |
| `max_timers_per_queue_` | Pending timers per queue. | A queue's timers live in their own pool, so a full timer pool never starves the I/O pool. |
| `affinity_` | Whether queues are pinned to a core. | Queue *n* is pinned to CPU *n*. The pin is best-effort. |
| `backend_` | Which event backend to use. | `auto_` picks `io_uring` when the kernel has it, `epoll` otherwise. |
| `concurrency_` | Default concurrency contract for new queues. | Two independent axes: producers and consumers. See below. |

## Choosing the concurrency model

The model belongs to a queue, not to the reactor. `reactor_config_t` only sets
the default that `get_or_create(name)` inherits; `get_or_create(name, model)`
chooses a different contract for that queue. A queue keeps its model for life:
asking for the same name with another model returns `nullptr`.

The contract has two axes because producing and consuming are different jobs:

| `producers_` | Who may call `post` | Cost |
| --- | --- | --- |
| `single_thread_` | Exactly one thread, verified in debug builds | plain loads and stores |
| `lock_free_` | Any number of threads | one CAS per post |
| `thread_safe_` | Any number of threads | one producer-side mutex per queue |

`consumers_` is separate because the consumer side is not symmetric with the
producer side. Today it must be `single_thread_`: each queue has one event loop,
and the io_uring ring, timer heap and strands do not support several consumers.
`bind_arena` rejects anything else, so the struct cannot promise concurrency
the reactor does not implement.

`thread_safe_` locks only producers. The single consumer keeps using the serial
path, so the mutex never serialises the queue's hot loop. `lock_free_` uses a
bounded Vyukov ring (MPMC) even though consumption is single-threaded today;
that keeps producer publication correct for every allowed producer model.

Strands have their own submission contract. `create_strand(queue, model)`
declares how many threads will post operations for that key; the no-argument
overload inherits the queue's `producers_` model. A `single_thread_` strand is
checked in debug builds, just like a `single_thread_` queue. If several threads
will submit to the same strand, create it with `lock_free_` or `thread_safe_`.
Handler serialisation is already guaranteed by the single queue consumer; the
strand model is about submission, not about running two handlers at once.

## Choosing a backend

```cpp
enum class backend_t : std::uint8_t {
  auto_ = 0,     // io_uring si el kernel lo expone, si no epoll
  io_uring_ = 1, // exigir io_uring: bind_arena falla si no esta
  epoll_ = 2,    // exigir epoll
};
```

`auto_` is the default and the right answer almost always: the reactor uses
`io_uring` when the kernel offers it and falls back to `epoll` when it does
not, so the same binary works on a modern server and on a kernel — or an
emulator — without it.

Force `io_uring_` when the io_uring path is the one you must have: the
reactor then refuses to start rather than silently serving you with epoll.
Force `epoll_` to test the portable path, or because you know the kernel's
`io_uring` is worse than its `epoll`.

The two backends are not identical in their timing: `epoll` waits in
milliseconds and wakes once per event, while `io_uring` waits in nanoseconds
and reaps a batch. Timers therefore fire with millisecond precision under
`epoll` and nanosecond precision under `io_uring`.

## Sizing the arena

`precompute(config)` returns the exact number of bytes the reactor's shard will
occupy, and it is a `constexpr` function — you can derive the budget at compile
time:

```cpp
constexpr reactor::reactor_config_t config{};
constexpr std::size_t bytes_k = reactor::precompute(config);

static_assert(bytes_k > 0);
```

The number is exact because the reactor builds the same table of object
descriptors for the calculation and for the allocation. You can check that
yourself:

```cpp
arena::arena_config_t arena_config;
arena_config.size_ = reactor::precompute(config);
arena::arena_t arena{arena_config};

reactor::reactor_t reactor;
reactor.bind_arena(arena, config);
assert(arena.get_used() == reactor::precompute(config));
```

Or take the ready-made helpers:

```cpp
arena::arena_t arena{reactor::get_reactor_arena_config(config)};
```

`get_reactor_arena_bytes(config, extra)` is `round_to_page(footprint + extra)`,
so pass any headroom you want for other modules as `extra`.

## Sizing the operation pool

The pool is the number of operations that can be outstanding on a queue at once:
posted but not yet completed. Size it from the number of connections that queue
serves, because a connection with a pending read holds one operation.

```cpp
// one queue, one operation per connection, plus room for writes
reactor_config_t config;
config.queues_ = 1;
config.max_jobs_per_queue_ = 1024;   // ~1000 concurrent connections
```

When the pool is empty, `post` returns `false`. That is your back-pressure
signal: accept fewer connections, or raise the number.

## Combining modules in one arena

Every module exposes the same contract, so you can size one arena for all of
them:

```cpp
constexpr environment::environment_config_t env_k{};
constexpr reactor::reactor_config_t rea_k{};

constexpr std::size_t total_k =
    arena::merge_reservations(
        arena::make_reservation<environment::environment_module_t>("environment", env_k),
        arena::make_reservation<reactor::reactor_module_t>("reactor", rea_k));

arena::arena_config_t arena_config;
arena_config.size_ = arena::round_to_page(total_k);
arena::arena_t arena{arena_config};
```

The contract is described in [Memory reservation](../memory/reservation).

## Suggested configurations

**Single queue, everything on one thread** — the simplest thing that works:

```cpp
reactor_config_t config;
config.queues_ = 1;
config.max_jobs_per_queue_ = 1024;
```

**One queue per core** — the usual choice for a busy server. With `affinity_`
on, queue *n* stays on CPU *n*, so its cache stays warm and the event loop
never migrates between cores:

```cpp
reactor_config_t config;
config.queues_ = 4;                    // one per core on a four-core box
config.max_jobs_per_queue_ = 1024;
config.affinity_ = true;
```

**Many short connections** — raise the socket table, keep the pool modest:

```cpp
reactor_config_t config;
config.queues_ = 4;
config.max_jobs_per_queue_ = 256;
config.max_sockets_ = 16384;
```
