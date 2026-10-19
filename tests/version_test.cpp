// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/arena/version.hpp"
#include "frameworkcpp/environment/version.hpp"

namespace frameworkcpp::environment {
namespace {

TEST(VersionTest, Major) {
    EXPECT_EQ(version_major(), 1);
}

TEST(VersionTest, Minor) {
    EXPECT_EQ(version_minor(), 0);
}

TEST(VersionTest, Patch) {
    EXPECT_EQ(version_patch(), 0);
}

TEST(VersionTest, String) {
    EXPECT_STREQ(version_string(), "1.0.0");
}

TEST(VersionTest, ArenaMatchesEnvironment) {
    EXPECT_EQ(arena::version_major(), version_major());
    EXPECT_EQ(arena::version_minor(), version_minor());
    EXPECT_EQ(arena::version_patch(), version_patch());
    EXPECT_STREQ(arena::version_string(), version_string());
}

} // namespace
} // namespace frameworkcpp::environment