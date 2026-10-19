// SPDX-License-Identifier: AGPL-3.0-only

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "frameworkcpp/environment/environment.hpp"

namespace frameworkcpp::environment {
namespace {

// The per-thread cache is a trailing array; the header alone must never be
// mistaken for a whole view object.
static_assert(detail::view_object_bytes(4) > sizeof(detail::view_t));

arena::arena_t make_arena(std::size_t bytes = 16 * 1024 * 1024) {
  arena::arena_config_t _config;
  _config.size_ = bytes;
  return arena::arena_t{_config};
}

std::string write_dotenv(const char *contents) {
  char _path[] = "/tmp/frameworkcpp_env_XXXXXX";
  const int _file_k = ::mkstemp(_path);
  EXPECT_GE(_file_k, 0);
  const auto _len_k = std::strlen(contents);
  EXPECT_EQ(::write(_file_k, contents, _len_k), static_cast<ssize_t>(_len_k));
  ::close(_file_k);
  return {_path};
}

class environment_test_t : public ::testing::Test {
  protected:
  void TearDown() override { shutdown(); }
}; // class environment_test_t

TEST_F(environment_test_t, ReadsProcessEnvironmentAndFallback) {
  ::setenv("FC_ENV_TEST_KEY", "from_process", 1);
  auto _arena = make_arena();
  init(_arena, nullptr);
  EXPECT_EQ(get_env("FC_ENV_TEST_KEY", "fallback"), "from_process");
  EXPECT_EQ(get_env("FC_ENV_TEST_MISSING", "fallback"), "fallback");
}

TEST_F(environment_test_t, ProcessWinsOverFile) {
  ::setenv("FC_ENV_TEST_KEY", "from_process", 1);
  const auto _path_k = write_dotenv("FC_ENV_TEST_KEY=from_file\nFC_ENV_FILE_ONLY=file_value\n");
  auto _arena = make_arena();
  init(_arena, _path_k.c_str());
  EXPECT_EQ(get_env("FC_ENV_TEST_KEY", ""), "from_process");
  EXPECT_EQ(get_env("FC_ENV_FILE_ONLY", ""), "file_value");
  ::unlink(_path_k.c_str());
}

TEST_F(environment_test_t, SetEnvOverridesAndCreates) {
  auto _arena = make_arena();
  init(_arena, nullptr);
  EXPECT_EQ(get_env("FC_NEW", "default"), "default");
  EXPECT_TRUE(set_env("FC_NEW", "one"));
  EXPECT_EQ(get_env("FC_NEW", "default"), "one");
  EXPECT_TRUE(set_env("FC_NEW", "two"));
  EXPECT_EQ(get_env("FC_NEW", "default"), "two");
}

TEST_F(environment_test_t, RecyclesCellsWhileAReaderPinsThem) {
  environment_config_t _config;
  _config.read_process_env_ = false;
  _config.max_keys_ = 8;
  _config.value_cells_ = 2; // tiny pool: max_keys + value_cells == 10 cells total
  _config.journal_size_ = 8;
  _config.max_threads_ = 4;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);
  ASSERT_TRUE(set_env("FC_R", "seed"));

  std::atomic<bool> _pinned{false};
  std::atomic<bool> _release{false};
  std::thread _pinner([&] {
    (void) get_env("FC_R", ""); // creates a view at the current version, then idles
    _pinned.store(true, std::memory_order_release);
    while (!_release.load(std::memory_order_acquire))
      std::this_thread::yield();
    (void) get_env("FC_R", ""); // advances the view, unpinning retired cells
  });
  while (!_pinned.load(std::memory_order_acquire))
    std::this_thread::yield();

  // A reader stuck at the initial version pins every retired cell, so the
  // value pool drains and writes eventually fail — the bounded back-pressure.
  int _successes = 0;
  bool _exhausted = false;
  for (int _index = 0; _index < 1000; ++_index) {
    if (set_env("FC_R", std::to_string(_index)))
      ++_successes;
    else {
      _exhausted = true;
      break;
    }
  }
  EXPECT_TRUE(_exhausted);
  EXPECT_GT(_successes, 0);

