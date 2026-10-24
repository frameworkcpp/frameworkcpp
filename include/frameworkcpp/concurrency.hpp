// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace frameworkcpp {

// Concurrency contract shared by every module that hands out individually
// synchronized resources. A resource is created with one model and keeps it
// for its whole life; asking for a different one later is a contract
// violation.
//
//   single_thread_  one thread owns the resource end to end.
//   lock_free_      several threads share it through atomics only.
//   thread_safe_    several threads share it; a mutex does the serialization.
enum class concurrency_model_t : std::uint8_t {
  single_thread_ = 0,
  lock_free_ = 1,
  thread_safe_ = 2,
}; // enum class concurrency_model_t

} // namespace frameworkcpp
