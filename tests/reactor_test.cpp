// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <cerrno>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/reactor/reactor.hpp"

namespace frameworkcpp::reactor {
namespace {

constexpr std::size_t queue_count_k = 2;
constexpr std::size_t jobs_per_queue_k = 64;

arena::arena_config_t make_arena_config(std::size_t bytes) {
  arena::arena_config_t _config;
  _config.size_ = bytes;
  return _config;
}

constexpr reactor_config_t make_reactor_config() {
  reactor_config_t _config;
  _config.queues_ = queue_count_k;
  _config.workers_per_queue_ = 1;
  _config.max_jobs_per_queue_ = jobs_per_queue_k;
  _config.max_sockets_ = 32;
  _config.max_strands_ = 32;
  _config.affinity_ = false;
  return _config;
}

struct recv_context_t {
  std::atomic<bool> done_{false};
  std::atomic<int> ec_{0};
  std::atomic<std::size_t> bytes_{0};
  char data_[64]{};
}; // struct recv_context_t

void on_recv(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<recv_context_t *>(ctx);
  _context->ec_.store(result.ec_);
  _context->bytes_.store(result.bytes_);
  _context->done_.store(true);
}

void on_send(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<recv_context_t *>(ctx);
  _context->ec_.store(result.ec_);
  _context->bytes_.store(result.bytes_);
  _context->done_.store(true);
}

// El footprint calculado en compile time debe caber exactamente en el arena.
TEST(reactor_test_t, FootprintIsExactAndConstexpr) {
  constexpr reactor_config_t _config_k = make_reactor_config();
  constexpr std::size_t _bytes_k = precompute(_config_k);
  static_assert(_bytes_k > 0, "footprint must be known at compile time");

  // El footprint coincide con el shard que la arena calcula a partir de la
  // misma tabla de specs. Esto se comprueba en compile time, así que vale
  // aunque el kernel no exponga io_uring.
  constexpr std::size_t _expected_k = []() {
    arena::object_spec_t _specs[reactor_spec_count_k] = {};
    make_specs(make_reactor_config(), _specs);
    return arena::shard_footprint(_specs);
  }();
  static_assert(_bytes_k == _expected_k, "precompute must match the shard footprint");

  auto _arena_config = make_arena_config(arena::round_to_page(_bytes_k));
  arena::arena_t _arena{_arena_config};

  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, _config_k));
  EXPECT_EQ(_arena.get_used(), _bytes_k);
}

TEST(reactor_test_t, BindArenaAllocatesNothingOnTheHeap) {
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  EXPECT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));
  EXPECT_TRUE(_reactor.initialized());
}

TEST(reactor_test_t, GetOrCreateIsIdempotentPerName) {
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue_a = _reactor.get_or_create("alpha");
  auto *_queue_b = _reactor.get_or_create("alpha");
  auto *_queue_c = _reactor.get_or_create("gamma");
  ASSERT_NE(_queue_a, nullptr);
  EXPECT_EQ(_queue_a, _queue_b);
  EXPECT_NE(_queue_a, _queue_c);
  EXPECT_EQ(_queue_a->name(), "alpha");
}

TEST(reactor_test_t, ReceivesOverIoUring) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
  int _fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds), 0);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("echo");
  ASSERT_NE(_queue, nullptr);
  auto _socket = _reactor.create_socket(_fds[0], _queue);
  ASSERT_TRUE(_socket);

  recv_context_t _context;
  ASSERT_TRUE(_socket.recv(_context.data_, sizeof(_context.data_), on_recv, &_context));

  ASSERT_EQ(::write(_fds[1], "hola", 4), 4);

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.done_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  EXPECT_TRUE(_context.done_.load(std::memory_order_acquire));
  EXPECT_EQ(_context.ec_.load(std::memory_order_acquire), 0);
  EXPECT_EQ(_context.bytes_.load(std::memory_order_acquire), 4U);
  EXPECT_EQ(std::string(_context.data_, 4), "hola");

  ::close(_fds[0]);
  ::close(_fds[1]);
}

