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
    arena::arena_config_t config;
    config.size = bytes;
    return arena::arena_t{config};
}

std::string write_dotenv(const char *contents) {
    char path[] = "/tmp/frameworkcpp_env_XXXXXX";
    const int fd = ::mkstemp(path);
    EXPECT_GE(fd, 0);
    const auto len = std::strlen(contents);
    EXPECT_EQ(::write(fd, contents, len), static_cast<ssize_t>(len));
    ::close(fd);
    return {path};
}

class EnvironmentTest : public ::testing::Test {
  protected:
    void TearDown() override { shutdown(); }
};

TEST_F(EnvironmentTest, ReadsProcessEnvironmentAndFallback) {
    ::setenv("FC_ENV_TEST_KEY", "from_process", 1);
    auto arena = make_arena();
    init(arena, nullptr);
    EXPECT_EQ(get_env("FC_ENV_TEST_KEY", "fallback"), "from_process");
    EXPECT_EQ(get_env("FC_ENV_TEST_MISSING", "fallback"), "fallback");
}

TEST_F(EnvironmentTest, ProcessWinsOverFile) {
    ::setenv("FC_ENV_TEST_KEY", "from_process", 1);
    const auto path = write_dotenv("FC_ENV_TEST_KEY=from_file\nFC_ENV_FILE_ONLY=file_value\n");
    auto arena = make_arena();
    init(arena, path.c_str());
    EXPECT_EQ(get_env("FC_ENV_TEST_KEY", ""), "from_process");
    EXPECT_EQ(get_env("FC_ENV_FILE_ONLY", ""), "file_value");
    ::unlink(path.c_str());
}

TEST_F(EnvironmentTest, SetEnvOverridesAndCreates) {
    auto arena = make_arena();
    init(arena, nullptr);
    EXPECT_EQ(get_env("FC_NEW", "default"), "default");
    EXPECT_TRUE(set_env("FC_NEW", "one"));
    EXPECT_EQ(get_env("FC_NEW", "default"), "one");
    EXPECT_TRUE(set_env("FC_NEW", "two"));
    EXPECT_EQ(get_env("FC_NEW", "default"), "two");
}

TEST_F(EnvironmentTest, RecyclesCellsWhileAReaderPinsThem) {
    environment_config_t config;
    config.read_process_env = false;
    config.max_keys = 8;
    config.value_cells = 2; // tiny pool: max_keys + value_cells == 10 cells total
    config.journal_size = 8;
    config.max_threads = 4;
    auto arena = make_arena();
    init(arena, nullptr, config);
    ASSERT_TRUE(set_env("FC_R", "seed"));

    std::atomic<bool> pinned{false};
    std::atomic<bool> release{false};
    std::thread pinner([&] {
        (void) get_env("FC_R", ""); // creates a view at the current version, then idles
        pinned.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        (void) get_env("FC_R", ""); // advances the view, unpinning retired cells
    });
    while (!pinned.load(std::memory_order_acquire))
        std::this_thread::yield();

    // A reader stuck at the initial version pins every retired cell, so the
    // value pool drains and writes eventually fail — the bounded back-pressure.
    int successes = 0;
    bool exhausted = false;
    for (int i = 0; i < 1000; ++i) {
        if (set_env("FC_R", std::to_string(i)))
            ++successes;
        else {
            exhausted = true;
            break;
        }
    }
    EXPECT_TRUE(exhausted);
    EXPECT_GT(successes, 0);

    // Once the pinner advances and exits, retired cells are recycled.
    release.store(true, std::memory_order_release);
    pinner.join();
    EXPECT_TRUE(set_env("FC_R", "after"));
    EXPECT_EQ(get_env("FC_R", ""), "after");
}

TEST_F(EnvironmentTest, ExhaustsKeys) {
    environment_config_t config;
    config.max_keys = 2;
    config.max_value_len = 32;
    config.read_process_env = false;
    auto arena = make_arena();
    init(arena, nullptr, config);
    EXPECT_TRUE(set_env("FC_K1", "a"));
    EXPECT_TRUE(set_env("FC_K2", "b"));
    EXPECT_FALSE(set_env("FC_K3", "c"));
}

TEST_F(EnvironmentTest, FootprintFitsShard) {
    const environment_config_t config;
    auto arena = make_arena(environment_footprint(config) + 4096);
    init(arena, nullptr, config);
    EXPECT_TRUE(set_env("FC_FOOTPRINT", "ok"));
    EXPECT_EQ(get_env("FC_FOOTPRINT", ""), "ok");
}

TEST_F(EnvironmentTest, ThreadsSeeTheirOwnWrite) {
    auto arena = make_arena();
    init(arena, nullptr);
    ASSERT_TRUE(set_env("FC_SHARED", "initial"));

    std::string seen_c;
    std::thread writer([&] { set_env("FC_SHARED", "updated"); });
    std::thread reader([&] { seen_c = std::string(get_env("FC_SHARED", "")); });
    writer.join();
    reader.join();

    EXPECT_EQ(get_env("FC_SHARED", ""), "updated");
    EXPECT_TRUE(seen_c == "initial" || seen_c == "updated");
}

