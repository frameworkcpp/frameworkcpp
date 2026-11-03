// SPDX-License-Identifier: AGPL-3.0-only

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "frameworkcpp/memory/memory.hpp"
#include "frameworkcpp/reactor/reactor.hpp"

#include "detail/stats.hpp"
#include "detail/workers.hpp"

namespace {

using frameworkcpp::memory::memory_config_t;
using frameworkcpp::memory::memory_t;
using frameworkcpp::memory::round_to_page;

using frameworkcpp::reactor::op_kind_t;
using frameworkcpp::reactor::op_result_t;
using frameworkcpp::reactor::queue_t;
using frameworkcpp::reactor::reactor_config_t;
using frameworkcpp::reactor::reactor_t;
using frameworkcpp::reactor::socket_t;

constexpr std::size_t payload_k = 64;
constexpr std::size_t jobs_per_queue_k = 16384;
constexpr std::size_t max_sockets_k = 32;
constexpr std::size_t max_strands_k = 16;
constexpr std::size_t max_timers_k = 64;
constexpr std::uint64_t timer_delay_ns_k = 60ULL * 1000ULL * 1000ULL * 1000ULL;

reactor_config_t make_reactor_config() {
  reactor_config_t _config;
  _config.queues_ = 1;
  _config.workers_per_queue_ = 1;
  _config.max_jobs_per_queue_ = jobs_per_queue_k;
  _config.max_sockets_ = max_sockets_k;
  _config.max_strands_ = max_strands_k;
  _config.max_timers_per_queue_ = max_timers_k;
  _config.affinity_ = false;
  return _config;
}

memory_config_t make_memory_config() {
  memory_config_t _config;
  _config.size_ = round_to_page(reactor_t::precompute(make_reactor_config()));
  return _config;
}

// Giro ocupado: medir la latencia de despacho no debe incluir el coste del
// scheduler. La fecha limite se comprueba cada 4096 vueltas.
bool wait_flag(const std::atomic<bool> &flag) {
  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::size_t _spins = 0;
  while (!flag.load(std::memory_order_acquire)) {
    if ((++_spins & 0xfff) == 0 && std::chrono::steady_clock::now() >= _deadline_k)
      return false;
  }
  return true;
}

bool wait_timer_count(reactor_t &reactor, queue_t *queue) {
  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::size_t _spins = 0;
  while (reactor.get_timer_count(queue) != 0) {
    if ((++_spins & 0xfff) == 0 && std::chrono::steady_clock::now() >= _deadline_k)
      return false;
  }
  return true;
}

struct base_fixture_t {
  memory_t memory_{make_memory_config()};
  reactor_t reactor_;
  queue_t *queue_ = nullptr;
  bool running_ = false;

  base_fixture_t() = default;
  base_fixture_t(const base_fixture_t &) = delete;
  base_fixture_t &operator=(const base_fixture_t &) = delete;
  base_fixture_t(base_fixture_t &&) = delete;
  base_fixture_t &operator=(base_fixture_t &&) = delete;
  ~base_fixture_t() { stop(); }

  bool start_queue(benchmark::State &state) {
    if (!reactor_.bind_memory(memory_, make_reactor_config())) {
      state.SkipWithError("reactor bind failed");
      return false;
    }
    queue_ = reactor_.get_or_create("bench");
    if (queue_ == nullptr) {
      state.SkipWithError("queue creation failed");
      return false;
    }
    return true;
  }

  void start_events() {
    reactor_.run_all();
    running_ = true;
  }

