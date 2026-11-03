// SPDX-License-Identifier: AGPL-3.0-only

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdlib>
#include <string_view>

#include "frameworkcpp/environment/environment.hpp"
#include "frameworkcpp/memory/memory.hpp"

#include "detail/stats.hpp"
#include "detail/workers.hpp"

namespace {

using frameworkcpp::environment::environment_config_t;
using frameworkcpp::environment::get_env;
using frameworkcpp::environment::get_environment_memory_config;
using frameworkcpp::environment::init;
using frameworkcpp::environment::set_env;
using frameworkcpp::environment::shutdown;
using frameworkcpp::memory::memory_t;

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

// Cada bench levanta su propia instancia del environment. El módulo es un
// store global con un escritor serializado y lectores lock-free con views
// por hilo; mezclar varias benches contra el mismo store corrompe las
// métricas (los views pinneados del lector impiden reclamar las celdas
// retiradas por el escritor). Una instancia por bench lo desacopla: el coste
// de setup es por-proceso, no por op medida.
struct bench_env_t {
  bench_env_t(const bench_env_t &) = delete;
  bench_env_t &operator=(const bench_env_t &) = delete;
  bench_env_t(bench_env_t &&) = delete;
  bench_env_t &operator=(bench_env_t &&) = delete;

  memory_t memory_;

  bench_env_t() : memory_{get_environment_memory_config(make_config())} {
    const environment_config_t _config_k = make_config();
    init(memory_, _config_k);
    if (!set_env(key_k, value_k))
      std::abort();
  }

  ~bench_env_t() { shutdown(); }
}; // struct bench_env_t

// --- single-thread benchmarks -------------------------------------------------

void bench_get_env(benchmark::State &state) {
  const bench_env_t _env_k;
  frameworkcpp::bench::histogram_t _histogram;
  while (state.KeepRunning()) {
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    auto _value = get_env(key_k, "");
    benchmark::DoNotOptimize(_value);
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    _histogram.record(_t1_k - _t0_k);
  }
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

void bench_set_env(benchmark::State &state) {
  const bench_env_t _env_k;
  frameworkcpp::bench::histogram_t _histogram;
  while (state.KeepRunning()) {
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    if (!set_env(key_k, value_k)) {
      state.SkipWithError("set_env failed");
      break;
    }
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    _histogram.record(_t1_k - _t0_k);
  }
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

// --- multi-thread benchmarks -------------------------------------------------
//
// N workers hacen get_env contra el mismo table (los lectores escalan por
// su view por hilo). Cada tanda agrega los histogramas de los workers.

constexpr std::size_t mt_ops_per_worker_k = 2048;

template <std::size_t ThreadCount> void bench_get_env_parallel(benchmark::State &state) {
  const bench_env_t _env_k;
  static frameworkcpp::bench::workers_t<ThreadCount> _pool_k;

  while (state.KeepRunning()) {
    const auto _t0_k = std::chrono::steady_clock::now();
    _pool_k.execute([&](std::size_t index, frameworkcpp::bench::histogram_t &hist) {
      (void) index;
      for (std::size_t _op = 0; _op < mt_ops_per_worker_k; ++_op) {
        const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
        auto _value = get_env(key_k, "");
        benchmark::DoNotOptimize(_value);
        const std::uint64_t _t2_k = frameworkcpp::bench::cycle_count();
        hist.record(_t2_k - _t1_k);
      }
    });
    const auto _t1_k = std::chrono::steady_clock::now();

    const double _seconds_k = std::chrono::duration<double>(_t1_k - _t0_k).count();
    const std::uint64_t _items_k = static_cast<std::uint64_t>(ThreadCount) * mt_ops_per_worker_k;
    const frameworkcpp::bench::histogram_t _hist_k = _pool_k.merged();
    _pool_k.reset();

    state.counters["aggregate_items_per_second"] = static_cast<double>(_items_k) / _seconds_k;
    frameworkcpp::bench::report_histogram(state, _hist_k, _items_k);
  }
}

template <std::size_t ThreadCount> void bench_set_env_parallel(benchmark::State &state) {
  const bench_env_t _env_k;
  static frameworkcpp::bench::workers_t<ThreadCount> _pool_k;

  while (state.KeepRunning()) {
    const auto _t0_k = std::chrono::steady_clock::now();
    _pool_k.execute([&](std::size_t index, frameworkcpp::bench::histogram_t &hist) {
      (void) index;
      for (std::size_t _op = 0; _op < mt_ops_per_worker_k; ++_op) {
        const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
        const bool _ok_k = set_env(key_k, value_k);
        const std::uint64_t _t2_k = frameworkcpp::bench::cycle_count();
        if (!_ok_k) {
          state.SkipWithError("set_env failed");
          return;
        }
        hist.record(_t2_k - _t1_k);
      }
    });
    const auto _t1_k = std::chrono::steady_clock::now();

    const double _seconds_k = std::chrono::duration<double>(_t1_k - _t0_k).count();
    const std::uint64_t _items_k = static_cast<std::uint64_t>(ThreadCount) * mt_ops_per_worker_k;
    const frameworkcpp::bench::histogram_t _hist_k = _pool_k.merged();
    _pool_k.reset();

    state.counters["aggregate_items_per_second"] = static_cast<double>(_items_k) / _seconds_k;
    frameworkcpp::bench::report_histogram(state, _hist_k, _items_k);
  }
}

} // namespace

BENCHMARK(bench_get_env)->Name("environment/get_env");
BENCHMARK(bench_set_env)->Name("environment/set_env");
BENCHMARK_TEMPLATE(bench_get_env_parallel, 1)->Name("environment/get_env_mt/1");
BENCHMARK_TEMPLATE(bench_get_env_parallel, 2)->Name("environment/get_env_mt/2");
BENCHMARK_TEMPLATE(bench_get_env_parallel, 4)->Name("environment/get_env_mt/4");
BENCHMARK_TEMPLATE(bench_set_env_parallel, 1)->Name("environment/set_env_mt/1");
BENCHMARK_TEMPLATE(bench_set_env_parallel, 2)->Name("environment/set_env_mt/2");
BENCHMARK_TEMPLATE(bench_set_env_parallel, 4)->Name("environment/set_env_mt/4");

BENCHMARK_MAIN();
