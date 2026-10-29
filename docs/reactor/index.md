# Reactor

Serving a socket is a loop: wait for something to happen, do the work, wait
again. Write that loop with `epoll` and you get one syscall per event; write it
with a thread per connection and you get one thread per connection. Both are
expensive in a system whose deadlines are measured in microseconds.

`frameworkcpp::reactor` gives you the third option. It keeps one event loop per
queue, each backed by its own `io_uring` ring, and hands work between threads
through lock-free queues. The kernel batches the syscalls, the queues never take
a lock, and every byte of memory the module uses is reserved before the first
request arrives.

> **Requires liburing for the fast backend.** The reactor talks to `io_uring`
> through `liburing`, so link it in: `-luring`. It is the one module in the
> framework with an external dependency.
>
> The reactor has **two backends**. `io_uring` is the one you want: the kernel
> batches the syscalls and the hot path stops paying for them. `epoll` is the
> portable one: it works on every Linux, including the ones that do not expose
> `io_uring`. The reactor picks `io_uring` when the kernel has it and falls back
> to `epoll` otherwise, so the same binary works everywhere.

```cpp
#include <frameworkcpp/reactor/reactor.hpp>
```

## The vocabulary

| Concept | What it is |
| --- | --- |
| **Reactor** | The owner of the queues. It carves its state out of a memory and hands out queues by name. |
| **Queue** | A named event loop with its own `io_uring` ring and its own thread. Two calls with the same name give you the same queue. |
| **Socket** | A file descriptor registered in a queue. It knows which queue will complete its operations. |
| **Operation** | A unit of work: read, write, accept, connect. It is a fixed-size record carved from the memory. |
| **Strand** | A serialisation key. Operations that share a strand never run at the same time. |
| **Handler** | A plain function pointer plus a context pointer. No closures, no captured state, no heap. |

## The mental model

1. You size the reactor at compile time and carve it out of a memory:

   ```cpp
   constexpr reactor::reactor_config_t config{
       .queues_ = 4,                 // four independent event loops
       .max_jobs_per_queue_ = 1024,  // operations in flight per queue
       .affinity_ = true,            // pin each queue to its own core
   };

   constexpr std::size_t bytes_k = reactor::precompute(config);
   memory::memory_config_t memory_config;
   memory_config.size_ = bytes_k;
   memory::memory_t memory{memory_config};

   reactor::reactor_t reactor;
   reactor.bind_memory(memory, config);
   ```

2. You ask for queues by name. The same name always gives you the same queue, so
   a component can ask for `"ingest"` without knowing who else is using it:

   ```cpp
   auto *queue = reactor.get_or_create("ingest");
   ```

3. You create a socket and post operations on it. The handler is a function
   pointer, not a lambda:

   ```cpp
   void on_read(void *ctx, const reactor::op_result_t &result);

   auto socket = reactor.create_socket(fd, queue);
   socket.recv(buffer, sizeof(buffer), on_read, &state);
   ```

4. `reactor.run_all()` starts one thread per queue and returns. Each thread
   drains its own operation queue and waits on its own `io_uring` ring.

## Why handlers are not lambdas

A capturing lambda is an object of unknown size, and putting it somewhere means
allocating. On a hot path that is a latency spike waiting for the allocator.

The reactor takes a function pointer and a `void *` instead:

```cpp
using reactor_fn_t = void (*)(void *ctx, const op_result_t &result);
```

You keep the state where it belongs — usually in a struct you carved from the
memory — and the handler receives a pointer to it. The framework moves a pointer
and a function address; nothing is copied and nothing is allocated.

```cpp
struct session_t {
    std::uint64_t id_;
    char buffer[4096];
};

void on_read(void *ctx, const reactor::op_result_t &result)
{
    auto *session = static_cast<session_t *>(ctx);
    if (result.ec_ != 0)
        return;                                  // error
    process(session->buffer, result.bytes_);     // no allocation
}
```

## Strands: serialising without a lock

Two operations on the same connection must not overlap, but two operations on
different connections should never wait for each other. A **strand** is the key
you use to say "these belong together".

Give an operation a `strand_id` and the queue will not start another operation
with the same id until the first one completes — not even if the first one is
still waiting on the network:

```cpp
socket.recv(buffer, sizeof(buffer), on_read, &session, /*strand_id=*/session.id_);
socket.send(reply, reply_len, on_write, &session, /*strand_id=*/session.id_);
```

The strand lives in the memory and is manipulated only by the thread that owns
the queue, so no lock is involved. The cost is one flag check per operation.

## What the memory budget buys you

The reactor's whole state — queues, operations, sockets, strands — is carved
from a memory at `bind_memory`. `precompute(config)` returns the exact number of
bytes that will be needed, and it is a `constexpr` function:

```cpp
constexpr std::size_t bytes_k = reactor::precompute(config);
static_assert(bytes_k > 0);
```

That number is not an estimate. The reactor builds the same table of object
descriptors for the calculation and for the allocation, so the memory you size
here is exactly the memory it needs:

```cpp
memory::memory_t memory{memory::memory_config_t{.size_ = reactor::precompute(config)}};
reactor.bind_memory(memory, config);
```

The same contract applies to every module in the framework, which is what lets
you size one memory for all of them:

```cpp
constexpr std::size_t total_k =
    memory::merge_reservations(
        memory::make_reservation<environment::environment_module_t>("environment", env_k),
        memory::make_reservation<reactor::reactor_module_t>("reactor", rea_k));
```

See [Memory reservation](../memory/reservation) for the contract every module
follows.

## Which backend runs where

The two backends cover the same API, so the question is only which one is
exercised. This is what the CI actually runs:

| Architecture | `epoll` | `io_uring` |
| --- | --- | --- |
| `amd64` | tested | tested |
| `arm64` | tested | tested |
| `armv7` | tested | tested through a simulated io_uring-free kernel (* ) |
| `ppc64le` | tested | tested through a simulated io_uring-free kernel (* ) |
| `riscv64` | tested | tested through a simulated io_uring-free kernel (* ) |
| `s390x` | tested | tested through a simulated io_uring-free kernel (* ) |

(*) Those four run under QEMU user-mode emulation, which does not implement the
`io_uring` syscalls: the kernel returns `ENOSYS` and the reactor falls back to
`epoll`. The `io_uring` path is therefore exercised natively on `amd64` and
`arm64`, and on the others through a simulated kernel without `io_uring`, which
is exactly what those platforms see in CI.

The `io_uring` code itself is architecture-independent: the ABI is the same
everywhere, and what does differ per architecture — the size of the
`sockaddr` structures, the alignment of the operations, and whether a 64-bit
atomic is lock-free — is checked at compile time on every architecture in the
matrix, with `static_assert`.

`tools/cross-check.sh` compiles the framework for the four emulated
architectures and lets the compiler of each one answer. It runs in seconds and
needs neither root nor emulation.

## The trade-offs

- **The queue depth is fixed.** `max_jobs_per_queue_` operations can be in
  flight per queue. When the pool is exhausted, `post` returns `false` instead
  of blocking — you decide what to do about back-pressure.
- **One thread per queue.** That is the price of the lock-free handoff: each
  queue is an independent event loop. Use more queues for more parallelism, and
  `affinity_` to keep each one on its own core.
- **`io_uring` is Linux-only.** The module does not build where the kernel lacks
  `io_uring`.

## Where to go next

- [The API](api) — every function, its contract and its failure modes.
- [Configuration](configuration) — the knobs and how to size the memory.
- [Internals](internals) — the ring, the operation pool and the strand protocol.
