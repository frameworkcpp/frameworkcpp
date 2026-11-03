// SPDX-License-Identifier: AGPL-3.0-only

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "frameworkcpp/concurrency.hpp"
#include "frameworkcpp/memory/memory.hpp"

#include "detail/stats.hpp"
#include "detail/workers.hpp"

namespace {

using frameworkcpp::memory::concurrency_model_t;
using frameworkcpp::memory::memory_config_t;
using frameworkcpp::memory::memory_t;
using frameworkcpp::memory::round_to_page;

constexpr std::size_t block_size_k = 64;
constexpr std::size_t block_count_k = 4096;
constexpr std::size_t shard_tag_k = 1;
constexpr std::size_t tagged_tag_k = 7;
constexpr std::size_t bucket_tag_k = 3;
constexpr std::size_t mt_lock_free_tag_k = 6;
constexpr std::size_t mt_thread_safe_tag_k = 5;

memory_t make_memory() {
  memory_config_t _config;
  _config.size_ = round_to_page(block_size_k * block_count_k * 8);
  return memory_t{_config};
}

// --- single-thread benchmarks -------------------------------------------------

void bench_allocate_deallocate(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(shard_tag_k,
                                     {{0, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  frameworkcpp::bench::histogram_t _histogram;
  while (state.KeepRunning()) {
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    auto *_block = _shard.allocate_bytes(block_size_k, alignof(std::max_align_t));
    benchmark::DoNotOptimize(_block);
    _shard.deallocate_bytes(_block);
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    _histogram.record(_t1_k - _t0_k);
  }
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

void bench_allocate_batch(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(shard_tag_k,
                                     {{0, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  const auto _batch_k = static_cast<std::size_t>(state.range(0));
  std::byte *_blocks[block_count_k] = {};
  frameworkcpp::bench::histogram_t _histogram;

  while (state.KeepRunning()) {
    for (std::size_t _index = 0; _index < _batch_k; ++_index) {
      const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
      _blocks[_index] = _shard.allocate_bytes(block_size_k, alignof(std::max_align_t));
      const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
      _histogram.record(_t1_k - _t0_k);
    }
    benchmark::DoNotOptimize(_blocks);
    for (std::size_t _index = 0; _index < _batch_k; ++_index)
      _shard.deallocate_bytes(_blocks[_index]);
  }
  const auto _items_k = static_cast<std::uint64_t>(state.iterations()) * _batch_k;
  frameworkcpp::bench::report_histogram(state, _histogram, _items_k);
}

void bench_allocate_tagged(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(
      shard_tag_k, {{tagged_tag_k, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  frameworkcpp::bench::histogram_t _histogram;
  while (state.KeepRunning()) {
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    auto *_block = _shard.allocate_tagged(tagged_tag_k, block_size_k, alignof(std::max_align_t));
    benchmark::DoNotOptimize(_block);
    _shard.deallocate_bytes(_block);
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    _histogram.record(_t1_k - _t0_k);
  }
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

void bench_bucket_acquire_release(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(
      shard_tag_k, {{bucket_tag_k, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  auto _bucket = _shard.get_bucket(bucket_tag_k, concurrency_model_t::single_thread_);
  if (!_bucket)
    state.SkipWithError("bucket lookup failed");

  frameworkcpp::bench::histogram_t _histogram;
  while (state.KeepRunning()) {
    const std::uint64_t _t0_k = frameworkcpp::bench::cycle_count();
    auto *_block = _bucket.allocate<std::byte>(block_size_k);
    benchmark::DoNotOptimize(_block);
    _bucket.deallocate_bytes(_block);
    const std::uint64_t _t1_k = frameworkcpp::bench::cycle_count();
    _histogram.record(_t1_k - _t0_k);
  }
  frameworkcpp::bench::report_histogram(
      state, _histogram, static_cast<std::uint64_t>(state.iterations()));
}

// --- multi-thread benchmarks --------------------------------------------------
//
// N workers piden y devuelven el mismo bucket. El pool persiste entre
// iteraciones: una iteración del benchmark = una tanda de postes en paralelo,
// y los histogramas de los workers se suman al final.

constexpr std::size_t mt_ops_per_worker_k = 2048;

struct bucket_holder_t {
  memory_t memory_;
  frameworkcpp::memory::memory_shard_t shard_;
  frameworkcpp::memory::memory_bucket_t lock_free_;
  frameworkcpp::memory::memory_bucket_t thread_safe_;

  explicit bucket_holder_t()
      : memory_{[] {
          memory_config_t _config;
          _config.size_ = round_to_page(block_size_k * block_count_k * 8);
          return _config;
        }()} {
    auto _shard = memory_.create_shard(
        99,
        {{mt_lock_free_tag_k, block_size_k, alignof(std::max_align_t), block_count_k},
         {mt_thread_safe_tag_k, block_size_k, alignof(std::max_align_t), block_count_k}});
    shard_ = _shard;
    if (_shard) {
      lock_free_ = _shard.get_bucket(mt_lock_free_tag_k, concurrency_model_t::lock_free_);
      thread_safe_ = _shard.get_bucket(mt_thread_safe_tag_k, concurrency_model_t::thread_safe_);
    }
  }
}; // struct bucket_holder_t

bucket_holder_t &bucket_holder() {
  static bucket_holder_t _holder_k;
  return _holder_k;
}

// --- bucket propio por hilo (single_thread_) -------------------------------
//
// N shards con un bucket por hilo, cada uno con su propio pool: mide cuánto
// escala el allocator cuando cada hilo posee su bucket (el diseño recomendado
// para RT), sin contención en ningún lock ni CAS compartido.

constexpr std::array<std::uint64_t, 4> private_shard_tags_k = {31, 32, 33, 34};
constexpr std::array<std::uint64_t, 4> private_bucket_tags_k = {41, 42, 43, 44};

struct private_bucket_holder_t {
  memory_t memory_;
  std::array<frameworkcpp::memory::memory_shard_t, 4> shards_{};
  std::array<frameworkcpp::memory::memory_bucket_t, 4> buckets_{};

  private_bucket_holder_t()
      : memory_{[] {
          memory_config_t _config;
          _config.size_ = round_to_page(block_size_k * block_count_k * 32);
          return _config;
        }()} {
    for (std::size_t _i = 0; _i < 4; ++_i) {
      shards_[_i] = memory_.create_shard(
          private_shard_tags_k[_i],
          {{private_bucket_tags_k[_i], block_size_k, alignof(std::max_align_t), block_count_k}});
      buckets_[_i] =
          shards_[_i].get_bucket(private_bucket_tags_k[_i], concurrency_model_t::single_thread_);
    }
  }
}; // struct private_bucket_holder_t

private_bucket_holder_t &private_bucket_holder() {
  static private_bucket_holder_t _holder_k;
  return _holder_k;
}

template <std::size_t ThreadCount> void bench_bucket_private(benchmark::State &state) {
  auto &_holder_k = private_bucket_holder();
  for (std::size_t _i = 0; _i < ThreadCount; ++_i)
    if (!_holder_k.buckets_[_i])
      state.SkipWithError("private bucket lookup failed");

  static frameworkcpp::bench::workers_t<ThreadCount> _pool_k;

  while (state.KeepRunning()) {
    const auto _t0_k = std::chrono::steady_clock::now();
    _pool_k.execute([&_holder_k](std::size_t index, frameworkcpp::bench::histogram_t &hist) {
      auto &_bucket_k = _holder_k.buckets_[index];
      for (std::size_t _op = 0; _op < mt_ops_per_worker_k; ++_op) {
        const std::uint64_t _op0_k = frameworkcpp::bench::cycle_count();
        auto *_block = _bucket_k.allocate<std::byte>(block_size_k);
        benchmark::DoNotOptimize(_block);
        _bucket_k.deallocate_bytes(_block);
        const std::uint64_t _op1_k = frameworkcpp::bench::cycle_count();
        hist.record(_op1_k - _op0_k);
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

template <concurrency_model_t Model, std::size_t ThreadCount>
void bench_bucket_parallel(benchmark::State &state) {
  auto &_holder_k = bucket_holder();
  if (!_holder_k.lock_free_ || !_holder_k.thread_safe_)
    state.SkipWithError("bucket setup failed");

  static frameworkcpp::bench::workers_t<ThreadCount> _pool_k;
  const bool _lock_free_k = Model == concurrency_model_t::lock_free_;

  while (state.KeepRunning()) {
    const auto _t0_k = std::chrono::steady_clock::now();
    _pool_k.execute([&](std::size_t index, frameworkcpp::bench::histogram_t &hist) {
      auto _bucket = _lock_free_k ? _holder_k.lock_free_ : _holder_k.thread_safe_;
      for (std::size_t _op = 0; _op < mt_ops_per_worker_k; ++_op) {
        const std::uint64_t _op0_k = frameworkcpp::bench::cycle_count();
        auto *_block = _bucket.allocate<std::byte>(block_size_k);
        benchmark::DoNotOptimize(_block);
        _bucket.deallocate_bytes(_block);
        const std::uint64_t _op1_k = frameworkcpp::bench::cycle_count();
        hist.record(_op1_k - _op0_k);
      }
      (void) index;
    });
    const auto _t1_k = std::chrono::steady_clock::now();
    const frameworkcpp::bench::histogram_t _hist_k = _pool_k.merged();
    _pool_k.reset();

    const double _seconds_k = std::chrono::duration<double>(_t1_k - _t0_k).count();
    const std::uint64_t _items_k = static_cast<std::uint64_t>(ThreadCount) * mt_ops_per_worker_k;
    state.counters["aggregate_items_per_second"] = static_cast<double>(_items_k) / _seconds_k;
    frameworkcpp::bench::report_histogram(state, _hist_k, _items_k);
  }
}

} // namespace

BENCHMARK(bench_allocate_deallocate)->Name("memory/allocate_deallocate");
BENCHMARK(bench_allocate_batch)->Name("memory/allocate_batch")->Arg(16)->Arg(256)->Arg(4096);
BENCHMARK(bench_allocate_tagged)->Name("memory/allocate_tagged");
BENCHMARK(bench_bucket_acquire_release)->Name("memory/bucket_acquire_release");
BENCHMARK_TEMPLATE(bench_bucket_parallel, concurrency_model_t::lock_free_, 1)
    ->Name("memory/bucket_lock_free_mt/1");
BENCHMARK_TEMPLATE(bench_bucket_parallel, concurrency_model_t::lock_free_, 2)
    ->Name("memory/bucket_lock_free_mt/2");
BENCHMARK_TEMPLATE(bench_bucket_parallel, concurrency_model_t::lock_free_, 4)
    ->Name("memory/bucket_lock_free_mt/4");
BENCHMARK_TEMPLATE(bench_bucket_parallel, concurrency_model_t::thread_safe_, 1)
    ->Name("memory/bucket_thread_safe_mt/1");
BENCHMARK_TEMPLATE(bench_bucket_parallel, concurrency_model_t::thread_safe_, 2)
    ->Name("memory/bucket_thread_safe_mt/2");
BENCHMARK_TEMPLATE(bench_bucket_parallel, concurrency_model_t::thread_safe_, 4)
    ->Name("memory/bucket_thread_safe_mt/4");
BENCHMARK_TEMPLATE(bench_bucket_private, 1)->Name("memory/bucket_private_mt/1");
BENCHMARK_TEMPLATE(bench_bucket_private, 2)->Name("memory/bucket_private_mt/2");
BENCHMARK_TEMPLATE(bench_bucket_private, 4)->Name("memory/bucket_private_mt/4");

BENCHMARK_MAIN();