  void stop() {
    if (running_) {
      reactor_.stop_all();
      running_ = false;
    }
  }
}; // struct base_fixture_t

template <typename Fixture, typename Operation>
void run_reactor_benchmark(benchmark::State &state, Operation operation) {
  Fixture _fixture;
  bool _initialized = false;
  while (state.KeepRunning()) {
    if (!_initialized) {
      state.PauseTiming();
      _initialized = true;
      if (!_fixture.start(state))
        break;
      state.ResumeTiming();
    }
    if (!operation(_fixture)) {
      state.SkipWithError("reactor operation failed");
      break;
    }
  }
}

struct dispatch_fixture_t : base_fixture_t {
  bool start(benchmark::State &state) {
    if (!start_queue(state))
      return false;
    start_events();
    return true;
  }
}; // struct dispatch_fixture_t

// --- coste del motor ---------------------------------------------------------
struct drain_context_t {
  std::atomic<std::size_t> done_{0};
}; // struct drain_context_t

void on_drain(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<drain_context_t *>(ctx);
  (void) result;
  _context->done_.fetch_add(1, std::memory_order_release);
}

bool wait_count(const std::atomic<std::size_t> &counter, std::size_t target) {
  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::size_t _spins = 0;
  while (counter.load(std::memory_order_acquire) < target) {
    if ((++_spins & 0xfff) == 0 && std::chrono::steady_clock::now() >= _deadline_k)
      return false;
  }
  return true;
}

// Coste de submit puro: se postean N jobs vacios y se para el cronometro antes
// de esperar a que el hilo de la queue los consuma. `items_per_second` es el
// caudal de encolado. La distribucion del op en si queda en los counters.
void bench_post_empty(benchmark::State &state) {
  const auto _batch_k = static_cast<std::size_t>(state.range(0));
  drain_context_t _context;
  frameworkcpp::bench::histogram_t _histogram;
  run_reactor_benchmark<dispatch_fixture_t>(
      state, [&_context, &state, _batch_k, &_histogram](dispatch_fixture_t &fixture) {
        _context.done_.store(0, std::memory_order_relaxed);
        for (std::size_t _index = 0; _index < _batch_k; ++_index) {
          const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
          const bool _ok_k =
              fixture.queue_->post(op_kind_t::custom_, -1, nullptr, 0, on_drain, &_context);
          const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
          if (!_ok_k)
            return false;
          _histogram.record(_t1_k - _t0_k);
        }
        state.PauseTiming();
        const bool _drained_k = wait_count(_context.done_, _batch_k);
        state.ResumeTiming();
        return _drained_k;
      });
  const auto _items_k =
      static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(_batch_k);
  frameworkcpp::bench::report_histogram(state, _histogram, _items_k);
}

// Coste total del motor para N jobs vacios: submit + despacho. La muestra del
// histograma es el lote completo; mide el tiempo hasta que el hilo de la cola
// entrega el ultimo callback.
void bench_process_empty(benchmark::State &state) {
  const auto _batch_k = static_cast<std::size_t>(state.range(0));
  drain_context_t _context;
  frameworkcpp::bench::histogram_t _histogram;
  run_reactor_benchmark<dispatch_fixture_t>(
      state, [&_context, _batch_k, &_histogram](dispatch_fixture_t &fixture) {
        _context.done_.store(0, std::memory_order_relaxed);
        const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
        for (std::size_t _index = 0; _index < _batch_k; ++_index) {
          if (!fixture.queue_->post(op_kind_t::custom_, -1, nullptr, 0, on_drain, &_context))
            return false;
        }
        if (!wait_count(_context.done_, _batch_k))
          return false;
        const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
        _histogram.record(_t1_k - _t0_k);
        return true;
      });
  const auto _items_k =
      static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(_batch_k);
  frameworkcpp::bench::report_histogram(state, _histogram, _items_k);
}

struct burst_slot_t {
  std::atomic<std::size_t> *done_counter_ = nullptr;
  frameworkcpp::bench::histogram_t *histogram_ = nullptr;
  std::uint64_t posted_cycles_ = 0;
}; // struct burst_slot_t

void on_burst(void *ctx, const op_result_t &result) {
  auto *_slot = static_cast<burst_slot_t *>(ctx);
  (void) result;
  const std::uint64_t _now_k = frameworkcpp::bench::cycle_count();
  if (_slot->histogram_ != nullptr)
    _slot->histogram_->record(_now_k - _slot->posted_cycles_);
  _slot->done_counter_->fetch_add(1, std::memory_order_release);
}

// Latencia end-to-end de cada job del burst. El callback del reactor registra
// la latencia desde el post hasta que corre el handler: incluye la cola que
// tiene delante y el coste del dispatch, no el scheduler que espera el caller.
void bench_burst_latency(benchmark::State &state) {
  const auto _burst_k = static_cast<std::size_t>(state.range(0));
  std::vector<burst_slot_t> _slots(_burst_k);
  frameworkcpp::bench::histogram_t _histogram;

  run_reactor_benchmark<dispatch_fixture_t>(
      state, [&_slots, &state, _burst_k, &_histogram](dispatch_fixture_t &fixture) {
        std::atomic<std::size_t> _done{0};
        for (auto &_slot : _slots) {
          _slot.done_counter_ = &_done;
          _slot.histogram_ = &_histogram;
        }

        for (std::size_t _index = 0; _index < _burst_k; ++_index) {
          _slots[_index].posted_cycles_ = frameworkcpp::bench::cycle_count();
          if (!fixture.queue_->post(op_kind_t::custom_, -1, nullptr, 0, on_burst, &_slots[_index]))
            return false;
        }

        return wait_count(_done, _burst_k);
      });

  const auto _items_k =
      static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(_burst_k);
  frameworkcpp::bench::report_histogram(state, _histogram, _items_k);
}

// --- timer schedule / cancel
// -------------------------------------------------
struct timer_context_t {
  std::atomic<int> fired_{0};
}; // struct timer_context_t

void on_timer(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<timer_context_t *>(ctx);
  (void) result;
  _context->fired_.fetch_add(1, std::memory_order_relaxed);
}

struct timer_fixture_t : base_fixture_t {
  bool start(benchmark::State &state) {
    if (!start_queue(state))
      return false;
    start_events();
    return true;
  }
}; // struct timer_fixture_t

void bench_timer_schedule_cancel(benchmark::State &state) {
  timer_context_t _context;
  frameworkcpp::bench::histogram_t _histogram;
  run_reactor_benchmark<timer_fixture_t>(state, [&_context, &_histogram](timer_fixture_t &fixture) {
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    const auto _id_k =
        fixture.reactor_.schedule_timer(fixture.queue_, timer_delay_ns_k, on_timer, &_context);
    if (_id_k == 0)
      return false;
    const bool _cancelled_k = fixture.reactor_.cancel_timer(fixture.queue_, _id_k);
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    if (!_cancelled_k)
      return false;
    _histogram.record(_t1_k - _t0_k);
    return wait_timer_count(fixture.reactor_, fixture.queue_);
  });
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

// --- socket send / recv ------------------------------------------------------
struct io_context_t {
  std::atomic<bool> done_{false};
  std::atomic<int> ec_{0};
  std::atomic<std::size_t> bytes_{0};
  char buffer_[payload_k]{};
}; // struct io_context_t

void on_io(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<io_context_t *>(ctx);
  _context->ec_.store(result.ec_, std::memory_order_relaxed);
  _context->bytes_.store(result.bytes_, std::memory_order_relaxed);
  _context->done_.store(true, std::memory_order_release);
}

struct socketpair_fixture_t : base_fixture_t {
  int peer_fd_ = -1;
  socket_t socket_;

  socketpair_fixture_t() = default;
  socketpair_fixture_t(const socketpair_fixture_t &) = delete;
  socketpair_fixture_t &operator=(const socketpair_fixture_t &) = delete;
  socketpair_fixture_t(socketpair_fixture_t &&) = delete;
  socketpair_fixture_t &operator=(socketpair_fixture_t &&) = delete;
  ~socketpair_fixture_t() {
    if (peer_fd_ >= 0)
      ::close(peer_fd_);
  }

  bool start(benchmark::State &state) {
    if (!start_queue(state))
      return false;

    int _fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds) != 0) {
      state.SkipWithError("socketpair failed");
      return false;
    }
    peer_fd_ = _fds[1];
    socket_ = reactor_.create_socket(_fds[0], queue_);
    if (!socket_) {
      state.SkipWithError("socket registration failed");
      return false;
    }

    start_events();
    return true;
  }
}; // struct socketpair_fixture_t

