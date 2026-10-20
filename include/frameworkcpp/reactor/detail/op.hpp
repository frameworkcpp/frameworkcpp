// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

#include "frameworkcpp/reactor/detail/types.hpp"

namespace frameworkcpp::reactor::detail {

// Una operación pre-reservada en arena. Se rellena al postear y se consume
// al completarse. No hay heap: todo vive en el slice del reactor.
//
// El op lleva su propio almacenamiento de direcciones porque el kernel
// escribe ahí la dirección del peer (accept, recvfrom) y la dirección de
// destino (sendto), y esos datos tienen que sobrevivir hasta que la
// operación se complete.
struct op_t {
  op_kind_t kind_ = op_kind_t::custom_;
  int fd_ = -1;
  void *buffer_ = nullptr;
  std::size_t len_ = 0;
  reactor_fn_t fn_ = nullptr;
  void *ctx_ = nullptr;
  std::uint64_t strand_id_ = 0;
  std::uint64_t socket_id_ = 0;   // socket dueño de la op (0 = ninguna)
  std::uint64_t timer_id_ = 0;    // id del timer (0 = no es un timer)
  std::uint64_t deadline_ns_ = 0; // deadline absoluto del timer
  std::size_t heap_index_ = 0;    // posición en el heap de timers
  int priority_ = 0;
  op_result_t result_{};
  bool done_ = false;
  op_t *next_ = nullptr; // para cadenas intrusivas de strand

  // Almacenamiento de direcciones de la operación.
  sockaddr_storage peer_{}; // peer de accept/recvfrom, destino de sendto
  socklen_t peer_len_ = 0;
  struct msghdr message_ {};
  struct iovec iov_ {};
}; // struct op_t

} // namespace frameworkcpp::reactor::detail
