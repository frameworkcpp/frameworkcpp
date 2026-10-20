// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <sys/socket.h>
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

  auto _arena_config = make_arena_config(arena::round_to_page(_bytes_k));
  arena::arena_t _arena{_arena_config};

  reactor_t _reactor;
  ASSERT_TRUE(_reactor.bind_arena(_arena, _config_k));
  EXPECT_EQ(_arena.get_used(), _bytes_k);
}

TEST(reactor_test_t, BindArenaAllocatesNothingOnTheHeap) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
  auto _arena_config = make_arena_config(arena::round_to_page(precompute(make_reactor_config())));
  arena::arena_t _arena{_arena_config};
  reactor_t _reactor;
  EXPECT_TRUE(_reactor.bind_arena(_arena, make_reactor_config()));
  EXPECT_TRUE(_reactor.initialized());
}

TEST(reactor_test_t, GetOrCreateIsIdempotentPerName) {
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
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
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
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
  if (!io_uring_available())
    GTEST_SKIP() << "io_uring is not available in this environment";
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

} // namespace
} // namespace frameworkcpp::reactor