void bench_socket_send(benchmark::State &state) {
  io_context_t _context;
  std::memset(_context.buffer_, 0x5a, sizeof(_context.buffer_));
  frameworkcpp::bench::histogram_t _histogram;
  run_reactor_benchmark<socketpair_fixture_t>(
      state, [&_context, &_histogram](socketpair_fixture_t &fixture) {
        _context.done_.store(false, std::memory_order_relaxed);
        const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
        if (!fixture.socket_.send(_context.buffer_, payload_k, on_io, &_context))
          return false;
        if (!wait_flag(_context.done_))
          return false;
        const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
        _histogram.record(_t1_k - _t0_k);
        char _drain_k[payload_k];
        // La lectura vacia el peer del socketpair; bloquear aqui es intencionado.
        return ::read(fixture.peer_fd_,
                      _drain_k,
                      sizeof(_drain_k)) == // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
               static_cast<ssize_t>(payload_k);
      });
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

void bench_socket_recv(benchmark::State &state) {
  io_context_t _context;
  std::memset(_context.buffer_, 0x5a, sizeof(_context.buffer_));
  frameworkcpp::bench::histogram_t _histogram;
  run_reactor_benchmark<socketpair_fixture_t>(
      state, [&_context, &_histogram](socketpair_fixture_t &fixture) {
        _context.done_.store(false, std::memory_order_relaxed);
        const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
        if (::write(fixture.peer_fd_, _context.buffer_, payload_k) !=
            static_cast<ssize_t>(payload_k))
          return false;
        if (!fixture.socket_.recv(_context.buffer_, payload_k, on_io, &_context))
          return false;
        if (!wait_flag(_context.done_) || _context.bytes_.load() != payload_k)
          return false;
        const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
        _histogram.record(_t1_k - _t0_k);
        return true;
      });
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

// --- echo round trip ---------------------------------------------------------
struct echo_context_t {
  reactor_t *reactor_ = nullptr;
  queue_t *queue_ = nullptr;
  socket_t echo_socket_;
  char buffer_[payload_k]{};
  std::atomic<bool> accepted_{false};
  std::atomic<bool> failed_{false};
}; // struct echo_context_t

void on_echo_send(void *ctx, const op_result_t &result);

void on_echo_recv(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<echo_context_t *>(ctx);
  if (result.ec_ != 0 || result.bytes_ == 0) {
    _context->failed_.store(true, std::memory_order_release);
    return;
  }
  if (!_context->echo_socket_.send(_context->buffer_, result.bytes_, on_echo_send, _context))
    _context->failed_.store(true, std::memory_order_release);
}

void on_echo_send(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<echo_context_t *>(ctx);
  if (result.ec_ != 0) {
    _context->failed_.store(true, std::memory_order_release);
    return;
  }
  if (!_context->echo_socket_.recv(_context->buffer_, payload_k, on_echo_recv, _context))
    _context->failed_.store(true, std::memory_order_release);
}

void on_accept_echo(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<echo_context_t *>(ctx);
  if (result.fd_ < 0) {
    _context->failed_.store(true, std::memory_order_release);
    _context->accepted_.store(true, std::memory_order_release);
    return;
  }
  _context->echo_socket_ = _context->reactor_->create_socket(result.fd_, _context->queue_);
  if (!_context->echo_socket_ ||
      !_context->echo_socket_.recv(_context->buffer_, payload_k, on_echo_recv, _context))
    _context->failed_.store(true, std::memory_order_release);
  _context->accepted_.store(true, std::memory_order_release);
}

struct echo_fixture_t : base_fixture_t {
  socket_t listener_;
  int client_fd_ = -1;
  sockaddr_in bound_{};
  echo_context_t context_{};

  echo_fixture_t() = default;
  echo_fixture_t(const echo_fixture_t &) = delete;
  echo_fixture_t &operator=(const echo_fixture_t &) = delete;
  echo_fixture_t(echo_fixture_t &&) = delete;
  echo_fixture_t &operator=(echo_fixture_t &&) = delete;
  ~echo_fixture_t() {
    if (client_fd_ >= 0)
      ::close(client_fd_);
  }

  bool start(benchmark::State &state) {
    if (!start_queue(state))
      return false;

    sockaddr_in _addr{};
    _addr.sin_family = AF_INET;
    _addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    _addr.sin_port = 0;

    listener_ =
        reactor_.listen_tcp(queue_, reinterpret_cast<const sockaddr *>(&_addr), sizeof(_addr));
    if (!listener_) {
      state.SkipWithError("listen failed");
      return false;
    }

    socklen_t _bound_len = sizeof(bound_);
    if (::getsockname(listener_.fd(), reinterpret_cast<sockaddr *>(&bound_), &_bound_len) != 0) {
      state.SkipWithError("getsockname failed");
      return false;
    }

    context_.reactor_ = &reactor_;
    context_.queue_ = queue_;
    if (!listener_.accept(on_accept_echo, &context_)) {
      state.SkipWithError("accept post failed");
      return false;
    }

    start_events();

    client_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd_ < 0 ||
        ::connect(client_fd_, reinterpret_cast<const sockaddr *>(&bound_), sizeof(bound_)) != 0) {
      state.SkipWithError("client connect failed");
      return false;
    }

    if (!wait_flag(context_.accepted_) || context_.failed_.load(std::memory_order_acquire)) {
      state.SkipWithError("server did not accept the connection");
      return false;
    }
    return true;
  }
}; // struct echo_fixture_t

void bench_echo_round_trip(benchmark::State &state) {
  char _out_k[payload_k];
  char _in_k[payload_k];
  std::memset(_out_k, 0x5a, sizeof(_out_k));
  frameworkcpp::bench::histogram_t _histogram;

  run_reactor_benchmark<echo_fixture_t>(
      state, [&_out_k, &_in_k, &_histogram](echo_fixture_t &fixture) {
        const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
        if (::write(fixture.client_fd_, _out_k, sizeof(_out_k)) !=
            static_cast<ssize_t>(sizeof(_out_k)))
          return false;
        // La lectura bloquea hasta que el eco vuelve: bloquear aqui es intencionado.
        const ssize_t _read_k =
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            ::read(fixture.client_fd_, _in_k, sizeof(_in_k));
        if (_read_k != static_cast<ssize_t>(sizeof(_in_k)))
          return false;
        const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
        _histogram.record(_t1_k - _t0_k);
        return std::memcmp(_out_k, _in_k, sizeof(_out_k)) == 0;
      });

  const auto _items_k = static_cast<std::uint64_t>(state.iterations());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * payload_k));
  frameworkcpp::bench::report_histogram(state, _histogram, _items_k);
}

