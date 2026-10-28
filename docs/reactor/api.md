# The API

Everything the reactor exposes, in the order you will meet it.

```cpp
namespace frameworkcpp::reactor {

enum class op_kind_t { accept_, recv_, send_, connect_, timer_, custom_ };

struct op_result_t {
    int ec_;             // 0 on success, errno on failure
    std::size_t bytes_;  // bytes transferred
};

using reactor_fn_t = void (*)(void *ctx, const op_result_t &result);

class reactor_t {
  public:
    reactor_t() = default;
    reactor_t(const reactor_t &) = delete;
    reactor_t &operator=(const reactor_t &) = delete;
    ~reactor_t();

    bool bind_arena(arena::arena_t &arena, const reactor_config_t &config) noexcept;
    [[nodiscard]] bool initialized() const noexcept;

    queue_t *get_or_create(std::string_view name) const noexcept;
    queue_t *get_or_create(std::string_view name,
                           queue_concurrency_t concurrency) const noexcept;
    strand_t create_strand(queue_t *queue, concurrency_model_t concurrency) const noexcept;
    strand_t create_strand(queue_t *queue) const noexcept;
    socket_t create_socket(int fd, queue_t *queue) const noexcept;
    socket_t open_socket(int family, int type, int protocol, queue_t *queue) const noexcept;
    socket_t listen_tcp(queue_t *queue, const sockaddr *addr, std::size_t addr_len,
                        int backlog = 128) const noexcept;
    socket_t listen_unix(queue_t *queue, const char *path, int backlog = 128) const noexcept;
    socket_t bind_udp(queue_t *queue, const sockaddr *addr, std::size_t addr_len) const noexcept;
    socket_t open_icmp(queue_t *queue) const noexcept;

    std::uint64_t schedule_timer(queue_t *queue, std::uint64_t delay_ns,
                                  reactor_fn_t fn, void *ctx,
                                  std::uint64_t strand_id = 0) noexcept;
    bool cancel_timer(queue_t *queue, std::uint64_t timer_id) const noexcept;
    [[nodiscard]] std::size_t get_timer_count(queue_t *queue) const noexcept;

    bool migrate(socket_t &socket, queue_t *target) noexcept;
    [[nodiscard]] std::size_t rebalance() const noexcept;
    [[nodiscard]] std::size_t get_socket_count(queue_t *queue) const noexcept;

    void run_all() noexcept;
    void stop_all() noexcept;

    static constexpr std::size_t precompute(const reactor_config_t &config) noexcept;
};

}
```

## bind_arena

```cpp
bool bind_arena(arena::arena_t &arena, const reactor_config_t &config) noexcept;
```

Carves the reactor's state out of `arena`: one control block per queue, the
operation pool, the socket table and the strand table. Call it once, from a
single thread, before you post anything.

- Returns `false` when the arena has no room for the shard, when an `io_uring`
  ring cannot be created, or when the reactor is already bound.
- After it succeeds, `arena.get_used()` has grown by exactly
  `precompute(config)` bytes.
- The arena must outlive the reactor.

```cpp
reactor::reactor_t reactor;
if (!reactor.bind_arena(arena, config))
    throw std::runtime_error("reactor: not enough arena");
```

## get_or_create

```cpp
queue_t *get_or_create(std::string_view name) noexcept;
queue_t *get_or_create(std::string_view name, queue_concurrency_t concurrency) noexcept;
```

Returns the queue called `name`, creating it if this is the first time the name
appears. Two calls with the same name and model return the same pointer; two
calls with different names return different queues.

- The first overload inherits `reactor_config_t::concurrency_`.
- The second gives that queue its own producer/consumer contract.
- A queue keeps its model for life. Asking for an existing name with another
  model returns `nullptr`.
- Returns `nullptr` when the reactor is not bound, the model is unsupported, or
  the pool of queues is exhausted and the name is new.
- The name is a `std::string_view`: it is not copied, so it must outlive the
  reactor.

```cpp
auto *ingest = reactor.get_or_create("ingest");
auto *same   = reactor.get_or_create("ingest");   // ingest == same

queue_concurrency_t shared{
    concurrency_model_t::lock_free_, concurrency_model_t::single_thread_};
auto *workers = reactor.get_or_create("workers", shared);
```

## create_strand

```cpp
strand_t create_strand(queue_t *queue, concurrency_model_t concurrency) noexcept;
strand_t create_strand(queue_t *queue) noexcept;   // inherits the queue's model
```

Creates a serialisation key owned by `queue` and records the submission model
the caller promises for it. Operations sharing `strand.id()` on the same queue
never run at the same time because that queue has one consumer. The
no-argument overload copies the queue's `producers_` model.

- Returns an invalid strand when the reactor is not bound, the queue is null, or
  `max_strands_` keys have already been created.
- `single_thread_` promises one submitting thread. Debug builds abort if a
  second thread posts an operation with that strand id.
- `lock_free_` and `thread_safe_` allow several submitting threads. They do not
  change handler execution: the queue's single consumer still runs handlers of a
  strand one at a time.