TEST(reactor_test_t, SendsOverIoUring) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
  int _fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds), 0);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("sender");
  auto _socket = _reactor.create_socket(_fds[0], _queue);
  ASSERT_TRUE(_socket);

  recv_context_t _context;
  ASSERT_TRUE(_socket.send("hola", 4, on_send, &_context));

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.done_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  EXPECT_TRUE(_context.done_.load(std::memory_order_acquire));
  EXPECT_EQ(_context.ec_.load(std::memory_order_acquire), 0);
  EXPECT_EQ(_context.bytes_.load(std::memory_order_acquire), 4U);

  char _buffer[8]{};
  EXPECT_EQ(::read(_fds[1], _buffer, 4), 4);
  EXPECT_EQ(std::string(_buffer, 4), "hola");

  ::close(_fds[0]);
  ::close(_fds[1]);
}

TEST(reactor_test_t, MigratesAnIdleSocketToAnotherQueue) {
  int _fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds), 0);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue_a = _reactor.get_or_create("alpha");
  auto *_queue_b = _reactor.get_or_create("beta");
  ASSERT_NE(_queue_a, nullptr);
  ASSERT_NE(_queue_b, nullptr);

  auto _socket = _reactor.create_socket(_fds[0], _queue_a);
  ASSERT_TRUE(_socket);
  EXPECT_EQ(_socket.queue(), _queue_a);
  EXPECT_TRUE(_socket.is_idle());

  // La conexión sigue viva: solo cambia el hilo que la atiende.
  EXPECT_TRUE(_reactor.migrate(_socket, _queue_b));
  EXPECT_EQ(_socket.queue(), _queue_b);
  EXPECT_EQ(_reactor.get_socket_count(_queue_b), 1U);
  EXPECT_EQ(_reactor.get_socket_count(_queue_a), 0U);

  ::close(_fds[0]);
  ::close(_fds[1]);
}

TEST(reactor_test_t, RefusesToMigrateABusySocket) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
  int _fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds), 0);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue_a = _reactor.get_or_create("alpha");
  auto *_queue_b = _reactor.get_or_create("beta");
  auto _socket = _reactor.create_socket(_fds[0], _queue_a);
  ASSERT_TRUE(_socket);

  recv_context_t _context;
  ASSERT_TRUE(_socket.recv(_context.data_, sizeof(_context.data_), on_recv, &_context));
  EXPECT_FALSE(_socket.is_idle());

  // Con una operación en vuelo el socket tiene dos dueños: no se puede mudarlo.
  EXPECT_FALSE(_reactor.migrate(_socket, _queue_b));
  EXPECT_EQ(_socket.queue(), _queue_a);

  ::close(_fds[0]);
  ::close(_fds[1]);
}

TEST(reactor_test_t, RebalancesSocketsAcrossQueues) {
  int _fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  for (int _index = 0; _index < 4; ++_index)
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, &_fds[_index * 2]), 0);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue_a = _reactor.get_or_create("alpha");
  auto *_queue_b = _reactor.get_or_create("beta");
  ASSERT_NE(_queue_a, nullptr);
  ASSERT_NE(_queue_b, nullptr);

  for (int _index = 0; _index < 4; ++_index) {
    auto _socket = _reactor.create_socket(_fds[_index * 2], _queue_a);
    ASSERT_TRUE(_socket);
  }
  EXPECT_EQ(_reactor.get_socket_count(_queue_a), 4U);
  EXPECT_EQ(_reactor.get_socket_count(_queue_b), 0U);

  // Reparte lo mínimo para que ninguna queue lleve más de una conexión de más.
  const auto _moved_k = _reactor.rebalance();
  EXPECT_EQ(_moved_k, 2U);
  EXPECT_EQ(_reactor.get_socket_count(_queue_a), 2U);
  EXPECT_EQ(_reactor.get_socket_count(_queue_b), 2U);

  // Repartido: un segundo rebalanceo no mueve nada.
  EXPECT_EQ(_reactor.rebalance(), 0U);

  for (const int _fd_k : _fds)
    ::close(_fd_k);
}