// --- accept ------------------------------------------------------------------
struct accept_context_t {
  reactor_t *reactor_ = nullptr;
  queue_t *queue_ = nullptr;
  socket_t listener_;
  std::atomic<int> accepted_{0};
  std::atomic<bool> failed_{false};
}; // struct accept_context_t

void on_accept(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<accept_context_t *>(ctx);
  if (result.fd_ < 0) {
    _context->failed_.store(true, std::memory_order_release);
    return;
  }
  ::close(result.fd_);
  _context->accepted_.fetch_add(1, std::memory_order_release);
  if (!_context->listener_.accept(on_accept, _context))
    _context->failed_.store(true, std::memory_order_release);
}

struct accept_fixture_t : base_fixture_t {
  accept_context_t context_{};
  sockaddr_in bound_{};

  bool start(benchmark::State &state) {
    if (!start_queue(state))
      return false;

    sockaddr_in _addr{};
    _addr.sin_family = AF_INET;
    _addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    _addr.sin_port = 0;

    context_.reactor_ = &reactor_;
    context_.queue_ = queue_;
    context_.listener_ =
        reactor_.listen_tcp(queue_, reinterpret_cast<const sockaddr *>(&_addr), sizeof(_addr));
    if (!context_.listener_) {
      state.SkipWithError("listen failed");
      return false;
    }

    socklen_t _bound_len = sizeof(bound_);
    if (::getsockname(
            context_.listener_.fd(), reinterpret_cast<sockaddr *>(&bound_), &_bound_len) != 0) {
      state.SkipWithError("getsockname failed");
      return false;
    }

    if (!context_.listener_.accept(on_accept, &context_)) {
      state.SkipWithError("accept post failed");
      return false;
    }

    start_events();
    return true;
  }
}; // struct accept_fixture_t

