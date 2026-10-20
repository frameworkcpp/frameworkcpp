// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

namespace frameworkcpp::reactor {

enum class op_kind_t : std::uint8_t {
  accept_ = 0,
  recv_ = 1,
  send_ = 2,
  connect_ = 3,
  timer_ = 4,
  custom_ = 5,
}; // enum class op_kind_t

struct op_result_t {
  int ec_ = 0;            // 0 = éxito, errno positivo si error
  std::size_t bytes_ = 0; // bytes transferidos
}; // struct op_result_t

// Firma de handler sin lambdas ni captura de heap.
// `ctx` apunta a estado pre-reservado en arena; el dueño gestiona su vida.
using reactor_fn_t = void (*)(void *ctx, const op_result_t &result);

} // namespace frameworkcpp::reactor