  // Once the pinner advances and exits, retired cells are recycled.
  _release.store(true, std::memory_order_release);
  _pinner.join();
  EXPECT_TRUE(set_env("FC_R", "after"));
  EXPECT_EQ(get_env("FC_R", ""), "after");
}

TEST_F(environment_test_t, ExhaustsKeys) {
  environment_config_t _config;
  _config.max_keys_ = 2;
  _config.max_value_len_ = 32;
  _config.read_process_env_ = false;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);
  EXPECT_TRUE(set_env("FC_K1", "a"));
  EXPECT_TRUE(set_env("FC_K2", "b"));
  EXPECT_FALSE(set_env("FC_K3", "c"));
}

TEST_F(environment_test_t, FootprintFitsShard) {
  const environment_config_t _config_k;
  auto _arena = make_arena(get_environment_footprint(_config_k) + 4096);
  init(_arena, nullptr, _config_k);
  EXPECT_TRUE(set_env("FC_FOOTPRINT", "ok"));
  EXPECT_EQ(get_env("FC_FOOTPRINT", ""), "ok");
}

TEST_F(environment_test_t, ThreadsSeeTheirOwnWrite) {
  auto _arena = make_arena();
  init(_arena, nullptr);
  ASSERT_TRUE(set_env("FC_SHARED", "initial"));

  std::string _seen;
  std::thread _writer([&] { set_env("FC_SHARED", "updated"); });
  std::thread _reader([&] { _seen = std::string(get_env("FC_SHARED", "")); });
  _writer.join();
  _reader.join();

  EXPECT_EQ(get_env("FC_SHARED", ""), "updated");
  EXPECT_TRUE(_seen == "initial" || _seen == "updated");
}

