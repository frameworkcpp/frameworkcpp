// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/arena/version.hpp"
#include "frameworkcpp/environment/version.hpp"

namespace frameworkcpp::environment {
namespace {

TEST(VersionTest, Major) {
  EXPECT_EQ(get_version_major(), 1);
}

TEST(VersionTest, Minor) {
  EXPECT_EQ(get_version_minor(), 0);
}

TEST(VersionTest, Patch) {
  EXPECT_EQ(get_version_patch(), 0);
}

TEST(VersionTest, String) {
  EXPECT_STREQ(get_version_string(), "1.0.0");
}

TEST(VersionTest, ArenaMatchesEnvironment) {
  EXPECT_EQ(arena::get_version_major(), get_version_major());
  EXPECT_EQ(arena::get_version_minor(), get_version_minor());
  EXPECT_EQ(arena::get_version_patch(), get_version_patch());
  EXPECT_STREQ(arena::get_version_string(), get_version_string());
}

} // namespace
} // namespace frameworkcpp::environment