void bench_accept(benchmark::State &state) {
  frameworkcpp::bench::histogram_t _histogram;
  run_reactor_benchmark<accept_fixture_t>(state, [&_histogram](accept_fixture_t &fixture) {
    const int _target_k = fixture.context_.accepted_.load(std::memory_order_acquire) + 1;
    const int _client_k = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_client_k < 0)
      return false;
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    const int _connected_k = ::connect(
        _client_k, reinterpret_cast<const sockaddr *>(&fixture.bound_), sizeof(fixture.bound_));
    if (_connected_k != 0) {
      ::close(_client_k);
      return false;
    }
    const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::size_t _spins = 0;
    while (fixture.context_.accepted_.load(std::memory_order_acquire) < _target_k) {
      if ((++_spins & 0xfff) == 0 && std::chrono::steady_clock::now() >= _deadline_k)
        break;
    }

    ::close(_client_k);
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    if (fixture.context_.accepted_.load(std::memory_order_acquire) < _target_k ||
        fixture.context_.failed_.load(std::memory_order_acquire))
      return false;
    _histogram.record(_t1_k - _t0_k);
    return true;
  });
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

// --- multi-producer post ---------------------------------------------------
//
// N workers postean al mismo queue sin limitar su tasa; el hilo de la queue
// drena un op vacío por op. Cada iteración del bench es una tanda de posts
// en paralelo; los histogramas de los workers se suman al final.