```cpp
auto strand = reactor.create_strand(queue, concurrency_model_t::lock_free_);
socket.recv(buffer, sizeof(buffer), on_read, &state, strand.id());
```

## create_socket

```cpp
socket_t create_socket(int fd, queue_t *queue) noexcept;
```

Registers `fd` with a queue and returns a handle. The reactor does not own the
descriptor: closing it is your job, or call `socket_t::close()`.

- Returns an invalid socket (falsy) when the fd is negative, the queue is null,
  or the socket table is full.
- The socket is not registered with the kernel here. You post the operations;
  the first `accept`, `recv` or `connect` is what arms the `io_uring` ring.

## schedule_timer

```cpp
std::uint64_t schedule_timer(queue_t *queue, std::uint64_t delay_ns,
                              reactor_fn_t fn, void *ctx,
                              std::uint64_t strand_id = 0) noexcept;
```

Schedules a timer that fires `delay_ns` nanoseconds from now. The handler runs
in the queue's thread, like any other operation.

- Returns a timer id (greater than zero) that you can pass to `cancel_timer`,
  or 0 when the queue has no free timer slot.
- The delay is measured against `monotonic_now_ns()`, so it is immune to wall
  clock changes.
- With a `strand_id`, the timer is serialised against the other operations of
  that strand: it does not run while one of them is in flight.

```cpp
const std::uint64_t id = reactor.schedule_timer(queue, 100000000ULL, on_tick, &state);
```

## cancel_timer

```cpp
bool cancel_timer(queue_t *queue, std::uint64_t timer_id) const noexcept;
```

Cancels a timer before it fires. Returns true when the request was queued.

The cancellation is **asynchronous**: the timer heap belongs to the queue's
thread, so the request travels through the operation queue and the thread
drops the timer when it gets to it. A timer that already fired is simply not
in the heap anymore, and the request has no effect.

```cpp
const std::uint64_t id = reactor.schedule_timer(queue, 100000000ULL, on_tick, &state);
reactor.cancel_timer(queue, id);   // the timer will not fire
```

## get_timer_count

```cpp
[[nodiscard]] std::size_t get_timer_count(queue_t *queue) const noexcept;
```

How many timers are still pending in a queue.

## migrate

```cpp
bool migrate(socket_t &socket, queue_t *target) noexcept;
```

Moves a socket to another queue without closing it. The connection stays alive:
its next operations are served by the target queue's thread. This is what lets
you balance connections across threads while the system is running.

- Returns `false` when the socket has operations in flight. A socket with a
  pending read belongs to the queue that will complete it, so moving it would
  leave the connection with two owners.
- Returns `false` when the socket is not registered or the target is null.
- The socket's strand moves with it: the strand id is interpreted in the target
  queue from now on.

```cpp
if (!socket.is_idle())
    return;                            // busy: try again later
reactor.migrate(socket, quietest_queue);
```

## rebalance

```cpp
[[nodiscard]] std::size_t rebalance() const noexcept;
```

Moves idle sockets from the busiest queue to the quietest one, until no queue
carries more than one connection more than another. It moves as few sockets as
possible: churn costs a cache miss on each side.

- Returns how many sockets moved.
- Busy sockets are left alone; they move in a later round.
- Returns 0 when there is one queue, or when the queues are already balanced.

Call it from a slow path — a timer tick, a health check — not from the hot
loop.

## Creating sockets

The reactor can create the sockets for you, so a server does not have to
drop down to raw syscalls before it can hand a descriptor over.

```cpp
socket_t open_socket(int family, int type, int protocol, queue_t *queue) const noexcept;
```

Creates a socket with `socket(2)` and registers it in the queue. `family` is
`AF_INET`, `AF_INET6` or `AF_UNIX`; `type` is `SOCK_STREAM` or `SOCK_DGRAM`.

- Returns an invalid socket when the creation fails or the socket table is
  full. On failure nothing is leaked: the descriptor is closed.

The named helpers build on it:

```cpp
// Servidor TCP: socket + bind + listen
socket_t listen_tcp(queue_t *queue, const sockaddr *addr, std::size_t addr_len,
                    int backlog = 128) const noexcept;

// Servidor Unix domain
socket_t listen_unix(queue_t *queue, const char *path, int backlog = 128) const noexcept;

// Socket UDP enlazado
socket_t bind_udp(queue_t *queue, const sockaddr *addr, std::size_t addr_len) const noexcept;

// Socket ICMP (raw). Necesita privilegios.
socket_t open_icmp(queue_t *queue) const noexcept;
```

`listen_unix` removes a stale socket file if the bind fails, so a restart does
not trip over the leftovers of the previous run.

## Datagrams

```cpp
bool send_to(const void *buffer, std::size_t len, const sockaddr *addr,
             std::size_t addr_len, reactor_fn_t fn, void *ctx,
             std::uint64_t strand_id = 0) noexcept;
bool recv_from(void *buffer, std::size_t len, reactor_fn_t fn, void *ctx,
               std::uint64_t strand_id = 0) noexcept;
[[nodiscard]] const sockaddr *last_peer() const noexcept;
```