TEST_F(environment_test_t, ConcurrentReadersStayCoherent) {
  environment_config_t _config;
  _config.value_cells_ = 256;
  _config.journal_size_ = 64;
  _config.max_threads_ = 32;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);
  ASSERT_TRUE(set_env("FC_HOT", "value-0"));

  std::atomic<bool> _stop{false};
  std::atomic<int> _bad{0};
  std::vector<std::thread> _readers;
  _readers.reserve(4);
  for (int _thread = 0; _thread < 4; ++_thread) {
    _readers.emplace_back([&] {
      while (!_stop.load(std::memory_order_relaxed)) {
        const std::string _value_k(get_env("FC_HOT", ""));
        if (!_value_k.starts_with("value-"))
          _bad.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::string _last = "value-0";
  for (int _index = 1; _index <= 3000; ++_index) {
    const std::string _candidate_k = "value-" + std::to_string(_index);
    if (set_env("FC_HOT", _candidate_k))
      _last = _candidate_k;
  }
  _stop.store(true, std::memory_order_relaxed);
  for (auto &_reader : _readers)
    _reader.join();
  EXPECT_EQ(_bad.load(), 0);
  EXPECT_EQ(get_env("FC_HOT", ""), _last);
}

TEST_F(environment_test_t, NotInitializedFallsBack) {
  shutdown();
  EXPECT_EQ(get_env("ANYTHING", "fallback"), "fallback");
  EXPECT_FALSE(set_env("ANYTHING", "x"));
}

TEST_F(environment_test_t, BudgetIsConstexpr) {
  constexpr environment_config_t _config_k{};
  constexpr std::size_t _footprint_k = get_environment_footprint(_config_k);
  constexpr std::size_t _budget_k = get_environment_arena_bytes(_config_k);
  static_assert(_footprint_k > 0, "footprint must be computable at compile time");
  static_assert(_budget_k >= _footprint_k, "the arena budget must cover the footprint");
  static_assert(_budget_k % 4096 == 0, "the arena budget must be page aligned");

  constexpr environment_config_t _small_k{.max_keys_ = 8, .value_cells_ = 8, .max_threads_ = 2};
  static_assert(get_environment_footprint(_small_k) < _footprint_k,
                "a smaller config must have a smaller footprint");

  constexpr auto _arena_config_k = get_environment_arena_config(_config_k, 64 * 1024);
  static_assert(_arena_config_k.size_ == get_environment_arena_bytes(_config_k, 64 * 1024));

  // And the derived budget actually works at runtime.
  auto _arena = arena::arena_t{get_environment_arena_config(_config_k)};
  init(_arena, nullptr, _config_k);
  EXPECT_TRUE(set_env("FC_BUDGET", "ok"));
  EXPECT_EQ(get_env("FC_BUDGET", ""), "ok");
}

TEST_F(environment_test_t, EnumeratesKeys) {
  environment_config_t _config;
  _config.read_process_env_ = false;
  _config.max_keys_ = 8;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);
  ASSERT_TRUE(set_env("FC_A", "1"));
  ASSERT_TRUE(set_env("FC_B", "2"));

  environment_entry_t _entries[4] = {};
  const auto _count_k = get_environment_keys(_entries, 4);
  ASSERT_EQ(_count_k, 2U);
  std::string _seen;
  for (std::size_t _index = 0; _index < _count_k; ++_index)
    _seen += std::string(_entries[_index].key_) + "=" + std::string(_entries[_index].value_) + ";";
  EXPECT_NE(_seen.find("FC_A=1;"), std::string::npos);
  EXPECT_NE(_seen.find("FC_B=2;"), std::string::npos);
}

TEST_F(environment_test_t, EnumerateRespectsCapacityAndNull) {
  environment_config_t _config;
  _config.read_process_env_ = false;
  _config.max_keys_ = 8;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);
  ASSERT_TRUE(set_env("FC_X", "1"));
  ASSERT_TRUE(set_env("FC_Y", "2"));
  ASSERT_TRUE(set_env("FC_Z", "3"));

  environment_entry_t _one[1] = {};
  EXPECT_EQ(get_environment_keys(_one, 1), 1U);
  EXPECT_EQ(get_environment_keys(nullptr, 4), 0U);
}

TEST_F(environment_test_t, StatsReportUsage) {
  environment_config_t _config;
  _config.read_process_env_ = false;
  _config.max_keys_ = 4;
  _config.value_cells_ = 3;
  _config.journal_size_ = 8;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);

  auto _stats = get_environment_stats();
  EXPECT_EQ(_stats.max_keys_, 4U);
  EXPECT_EQ(_stats.journal_size_, 8U);
  EXPECT_EQ(_stats.keys_used_, 0U);
  EXPECT_EQ(_stats.total_updates_, 0U);
  EXPECT_EQ(_stats.retired_pending_, 0U);
  EXPECT_EQ(_stats.max_threads_, _config.max_threads_);
  EXPECT_EQ(_stats.active_views_, 0U);

  ASSERT_TRUE(set_env("FC_S1", "a"));
  ASSERT_TRUE(set_env("FC_S2", "b"));
  _stats = get_environment_stats();
  EXPECT_EQ(_stats.keys_used_, 2U);
  EXPECT_EQ(_stats.total_updates_, 2U);
}

TEST_F(environment_test_t, RecyclesViewSlotsAcrossThreads) {
  environment_config_t _config;
  _config.read_process_env_ = false;
  _config.max_keys_ = 4;
  _config.journal_size_ = 8;
  _config.max_threads_ = 1;
  auto _arena = make_arena();
  init(_arena, nullptr, _config);
  ASSERT_TRUE(set_env("FC_V", "value"));

  std::atomic<bool> _held{false};
  std::atomic<bool> _release{false};
  std::thread _holder([&] {
    (void) get_env("FC_V", ""); // claims the only view slot
    _held.store(true, std::memory_order_release);
    while (!_release.load(std::memory_order_acquire))
      std::this_thread::yield();
  });
  while (!_held.load(std::memory_order_acquire))
    std::this_thread::yield();
  EXPECT_EQ(get_environment_stats().active_views_, 1U);

  _release.store(true, std::memory_order_release);
  _holder.join();
  EXPECT_EQ(get_environment_stats().active_views_, 0U); // freed on thread exit

  // The recycled slot is claimable again: a thread that ran before does not
  // leak a slot forever, it hands it back on exit.
  EXPECT_EQ(get_env("FC_V", ""), "value");
  EXPECT_EQ(get_environment_stats().active_views_, 1U);
}

} // namespace
} // namespace frameworkcpp::environment