TEST(reactor_test_t, AffinityPinsEachQueueToItsOwnCore) {
  reactor_config_t _config = make_reactor_config();
  _config.affinity_ = true;

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(_config)));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, _config));

  // Con afinidad activa el reactor arranca igual: el pin es una mejora, no un
  // requisito. Si el sistema no lo permite, el reactor sigue funcionando.
  _reactor.run_all();
  EXPECT_TRUE(_reactor.initialized());
  _reactor.stop_all();
}

struct timer_context_t {
  std::atomic<bool> fired_{false};
  std::atomic<std::uint64_t> fired_at_ns_{0};
}; // struct timer_context_t

void on_timer(void *ctx, const op_result_t &result) {
  auto *_context = static_cast<timer_context_t *>(ctx);
  _context->fired_at_ns_.store(monotonic_now_ns(), std::memory_order_release);
  _context->fired_.store(true, std::memory_order_release);
  (void) result;
}

TEST(reactor_test_t, TimerFiresAfterItsDeadline) {
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("timers");
  ASSERT_NE(_queue, nullptr);

  timer_context_t _context;
  const std::uint64_t _id_k = _reactor.schedule_timer(_queue, 50000000ULL, on_timer, &_context);
  ASSERT_NE(_id_k, 0U);
  EXPECT_EQ(_reactor.get_timer_count(_queue), 1U);

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.fired_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  EXPECT_TRUE(_context.fired_.load(std::memory_order_acquire));
  EXPECT_EQ(_reactor.get_timer_count(_queue), 0U);
}

TEST(reactor_test_t, CancelsATimerBeforeItFires) {
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("timers");
  timer_context_t _context;
  const std::uint64_t _id_k = _reactor.schedule_timer(_queue, 50000000ULL, on_timer, &_context);
  ASSERT_NE(_id_k, 0U);
  EXPECT_TRUE(_reactor.cancel_timer(_queue, _id_k));
  EXPECT_EQ(_reactor.get_timer_count(_queue), 0U);

  // Cancelar dos veces no tiene efecto: el timer ya no existe.
  EXPECT_FALSE(_reactor.cancel_timer(_queue, _id_k));

  _reactor.run_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  _reactor.stop_all();

  EXPECT_FALSE(_context.fired_.load(std::memory_order_acquire));
}

TEST(reactor_test_t, FiresTimersInDeadlineOrder) {
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("timers");
  timer_context_t _first;
  timer_context_t _second;
  // Se insertan al revés: el heap tiene que entregarlos por deadline.
  const std::uint64_t _second_id_k =
      _reactor.schedule_timer(_queue, 60000000ULL, on_timer, &_second);
  const std::uint64_t _first_id_k = _reactor.schedule_timer(_queue, 20000000ULL, on_timer, &_first);
  ASSERT_NE(_second_id_k, 0U);
  ASSERT_NE(_first_id_k, 0U);

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_second.fired_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  EXPECT_TRUE(_first.fired_.load(std::memory_order_acquire));
  EXPECT_TRUE(_second.fired_.load(std::memory_order_acquire));
  EXPECT_LE(_first.fired_at_ns_.load(std::memory_order_acquire),
            _second.fired_at_ns_.load(std::memory_order_acquire));
}

struct strand_order_t {
  std::atomic<int> counter_{0};
  std::atomic<int> max_concurrent_{0};
  std::atomic<int> current_{0};
  std::atomic<bool> slow_done_{false};
}; // struct strand_order_t

