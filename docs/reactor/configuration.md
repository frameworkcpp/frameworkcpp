# Configuration

The reactor is sized up front, like every other module in the framework. You
hand `bind_arena` a `reactor_config_t` and it carves exactly that much space
out of an arena.

```cpp
struct reactor_config_t {
    std::size_t queues_ = 1;                 // named event loops
    std::size_t workers_per_queue_ = 1;      // threads per queue
    std::size_t max_jobs_per_queue_ = 1024;  // operations in flight per queue
    std::size_t max_sockets_ = 4096;         // registered sockets
    std::size_t max_strands_ = 4096;         // serialisation keys
    std::size_t max_timers_per_queue_ = 256; // pending timers per queue
    bool affinity_ = false;                  // pin each queue to its own core
};
```

| Field | What it bounds | Notes |
| --- | --- | --- |
| `queues_` | Number of named event loops. | Each one gets its own `io_uring` ring and its own thread. |
| `workers_per_queue_` | Threads per queue. | One thread per queue is the current model; the knob is reserved. |
| `max_jobs_per_queue_` | Operations in flight per queue. | The operation pool. When it is empty, `post` returns `false`. |
| `max_sockets_` | Registered sockets. | A socket table entry is small, but the table is fixed. |
| `max_strands_` | Distinct strand keys. | Strands are addressed modulo this number. |
| `max_timers_per_queue_` | Pending timers per queue. | A queue's timers live in their own pool, so a full timer pool never starves the I/O pool. |
| `affinity_` | Whether queues are pinned to a core. | Queue *n* is pinned to CPU *n*. The pin is best-effort. |

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