`send_to` sends a datagram to a specific address; `recv_from` receives one.
The destination address is copied into the operation, so it does not have to
outlive the call.

`last_peer` returns the address of the sender of the last datagram the socket
received, or null when it has not received one. It is only meaningful for a
socket with one pending receive, which is the usual shape of a datagram
server.

## get_backend

```cpp
[[nodiscard]] backend_t get_backend() const noexcept;
```

Which event backend the reactor ended up using. With `backend_t::auto_` in the
configuration this tells you whether the kernel offered `io_uring` or whether
the reactor fell back to `epoll` — useful to know before you read a timing.

## queue_t::get_concurrency

```cpp
[[nodiscard]] queue_concurrency_t get_concurrency() const noexcept;
```

The producer/consumer contract the queue was created with. It never changes for
the life of the queue.

## get_socket_count

```cpp
[[nodiscard]] std::size_t get_socket_count(queue_t *queue) const noexcept;
```

How many sockets a queue currently serves. Useful to observe the balance.

## queue_t::post

```cpp
bool post(op_kind_t kind, int fd, void *buffer, std::size_t len,
          reactor_fn_t fn, void *ctx,
          std::uint64_t strand_id = 0, int priority = 0) noexcept;
```

Queues an operation for the queue's thread. `fn` is called with `ctx` and the
result when the operation completes.

- Returns `false` when the queue's operation pool is exhausted or the queue
  handle is invalid. It never blocks and never throws.
- `buffer` must stay valid until the handler runs. Put it in memory you own —
  the arena, or a structure that outlives the operation.
- `strand_id` groups operations that must not overlap. `0` means "no strand".
- `priority` is reserved for future use.

This is the low-level entry point; `socket_t` wraps it for the common cases.

## socket_t

```cpp
bool recv(void *buffer, std::size_t len, reactor_fn_t fn, void *ctx,
          std::uint64_t strand_id = 0) noexcept;
bool send(const void *buffer, std::size_t len, reactor_fn_t fn, void *ctx,
          std::uint64_t strand_id = 0) noexcept;
bool accept(reactor_fn_t fn, void *ctx, std::uint64_t strand_id = 0) noexcept;
bool connect(const sockaddr *addr, std::size_t addr_len, reactor_fn_t fn,
             void *ctx, std::uint64_t strand_id = 0) noexcept;
void close() noexcept;
```

Each posts one operation and returns whether it was queued.

- `recv` and `send` are zero-copy in the sense that matters here: the kernel
  reads and writes directly into the buffer you pass. Nothing is copied through
  an intermediate.
- `accept` completes with the peer address in the handler's `result`.
- `connect` needs the address to outlive the operation; copy it into your own
  storage if it is temporary.
- `close` asks the queue to close the descriptor. The queue's thread does it,
  never the caller: closing a descriptor while the queue is using it would be
  a race, and it would leave the pending operation without an owner.
- Closing a socket with an operation in flight is therefore safe: the
  descriptor stays open until the operation finishes, and then the queue closes
  it and releases the slot.
- Closing the same socket twice is harmless: the second call does nothing, so
  it cannot close an unrelated descriptor.
- The descriptor belongs to the reactor after `close`. Do not close it again
  yourself.

## run_all and stop_all

```cpp
void run_all() noexcept;
void stop_all() noexcept;
```

`run_all` starts one thread per queue and returns immediately. Each thread
drains its own operation queue and waits on its own `io_uring` ring with a
timeout, so it notices a stop request promptly.

`stop_all` signals every queue to stop and joins the threads. It is safe to call
it when nothing is running, and the destructor calls it for you.

```cpp
reactor.run_all();
// ... serve ...
reactor.stop_all();
```

A queue can also be driven on the caller's thread:

```cpp
void queue_t::run() noexcept;   // blocks until the queue is stopped
void queue_t::stop() noexcept;
```

`queue_t::run` serves that queue in the calling thread and returns once `stop`
has been requested. `run_all` uses it internally. This is how a
`single_thread_` reactor is run when you do not want a thread per queue. Do not
drive a queue with `run` while `run_all` already owns it.

## The result

```cpp
struct op_result_t {
    int ec_;             // 0 on success, errno on failure
    std::size_t bytes_;  // bytes transferred
};
```

`ec_` is zero on success and a positive `errno` on failure — `-EAGAIN`, `-ECONNREFUSED`,
`-EBADF`. Check it before you use `bytes_`: a failed operation transfers nothing.

## What you do not get (by design)

- **No closures.** Handlers are function pointers. If you want captured state,
  put it in a struct and pass its address.
- **No dynamic allocation.** After `bind_arena`, no path allocates. That
  includes the operation pool: when it runs dry, `post` returns `false`.
- **No blocking `post`.** A full pool is reported, not waited on.
- **One operation per direction.** A socket can have one outstanding read and
  one outstanding write. A second read on the same socket completes with
  `EALREADY` instead of piling up.
- **Migration needs an idle socket.** A socket with an operation in flight
  cannot move, because the queue that will complete that operation has to be
  the one that owns the connection.