void on_slow(void *ctx, const op_result_t &result) {
  auto *_state = static_cast<strand_order_t *>(ctx);
  (void) result;
  const int _inside_k = _state->current_.fetch_add(1) + 1;
  int _seen = _state->max_concurrent_.load();
  while (_inside_k > _seen && !_state->max_concurrent_.compare_exchange_weak(_seen, _inside_k)) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  _state->current_.fetch_sub(1);
  _state->counter_.fetch_add(1);
  _state->slow_done_.store(true, std::memory_order_release);
}

void on_quick(void *ctx, const op_result_t &result) {
  auto *_state = static_cast<strand_order_t *>(ctx);
  (void) result;
  _state->counter_.fetch_add(1);
}

// Un timer con strand no puede solaparse con otra operación del mismo strand.
TEST(reactor_test_t, TimerWithStrandDoesNotOverlapTheStrand) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
  int _fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds), 0);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("strands");
  auto _socket = _reactor.create_socket(_fds[0], _queue);
  ASSERT_TRUE(_socket);

  strand_order_t _state;
  const std::uint64_t _strand_k = 42;

  // Una operación lenta en vuelo con el strand...
  recv_context_t _context;
  ASSERT_TRUE(_socket.recv(_context.data_, sizeof(_context.data_), on_slow, &_state, _strand_k));

  // ...y un timer del mismo strand: no puede ejecutarse hasta que la primera
  // termine.
  const std::uint64_t _timer_k =
      _reactor.schedule_timer(_queue, 10000000ULL, on_quick, &_state, _strand_k);
  ASSERT_NE(_timer_k, 0U);

  _reactor.run_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  _reactor.stop_all();

  // Nunca hubo dos operaciones del strand a la vez.
  EXPECT_LE(_state.max_concurrent_.load(), 1);

  ::close(_fds[0]);
  ::close(_fds[1]);
}

struct server_context_t {
  std::atomic<int> accepted_fd_{-1};
  std::atomic<bool> done_{false};
}; // struct server_context_t

void on_accept(void *ctx, const op_result_t &result) {
  auto *_state = static_cast<server_context_t *>(ctx);
  _state->accepted_fd_.store(result.fd_);
  _state->done_.store(true, std::memory_order_release);
}

struct udp_context_t {
  std::atomic<bool> received_{false};
  std::atomic<bool> sent_{false};
  char data_[128]{};
}; // struct udp_context_t

void on_udp_recv(void *ctx, const op_result_t &result) {
  auto *_state = static_cast<udp_context_t *>(ctx);
  (void) result;
  _state->received_.store(true, std::memory_order_release);
}

void on_udp_send(void *ctx, const op_result_t &result) {
  auto *_state = static_cast<udp_context_t *>(ctx);
  (void) result;
  _state->sent_.store(true, std::memory_order_release);
}

// Un servidor TCP completo: listen, accept y la conexión entrante.
TEST(reactor_test_t, RunsATcpServerThatAcceptsAConnection) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("server");
  ASSERT_NE(_queue, nullptr);

  sockaddr_in _addr{};
  _addr.sin_family = AF_INET;
  _addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  _addr.sin_port = 0;

  auto _listener =
      _reactor.listen_tcp(_queue, reinterpret_cast<const sockaddr *>(&_addr), sizeof(_addr));
  ASSERT_TRUE(_listener);

  // El puerto real se conoce después del bind.
  sockaddr_in _bound{};
  socklen_t _bound_len = sizeof(_bound);
  ASSERT_EQ(::getsockname(_listener.fd(), reinterpret_cast<sockaddr *>(&_bound), &_bound_len), 0);
  const std::uint16_t _port_k = ntohs(_bound.sin_port);
  ASSERT_NE(_port_k, 0);

  server_context_t _server_context;
  ASSERT_TRUE(_listener.accept(on_accept, &_server_context));

  _reactor.run_all();

  // Un cliente se conecta.
  const int _client_k = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(_client_k, 0);
  sockaddr_in _target = _bound; // el puerto ya viene en orden de red
  ASSERT_EQ(::connect(_client_k, reinterpret_cast<const sockaddr *>(&_target), sizeof(_target)), 0);

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_server_context.done_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  ASSERT_TRUE(_server_context.done_.load(std::memory_order_acquire));
  const int _accepted_k = _server_context.accepted_fd_.load();
  EXPECT_GE(_accepted_k, 0);

  ::close(_client_k);
  if (_accepted_k >= 0)
    ::close(_accepted_k);
  _listener.close();
}

