// SPDX-License-Identifier: AGPL-3.0-only

// Translation unit used by tools/cross-check.sh.
//
// It exercises the public API of every module so the compiler has to
// instantiate the real code, not just parse the headers: the memory stores, the
// views, the queues, the sockets, the timers and the strands. Its real job is
// to make the compiler evaluate the static_asserts that state the
// real-time guarantees of the framework, on the architecture being checked.
//
// It is compiled, not run: the point is that it compiles.

#include <cstddef>
#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>

#include "frameworkcpp/memory/memory.hpp"
#include "frameworkcpp/environment/environment.hpp"
#include "frameworkcpp/reactor/reactor.hpp"

using namespace frameworkcpp;

namespace {

struct state_t {
  int ec_ = 0;
  int fd_ = -1;
  char buffer_[256]{};
}; // struct state_t

void on_accept(void *ctx, const reactor::op_result_t &result) {
  auto *_state = static_cast<state_t *>(ctx);
  _state->ec_ = result.ec_;
  _state->fd_ = result.fd_;
}

void on_read(void *ctx, const reactor::op_result_t &result) {
  auto *_state = static_cast<state_t *>(ctx);
  _state->ec_ = result.ec_;
}

} // namespace

int main() {
  constexpr environment::environment_config_t env_config{};
  constexpr reactor::reactor_config_t reactor_config{};

  // El presupuesto de memoria se computa en compile time.
  constexpr std::size_t env_bytes_k = environment::precompute(env_config);
  constexpr std::size_t reactor_bytes_k = reactor::precompute(reactor_config);
  constexpr std::size_t total_k = env_bytes_k + reactor_bytes_k;
  static_assert(total_k > 0, "the modules must declare a footprint");

  memory::memory_config_t memory_config{};
  memory_config.size_ = memory::round_to_page(total_k);
  memory::memory_t memory{memory_config};

  environment::init(memory, env_config);

  reactor::reactor_t reactor;
  if (!reactor.bind_memory(memory, reactor_config))
    return 1;

  auto *_queue = reactor.get_or_create("cross");
  auto *_same = reactor.get_or_create("cross");
  if (_queue == nullptr || _queue != _same)
    return 2;

  int _fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds) != 0)
    return 3;

  auto _socket = reactor.create_socket(_fds[0], _queue);
  if (!_socket)
    return 4;

  state_t _state{};
  _socket.recv(_state.buffer_, sizeof(_state.buffer_), on_read, &_state);
  _socket.accept(on_accept, &_state);
  (void) reactor.schedule_timer(_queue, 1000000ULL, on_read, &_state);
  (void) reactor.rebalance();
  (void) reactor.get_socket_count(_queue);
  (void) reactor.get_timer_count(_queue);
  (void) reactor.get_backend();

  reactor.run_all();
  reactor.stop_all();

  environment::shutdown();
  ::close(_fds[0]);
  ::close(_fds[1]);
  return 0;
}
