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

TEST_F(EnvironmentTest, ExhaustsUpdates) {
    environment_config_t config;
    config.max_keys = 8;
    config.max_updates = 3;
    config.read_process_env = false;
    auto arena = make_arena();
    init(arena, nullptr, config);
    EXPECT_TRUE(set_env("FC_A", "1"));
    EXPECT_TRUE(set_env("FC_B", "2"));
    EXPECT_TRUE(set_env("FC_C", "3"));
    EXPECT_FALSE(set_env("FC_D", "4"));
    EXPECT_EQ(get_env("FC_C", ""), "3");
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
    config.max_updates = 2048;
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
    for (int i = 1; i <= 500; ++i)
        set_env("FC_HOT", "value-" + std::to_string(i));
    stop.store(true, std::memory_order_relaxed);
    for (auto &reader : readers)
        reader.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(get_env("FC_HOT", ""), "value-500");
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

    constexpr environment_config_t small{.max_keys = 8, .max_updates = 8, .max_threads = 2};
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

TEST_F(EnvironmentTest, StatsReportUsage) {
    environment_config_t config;
    config.read_process_env = false;
    config.max_keys = 4;
    config.max_updates = 3;
    auto arena = make_arena();
    init(arena, nullptr, config);

    auto stats = environment_stats();
    EXPECT_EQ(stats.max_keys, 4U);
    EXPECT_EQ(stats.max_updates, 3U);
    EXPECT_EQ(stats.keys_used, 0U);
    EXPECT_EQ(stats.updates_used, 0U);
    EXPECT_EQ(stats.key_room(), 4U);
    EXPECT_EQ(stats.update_room(), 3U);

    ASSERT_TRUE(set_env("FC_S1", "a"));
    ASSERT_TRUE(set_env("FC_S2", "b"));
    stats = environment_stats();
    EXPECT_EQ(stats.keys_used, 2U);
    EXPECT_EQ(stats.updates_used, 2U);
    EXPECT_EQ(stats.update_room(), 1U);
}

} // namespace
} // namespace frameworkcpp::environment