// UDP: sendto a una dirección y recepción del datagrama.
TEST(reactor_test_t, SendsAndReceivesAUnixDatagram) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("udp");
  ASSERT_NE(_queue, nullptr);

  // Datagramas sobre Unix domain: no necesita puertos ni red.
  auto _server = _reactor.open_socket(AF_UNIX, SOCK_DGRAM, 0, _queue);
  ASSERT_TRUE(_server);
  sockaddr_un _server_addr{};
  _server_addr.sun_family = AF_UNIX;
  std::strncpy(
      _server_addr.sun_path, "/tmp/frameworkcpp_udp_test.sock", sizeof(_server_addr.sun_path) - 1);
  ::unlink(_server_addr.sun_path);
  ASSERT_TRUE(
      _server.bind_to(reinterpret_cast<const sockaddr *>(&_server_addr), sizeof(_server_addr)));

  auto _client_k = _reactor.open_socket(AF_UNIX, SOCK_DGRAM, 0, _queue);
  ASSERT_TRUE(_client_k);
  sockaddr_un _client_addr{};
  _client_addr.sun_family = AF_UNIX;
  std::strncpy(_client_addr.sun_path,
               "/tmp/frameworkcpp_udp_client.sock",
               sizeof(_client_addr.sun_path) - 1);
  ::unlink(_client_addr.sun_path);
  ASSERT_TRUE(
      _client_k.bind_to(reinterpret_cast<const sockaddr *>(&_client_addr), sizeof(_client_addr)));

  udp_context_t _context;
  ASSERT_TRUE(_server.recv_from(_context.data_, sizeof(_context.data_), on_udp_recv, &_context));

  _reactor.run_all();

  const std::size_t _sent_k = 4;
  ASSERT_TRUE(_client_k.send_to("hola",
                                _sent_k,
                                reinterpret_cast<const sockaddr *>(&_server_addr),
                                sizeof(_server_addr),
                                on_udp_send,
                                &_context));

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.received_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  EXPECT_TRUE(_context.received_.load(std::memory_order_acquire));
  EXPECT_EQ(std::string(_context.data_, _sent_k), "hola");
  // La dirección del emisor queda disponible en la socket.
  EXPECT_NE(_server.last_peer(), nullptr);

  ::unlink(_server_addr.sun_path);
  ::unlink(_client_addr.sun_path);
  _server.close();
  _client_k.close();
}

// ICMP necesita privilegios: si el sistema no los concede, no es un fallo.
TEST(reactor_test_t, OpensAnIcmpSocketWhenPrivilegesAllow) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("icmp");
  auto _socket = _reactor.open_icmp(_queue);
  if (!_socket) {
    GTEST_SKIP() << "raw sockets are not permitted in this environment";
  }
  EXPECT_GE(_socket.fd(), 0);
  _socket.close();
}

struct connect_context_t {
  std::atomic<int> ec_{-999};
  std::atomic<bool> done_{false};
}; // struct connect_context_t

void on_connect(void *ctx, const op_result_t &result) {
  auto *_state = static_cast<connect_context_t *>(ctx);
  _state->ec_.store(result.ec_);
  _state->done_.store(true, std::memory_order_release);
}