constexpr std::size_t mp_ops_per_worker_k = 256;

void on_noop(void *ctx, const op_result_t &result) {
  (void) ctx;
  (void) result;
}

// Cuerpo común para los benches con productores concurrentes sobre un mismo
// queue. Pasa la referencia al pool de métricas por referencia; la variante
// del modelo se elige en cada bench concreto.
template <std::size_t ThreadCount>
void bench_post_shared(benchmark::State &state,
                       queue_t *queue,
                       std::atomic<std::uint64_t> &dropped) {
  static frameworkcpp::bench::workers_t<ThreadCount> _pool_k;

  while (state.KeepRunning()) {
    dropped.store(0, std::memory_order_relaxed);
    const auto _t0_k = std::chrono::steady_clock::now();
    _pool_k.execute([queue, &dropped](std::size_t index, frameworkcpp::bench::histogram_t &hist) {
      (void) index;
      for (std::size_t _op = 0; _op < mp_ops_per_worker_k; ++_op) {
        const std::uint64_t _op0_k = frameworkcpp::bench::cycle_count();
        const bool _ok_k = queue->post(op_kind_t::custom_, -1, nullptr, 0, on_noop, nullptr);
        const std::uint64_t _op1_k = frameworkcpp::bench::cycle_count();
        if (!_ok_k) {
          dropped.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        hist.record(_op1_k - _op0_k);
      }
    });
    const auto _t1_k = std::chrono::steady_clock::now();

    const double _seconds_k = std::chrono::duration<double>(_t1_k - _t0_k).count();
    const std::uint64_t _items_k =
        static_cast<std::uint64_t>(ThreadCount) * static_cast<std::uint64_t>(mp_ops_per_worker_k);
    const frameworkcpp::bench::histogram_t _hist_k = _pool_k.merged();
    _pool_k.reset();

    state.counters["aggregate_items_per_second"] = static_cast<double>(_items_k) / _seconds_k;
    state.counters["dropped_posts"] = static_cast<double>(dropped.load());
    frameworkcpp::bench::report_histogram(state, _hist_k, _items_k);
  }
}

struct post_mp_holder_t {
  post_mp_holder_t(const post_mp_holder_t &) = delete;
  post_mp_holder_t &operator=(const post_mp_holder_t &) = delete;
  post_mp_holder_t(post_mp_holder_t &&) = delete;
  post_mp_holder_t &operator=(post_mp_holder_t &&) = delete;
  memory_t memory_;
  reactor_t reactor_;
  queue_t *queue_ = nullptr;
  std::atomic<std::uint64_t> dropped_{0};

  post_mp_holder_t() : memory_{make_memory_config()} {
    if (!reactor_.bind_memory(memory_, make_reactor_config()))
      std::abort();
    queue_ = reactor_.get_or_create("bench-mp-lock-free"); // hereda lock_free_ de la config
    if (queue_ == nullptr)
      std::abort();
    reactor_.run_all();
  }
  ~post_mp_holder_t() { reactor_.stop_all(); }
}; // struct post_mp_holder_t

post_mp_holder_t &post_mp_holder() {
  static post_mp_holder_t _holder_k;
  return _holder_k;
}

struct post_mp_ts_holder_t {
  post_mp_ts_holder_t(const post_mp_ts_holder_t &) = delete;
  post_mp_ts_holder_t &operator=(const post_mp_ts_holder_t &) = delete;
  post_mp_ts_holder_t(post_mp_ts_holder_t &&) = delete;
  post_mp_ts_holder_t &operator=(post_mp_ts_holder_t &&) = delete;
  memory_t memory_;
  reactor_t reactor_;
  queue_t *queue_ = nullptr;
  std::atomic<std::uint64_t> dropped_{0};

  post_mp_ts_holder_t() : memory_{make_memory_config()} {
    reactor_config_t _config_k = make_reactor_config();
    _config_k.concurrency_.producers_ = frameworkcpp::concurrency_model_t::thread_safe_;
    if (!reactor_.bind_memory(memory_, _config_k))
      std::abort();
    queue_ = reactor_.get_or_create("bench-mp-thread-safe");
    if (queue_ == nullptr)
      std::abort();
    reactor_.run_all();
  }
  ~post_mp_ts_holder_t() { reactor_.stop_all(); }
}; // struct post_mp_ts_holder_t

post_mp_ts_holder_t &post_mp_ts_holder() {
  static post_mp_ts_holder_t _holder_k;
  return _holder_k;
}

// --- thread propia por productor (single_thread_) --------------------------
//
// Cada productor tiene su propio queue con su hilo consumidor, el techo de
// escalado cuando nadie comparte nada. Config.queues_ = N queues; el modelo
// de productores por defecto es lock_free_, pero el bench usa
// producers_=single_thread_ porque cada queue lo consume un único hilo.

constexpr std::size_t private_queues_k = 4;

reactor_config_t make_private_reactor_config() {
  auto _config = make_reactor_config();
  _config.queues_ = private_queues_k;
  _config.concurrency_.producers_ = frameworkcpp::concurrency_model_t::single_thread_;
  return _config;
}

memory_config_t make_private_memory_config() {
  memory_config_t _config;
  _config.size_ = round_to_page(reactor_t::precompute(make_private_reactor_config()));
  return _config;
}

struct post_private_holder_t {
  post_private_holder_t(const post_private_holder_t &) = delete;
  post_private_holder_t &operator=(const post_private_holder_t &) = delete;
  post_private_holder_t(post_private_holder_t &&) = delete;
  post_private_holder_t &operator=(post_private_holder_t &&) = delete;
  memory_t memory_;
  reactor_t reactor_;
  queue_t *queues_[private_queues_k] = {};

  post_private_holder_t() : memory_{make_private_memory_config()} {
    const reactor_config_t _config_k = make_private_reactor_config();
    if (!reactor_.bind_memory(memory_, _config_k))
      std::abort();
    for (std::size_t _i = 0; _i < private_queues_k; ++_i) {
      const std::string _name_k = "bench-private-" + std::to_string(_i);
      queues_[_i] = reactor_.get_or_create(_name_k);
      if (queues_[_i] == nullptr)
        std::abort();
    }
    reactor_.run_all();
  }
  ~post_private_holder_t() { reactor_.stop_all(); }
}; // struct post_private_holder_t

post_private_holder_t &post_private_holder() {
  static post_private_holder_t _holder_k;
  return _holder_k;
}

template <std::size_t ThreadCount> void bench_post_multiproducers(benchmark::State &state) {
  auto &_holder_k = post_mp_holder();
  bench_post_shared<ThreadCount>(state, _holder_k.queue_, _holder_k.dropped_);
}

template <std::size_t ThreadCount> void bench_post_mp_thread_safe(benchmark::State &state) {
  auto &_holder_k = post_mp_ts_holder();
  bench_post_shared<ThreadCount>(state, _holder_k.queue_, _holder_k.dropped_);
}

template <std::size_t ThreadCount> void bench_post_private(benchmark::State &state) {
  auto &_holder_k = post_private_holder();
  static frameworkcpp::bench::workers_t<ThreadCount> _pool_k;

  while (state.KeepRunning()) {
    const auto _t0_k = std::chrono::steady_clock::now();
    _pool_k.execute([&_holder_k](std::size_t index, frameworkcpp::bench::histogram_t &hist) {
      queue_t *_queue = _holder_k.queues_[index];
      for (std::size_t _op = 0; _op < mp_ops_per_worker_k; ++_op) {
        const std::uint64_t _op0_k = frameworkcpp::bench::cycle_count();
        const bool _ok_k = _queue->post(op_kind_t::custom_, -1, nullptr, 0, on_noop, nullptr);
        const std::uint64_t _op1_k = frameworkcpp::bench::cycle_count();
        if (_ok_k)
          hist.record(_op1_k - _op0_k);
      }
    });
    const auto _t1_k = std::chrono::steady_clock::now();

    const double _seconds_k = std::chrono::duration<double>(_t1_k - _t0_k).count();
    const std::uint64_t _items_k =
        static_cast<std::uint64_t>(ThreadCount) * static_cast<std::uint64_t>(mp_ops_per_worker_k);
    const frameworkcpp::bench::histogram_t _hist_k = _pool_k.merged();
    _pool_k.reset();

    state.counters["aggregate_items_per_second"] = static_cast<double>(_items_k) / _seconds_k;
    frameworkcpp::bench::report_histogram(state, _hist_k, _items_k);
  }
}

} // namespace

