// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/frameworkcpp.hpp"

namespace frameworkcpp {
namespace {

// Including the umbrella header must expose every module and its version entry
// point; this TU also gives clang-tidy a translation unit that reaches all
// public headers, including those no other test includes directly.
TEST(Umbrella, PullsInEveryModule) {
  arena::arena_t _arena;
  auto _shard = _arena.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 8}});
  ASSERT_TRUE(_shard);

  environment::environment_config_t _config;
  _config.read_process_env_ = false;
  arena::arena_t _env_arena{environment::get_environment_arena_config(_config)};
  environment::init(_env_arena, nullptr, _config);
  EXPECT_TRUE(environment::set_env("FC_UMBRELLA", "ok"));
  EXPECT_EQ(environment::get_env("FC_UMBRELLA", ""), "ok");
  environment::shutdown();

  EXPECT_STREQ(arena::get_version_string(), environment::get_version_string());
}

} // namespace
} // namespace frameworkcpp
