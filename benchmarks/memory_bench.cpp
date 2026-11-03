// SPDX-License-Identifier: AGPL-3.0-only

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

#include "frameworkcpp/memory/memory.hpp"

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

memory_t make_memory() {
  memory_config_t _config;
  _config.size_ = round_to_page(block_size_k * block_count_k * 4);
  return memory_t{_config};
}

void bench_allocate_deallocate(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(shard_tag_k,
                                     {{0, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  while (state.KeepRunning()) {
    auto *_block = _shard.allocate_bytes(block_size_k, alignof(std::max_align_t));
    benchmark::DoNotOptimize(_block);
    _shard.deallocate_bytes(_block);
  }
  state.SetItemsProcessed(state.iterations());
}

void bench_allocate_batch(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(shard_tag_k,
                                     {{0, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  const auto _batch_k = static_cast<std::size_t>(state.range(0));
  std::byte *_blocks[block_count_k] = {};

  while (state.KeepRunning()) {
    for (std::size_t _index = 0; _index < _batch_k; ++_index)
      _blocks[_index] = _shard.allocate_bytes(block_size_k, alignof(std::max_align_t));
    benchmark::DoNotOptimize(_blocks);
    for (std::size_t _index = 0; _index < _batch_k; ++_index)
      _shard.deallocate_bytes(_blocks[_index]);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(_batch_k));
}

void bench_allocate_tagged(benchmark::State &state) {
  auto _memory = make_memory();
  auto _shard = _memory.create_shard(
      shard_tag_k, {{tagged_tag_k, block_size_k, alignof(std::max_align_t), block_count_k}});
  if (!_shard)
    state.SkipWithError("shard creation failed");

  while (state.KeepRunning()) {
    auto *_block = _shard.allocate_tagged(tagged_tag_k, block_size_k, alignof(std::max_align_t));
    benchmark::DoNotOptimize(_block);
    _shard.deallocate_bytes(_block);
  }
  state.SetItemsProcessed(state.iterations());
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

  while (state.KeepRunning()) {
    auto *_block = _bucket.allocate<std::byte>(block_size_k);
    benchmark::DoNotOptimize(_block);
    _bucket.deallocate_bytes(_block);
  }
  state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(bench_allocate_deallocate)->Name("memory/allocate_deallocate");
BENCHMARK(bench_allocate_batch)->Name("memory/allocate_batch")->Arg(16)->Arg(256)->Arg(4096);
BENCHMARK(bench_allocate_tagged)->Name("memory/allocate_tagged");
BENCHMARK(bench_bucket_acquire_release)->Name("memory/bucket_acquire_release");

BENCHMARK_MAIN();