BENCHMARK(bench_post_empty)->Name("reactor/post_empty")->Arg(64)->Arg(512)->Arg(4096);
BENCHMARK(bench_process_empty)
    ->Name("reactor/process_empty")
    ->Arg(1)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);
BENCHMARK(bench_burst_latency)
    ->Name("reactor/burst_latency")
    ->Arg(1)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);
BENCHMARK(bench_timer_schedule_cancel)->Name("reactor/timer_schedule_cancel");
BENCHMARK(bench_socket_send)->Name("reactor/socket_send");
BENCHMARK(bench_socket_recv)->Name("reactor/socket_recv");
BENCHMARK(bench_echo_round_trip)->Name("reactor/echo_round_trip");
BENCHMARK(bench_accept)->Name("reactor/accept");
BENCHMARK_TEMPLATE(bench_post_multiproducers, 1)->Name("reactor/post_mp/1");
BENCHMARK_TEMPLATE(bench_post_multiproducers, 2)->Name("reactor/post_mp/2");
BENCHMARK_TEMPLATE(bench_post_multiproducers, 4)->Name("reactor/post_mp/4");
BENCHMARK_TEMPLATE(bench_post_mp_thread_safe, 1)->Name("reactor/post_mp_thread_safe/1");
BENCHMARK_TEMPLATE(bench_post_mp_thread_safe, 2)->Name("reactor/post_mp_thread_safe/2");
BENCHMARK_TEMPLATE(bench_post_mp_thread_safe, 4)->Name("reactor/post_mp_thread_safe/4");
BENCHMARK_TEMPLATE(bench_post_private, 1)->Name("reactor/post_private_mt/1");
BENCHMARK_TEMPLATE(bench_post_private, 2)->Name("reactor/post_private_mt/2");
BENCHMARK_TEMPLATE(bench_post_private, 4)->Name("reactor/post_private_mt/4");

BENCHMARK_MAIN();