TEST_F(EnvironmentTest, ConcurrentReadersStayCoherent) {
    environment_config_t config;
    config.value_cells = 256;
    config.journal_size = 64;
    config.max_threads = 32;
    auto arena = make_arena();
    init(arena, nullptr, config);
    ASSERT_TRUE(set_env("FC_HOT", "value-0"));

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::string value(get_env("FC_HOT", ""));
                if (!value.starts_with("value-"))
                    bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::string last = "value-0";
    for (int i = 1; i <= 3000; ++i) {
        const std::string candidate = "value-" + std::to_string(i);
        if (set_env("FC_HOT", candidate))
            last = candidate;
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto &reader : readers)
        reader.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(get_env("FC_HOT", ""), last);
}

TEST_F(EnvironmentTest, NotInitializedFallsBack) {
    shutdown();
    EXPECT_EQ(get_env("ANYTHING", "fallback"), "fallback");
    EXPECT_FALSE(set_env("ANYTHING", "x"));
}

TEST_F(EnvironmentTest, BudgetIsConstexpr) {
    constexpr environment_config_t config{};
    constexpr std::size_t footprint = environment_footprint(config);
    constexpr std::size_t budget = environment_arena_bytes(config);
    static_assert(footprint > 0, "footprint must be computable at compile time");
    static_assert(budget >= footprint, "the arena budget must cover the footprint");
    static_assert(budget % 4096 == 0, "the arena budget must be page aligned");

    constexpr environment_config_t small{.max_keys = 8, .value_cells = 8, .max_threads = 2};
    static_assert(environment_footprint(small) < footprint,
                  "a smaller config must have a smaller footprint");

    constexpr auto arena_config = environment_arena_config(config, 64 * 1024);
    static_assert(arena_config.size == environment_arena_bytes(config, 64 * 1024));

    // And the derived budget actually works at runtime.
    auto arena = arena::arena_t{environment_arena_config(config)};
    init(arena, nullptr, config);
    EXPECT_TRUE(set_env("FC_BUDGET", "ok"));
    EXPECT_EQ(get_env("FC_BUDGET", ""), "ok");
}

TEST_F(EnvironmentTest, EnumeratesKeys) {
    environment_config_t config;
    config.read_process_env = false;
    config.max_keys = 8;
    auto arena = make_arena();
    init(arena, nullptr, config);
    ASSERT_TRUE(set_env("FC_A", "1"));
    ASSERT_TRUE(set_env("FC_B", "2"));

    environment_entry_t entries[4] = {};
    const auto count = environment_keys(entries, 4);
    ASSERT_EQ(count, 2U);
    std::string seen;
    for (std::size_t i = 0; i < count; ++i)
        seen += std::string(entries[i].key) + "=" + std::string(entries[i].value) + ";";
    EXPECT_NE(seen.find("FC_A=1;"), std::string::npos);
    EXPECT_NE(seen.find("FC_B=2;"), std::string::npos);
}

TEST_F(EnvironmentTest, EnumerateRespectsCapacityAndNull) {
    environment_config_t config;
    config.read_process_env = false;
    config.max_keys = 8;
    auto arena = make_arena();
    init(arena, nullptr, config);
    ASSERT_TRUE(set_env("FC_X", "1"));
    ASSERT_TRUE(set_env("FC_Y", "2"));
    ASSERT_TRUE(set_env("FC_Z", "3"));

    environment_entry_t one[1] = {};
    EXPECT_EQ(environment_keys(one, 1), 1U);
    EXPECT_EQ(environment_keys(nullptr, 4), 0U);
}

TEST_F(EnvironmentTest, StatsReportUsage) {
    environment_config_t config;
    config.read_process_env = false;
    config.max_keys = 4;
    config.value_cells = 3;
    config.journal_size = 8;
    auto arena = make_arena();
    init(arena, nullptr, config);

    auto stats = environment_stats();
    EXPECT_EQ(stats.max_keys, 4U);
    EXPECT_EQ(stats.journal_size, 8U);
    EXPECT_EQ(stats.keys_used, 0U);
    EXPECT_EQ(stats.total_updates, 0U);
    EXPECT_EQ(stats.retired_pending, 0U);
    EXPECT_EQ(stats.max_threads, config.max_threads);
    EXPECT_EQ(stats.active_views, 0U);

    ASSERT_TRUE(set_env("FC_S1", "a"));
    ASSERT_TRUE(set_env("FC_S2", "b"));
    stats = environment_stats();
    EXPECT_EQ(stats.keys_used, 2U);
    EXPECT_EQ(stats.total_updates, 2U);
}

TEST_F(EnvironmentTest, RecyclesViewSlotsAcrossThreads) {
    environment_config_t config;
    config.read_process_env = false;
    config.max_keys = 4;
    config.journal_size = 8;
    config.max_threads = 1;
    auto arena = make_arena();
    init(arena, nullptr, config);
    ASSERT_TRUE(set_env("FC_V", "value"));

    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        (void) get_env("FC_V", ""); // claims the only view slot
        held.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    while (!held.load(std::memory_order_acquire))
        std::this_thread::yield();
    EXPECT_EQ(environment_stats().active_views, 1U);

    release.store(true, std::memory_order_release);
    holder.join();
    EXPECT_EQ(environment_stats().active_views, 0U); // freed on thread exit

    // The recycled slot is claimable again: a thread that ran before does not
    // leak a slot forever, it hands it back on exit.
    EXPECT_EQ(get_env("FC_V", ""), "value");
    EXPECT_EQ(environment_stats().active_views, 1U);
}

} // namespace
} // namespace frameworkcpp::environment