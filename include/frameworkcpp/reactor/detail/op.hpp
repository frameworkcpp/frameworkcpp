// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

#include "frameworkcpp/reactor/detail/types.hpp"

namespace frameworkcpp::reactor::detail {

// Una operación pre-reservada en arena. Se rellena al postear y se consume
// al completarse. No hay heap: todo vive en el slice del reactor.
struct op_t {
  op_kind_t kind_ = op_kind_t::custom_;
  int fd_ = -1;
  void *buffer_ = nullptr;
  std::size_t len_ = 0;
  reactor_fn_t fn_ = nullptr;
  void *ctx_ = nullptr;
  std::uint64_t strand_id_ = 0;
  std::uint64_t socket_id_ = 0; // socket dueño de la op (0 = ninguna)
  int priority_ = 0;
  op_result_t result_{};
  bool done_ = false;
  op_t *next_ = nullptr; // para cadenas intrusivas de strand
}; // struct op_t

} // namespace frameworkcpp::reactor::detail
