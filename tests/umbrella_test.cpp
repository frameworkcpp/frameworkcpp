// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/frameworkcpp.hpp"

namespace frameworkcpp {
namespace {

// Including the umbrella header must expose every module and its version entry
// point; this TU also gives clang-tidy a translation unit that reaches all
// public headers, including those no other test includes directly.
TEST(Umbrella, PullsInEveryModule) {
  memory::memory_t _memory;
  auto _shard = _memory.create_shard(1, {{0, sizeof(std::uint64_t), alignof(std::uint64_t), 8}});
  ASSERT_TRUE(_shard);

  environment::environment_config_t _config;
  _config.read_process_env_ = false;
  memory::memory_t _env_memory{environment::get_environment_memory_config(_config)};
  environment::init(_env_memory, _config);
  EXPECT_TRUE(environment::set_env("FC_UMBRELLA", "ok"));
  EXPECT_EQ(environment::get_env("FC_UMBRELLA", ""), "ok");
  environment::shutdown();

  EXPECT_STREQ(memory::get_version_string(), environment::get_version_string());
}

} // namespace
} // namespace frameworkcpp