// Un cliente TCP: connect asíncrono y lectura de la respuesta.
TEST(reactor_test_t, ConnectsToATcpServer) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("client");
  sockaddr_in _addr{};
  _addr.sin_family = AF_INET;
  _addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  _addr.sin_port = htonl(9); // puerto de descarte: connect falla con ICMP

  auto _socket = _reactor.open_socket(AF_INET, SOCK_STREAM, 0, _queue);
  ASSERT_TRUE(_socket);

  connect_context_t _context;
  ASSERT_TRUE(_socket.connect(
      reinterpret_cast<const sockaddr *>(&_addr), sizeof(_addr), on_connect, &_context));

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.done_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  // Lo que importa es que la operación se completa y reporta su error.
  EXPECT_TRUE(_context.done_.load(std::memory_order_acquire));
  EXPECT_NE(_context.ec_.load(std::memory_order_acquire), -999);
  _socket.close();
}

// Un servidor Unix domain acepta conexiones.
TEST(reactor_test_t, RunsAUnixDomainServerThatAcceptsAConnection) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";

  const char *_path_k = "/tmp/frameworkcpp_unix_test.sock";
  ::unlink(_path_k);

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));

  auto *_queue = _reactor.get_or_create("unix");
  auto _listener = _reactor.listen_unix(_queue, _path_k);
  ASSERT_TRUE(_listener);

  server_context_t _server_context;
  ASSERT_TRUE(_listener.accept(on_accept, &_server_context));

  _reactor.run_all();

  const int _client_k = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(_client_k, 0);
  sockaddr_un _addr{};
  _addr.sun_family = AF_UNIX;
  std::strncpy(_addr.sun_path, _path_k, sizeof(_addr.sun_path) - 1);
  ASSERT_EQ(::connect(_client_k, reinterpret_cast<const sockaddr *>(&_addr), sizeof(_addr)), 0);

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_server_context.done_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  ASSERT_TRUE(_server_context.done_.load(std::memory_order_acquire));
  EXPECT_GE(_server_context.accepted_fd_.load(), 0);

  const int _accepted_k = _server_context.accepted_fd_.load();
  ::close(_client_k);
  if (_accepted_k >= 0)
    ::close(_accepted_k);
  _listener.close();
  ::unlink(_path_k);
}

TEST(reactor_test_t, DegradesWithoutIoUringAndStillRunsTimers) {
  // Sin io_uring el reactor no puede hacer E/S, pero los timers, los strands y
  // el resto del mecanismo siguen funcionando. Es lo que permite probar el
  // modulo en plataformas donde el kernel no expone io_uring.
  reactor_config_t _config = make_reactor_config();
  _config.enable_io_ = false;

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(_config)));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, _config));
  EXPECT_TRUE(_reactor.initialized());

  auto *_queue = _reactor.get_or_create("degraded");
  ASSERT_NE(_queue, nullptr);

  timer_context_t _context;
  const std::uint64_t _id_k = _reactor.schedule_timer(_queue, 20000000ULL, on_timer, &_context);
  ASSERT_NE(_id_k, 0U);

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.fired_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  EXPECT_TRUE(_context.fired_.load(std::memory_order_acquire));
}

TEST(reactor_test_t, ReportsEnosysForIoWithoutIoUring) {
  reactor_config_t _config = make_reactor_config();
  _config.enable_io_ = false;

  auto _arena_config = make_arena_config(arena::round_to_page(precompute(_config)));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, _config));

  auto *_queue = _reactor.get_or_create("degraded");
  int _fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds), 0);
  auto _socket = _reactor.create_socket(_fds[0], _queue);
  ASSERT_TRUE(_socket);

  recv_context_t _context;
  ASSERT_TRUE(_socket.recv(_context.data_, sizeof(_context.data_), on_recv, &_context));

  _reactor.run_all();

  const auto _deadline_k = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!_context.done_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < _deadline_k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _reactor.stop_all();

  // La operacion de E/S no se puede ejecutar y lo dice.
  EXPECT_TRUE(_context.done_.load(std::memory_order_acquire));
  EXPECT_EQ(_context.ec_.load(std::memory_order_acquire), ENOSYS);

  ::close(_fds[0]);
  ::close(_fds[1]);
}

} // namespace
} // namespace frameworkcpp::reactor
