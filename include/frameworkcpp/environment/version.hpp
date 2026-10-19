// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace frameworkcpp::environment {

[[nodiscard]] constexpr int get_version_major() noexcept {
  return 1;
}
[[nodiscard]] constexpr int get_version_minor() noexcept {
  return 0;
}
[[nodiscard]] constexpr int get_version_patch() noexcept {
  return 0;
}

[[nodiscard]] inline const char *get_version_string() noexcept {
#ifdef FRAMEWORKCPP_VERSION_STRING
  return FRAMEWORKCPP_VERSION_STRING;
#else
  return "0.0.0";
#endif
}

} // namespace frameworkcpp::environment
