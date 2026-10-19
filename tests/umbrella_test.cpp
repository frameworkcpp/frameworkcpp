// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/frameworkcpp.hpp"

namespace frameworkcpp {
namespace {

// Including the umbrella header must expose every module and its version entry
// point; this TU also gives clang-tidy a translation unit that reaches all
// public headers, including those no other test includes directly.
TEST(Umbrella, PullsInEveryModule) {
    arena::arena_t arena;
    auto shard = arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 8}});
    ASSERT_TRUE(shard);

    environment::environment_config_t config;
    config.read_process_env = false;
    arena::arena_t env_arena{environment::environment_arena_config(config)};
    environment::init(env_arena, nullptr, config);
    EXPECT_TRUE(environment::set_env("FC_UMBRELLA", "ok"));
    EXPECT_EQ(environment::get_env("FC_UMBRELLA", ""), "ok");
    environment::shutdown();

    EXPECT_STREQ(arena::version_string(), environment::version_string());
}

} // namespace
} // namespace frameworkcpp
