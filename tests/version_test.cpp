// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/environment/version.hpp"
#include "frameworkcpp/memory/version.hpp"

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

TEST(VersionTest, MemoryMatchesEnvironment) {
  EXPECT_EQ(frameworkcpp::memory::get_version_major(), get_version_major());
  EXPECT_EQ(frameworkcpp::memory::get_version_minor(), get_version_minor());
  EXPECT_EQ(frameworkcpp::memory::get_version_patch(), get_version_patch());
  EXPECT_STREQ(frameworkcpp::memory::get_version_string(), get_version_string());
}

} // namespace
} // namespace frameworkcpp::environment
