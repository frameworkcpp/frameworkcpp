// SPDX-License-Identifier: AGPL-3.0-only

#include <benchmark/benchmark.h>

#include <string_view>

#include "frameworkcpp/environment/environment.hpp"
#include "frameworkcpp/memory/memory.hpp"

namespace {

using frameworkcpp::environment::environment_config_t;
using frameworkcpp::environment::get_env;
using frameworkcpp::environment::get_environment_memory_config;
using frameworkcpp::environment::init;
using frameworkcpp::environment::set_env;
using frameworkcpp::environment::shutdown;
using frameworkcpp::memory::memory_t;

// The per-thread views point into the memory: the module handle has to go
// away before the memory does, or the TLS destructor touches freed memory.
struct shutdown_guard_t {
  shutdown_guard_t() = default;
  shutdown_guard_t(const shutdown_guard_t &) = delete;
  shutdown_guard_t &operator=(const shutdown_guard_t &) = delete;
  shutdown_guard_t(shutdown_guard_t &&) = delete;
  shutdown_guard_t &operator=(shutdown_guard_t &&) = delete;
  ~shutdown_guard_t() { shutdown(); }
}; // struct shutdown_guard_t

constexpr std::string_view key_k = "FRAMEWORKCPP_BENCH_KEY";
constexpr std::string_view value_k = "0123456789abcdef0123456789abcdef";

environment_config_t make_config() {
  environment_config_t _config;
  _config.max_keys_ = 64;
  _config.max_key_len_ = 64;
  _config.max_value_len_ = 256;
  _config.value_cells_ = 65536;
  _config.journal_size_ = 256;
  _config.max_threads_ = 8;
  _config.read_process_env_ = false;
  return _config;
}

void bench_get_env(benchmark::State &state) {
  while (state.KeepRunning()) {
    auto _value = get_env(key_k, "");
    benchmark::DoNotOptimize(_value);
  }
}

void bench_set_env(benchmark::State &state) {
  while (state.KeepRunning()) {
    if (!set_env(key_k, value_k)) {
      state.SkipWithError("set_env failed");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(bench_get_env)->Name("environment/get_env");
BENCHMARK(bench_set_env)->Name("environment/set_env");

int main(int argc, char **argv) {
  try {
    auto _config = make_config();
    memory_t _memory{get_environment_memory_config(_config)};
    const shutdown_guard_t _guard_k;
    init(_memory, _config);
    if (!set_env(key_k, value_k))
      return 1;

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
      return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
  } catch (...) {
    return 1;
  }
  return 0;
}
