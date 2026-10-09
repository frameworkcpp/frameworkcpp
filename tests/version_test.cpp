// SPDX-License-Identifier: AGPL-3.0-only

#include <gtest/gtest.h>

#include "frameworkcpp/env/version.hpp"

namespace frameworkcpp::env {
namespace {

TEST(VersionTest, Major) { EXPECT_EQ(version_major(), 1); }

TEST(VersionTest, Minor) { EXPECT_EQ(version_minor(), 0); }

TEST(VersionTest, Patch) { EXPECT_EQ(version_patch(), 0); }

TEST(VersionTest, String) { EXPECT_STREQ(version_string(), "1.0.0"); }

}  // namespace
}  // namespace frameworkcpp::env