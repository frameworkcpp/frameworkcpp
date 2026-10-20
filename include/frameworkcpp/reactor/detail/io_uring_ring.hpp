// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <liburing.h>

namespace frameworkcpp::reactor::detail {

// Wrapper sobre liburing. El ring se crea una vez (setup) y luego el hot path
// solo usa SQEs y CQEs: sin syscalls por operación cuando el kernel tiene
// trabajo pendiente, y sin malloc.
//
// La cola de trabajo del reactor es independiente de este ring: este solo
// cubre las operaciones de E/S.
class io_uring_ring_t {
  struct io_uring ring_{};
  bool ready_ = false;
  std::uint32_t entries_ = 0;

  public:
  io_uring_ring_t() = default;
  io_uring_ring_t(const io_uring_ring_t &) = delete;
  io_uring_ring_t &operator=(const io_uring_ring_t &) = delete;
  io_uring_ring_t(io_uring_ring_t &&) = delete;
  io_uring_ring_t &operator=(io_uring_ring_t &&) = delete;

  ~io_uring_ring_t() {
    if (ready_)
      io_uring_queue_exit(&ring_);
  }

  [[nodiscard]] bool init(std::uint32_t entries) noexcept {
    if (ready_)
      return false;
    if (io_uring_queue_init(entries, &ring_, 0) < 0)
      return false;
    entries_ = entries;
    ready_ = true;
    return true;
  }

  [[nodiscard]] bool is_ready() const noexcept { return ready_; }
  [[nodiscard]] std::uint32_t entries() const noexcept { return entries_; }

  // Obtiene un SQE libre. Devuelve nullptr si el ring de envío está lleno.
  [[nodiscard]] struct io_uring_sqe *acquire_sqe() noexcept { return io_uring_get_sqe(&ring_); }

  // Publica los SQEs pendientes. Devuelve false si el envío falló.
  [[nodiscard]] bool submit(std::uint32_t count = 1) noexcept {
    (void) count;
    const int _ret_k = io_uring_submit(&ring_);
    return _ret_k >= 0;
  }

  // Espera un CQE con timeout. Devuelve false si venció el timeout.
  [[nodiscard]] bool wait_cqe(struct io_uring_cqe **out_cqe, long timeout_ns = -1) noexcept {
    if (timeout_ns < 0) {
      return io_uring_wait_cqe(&ring_, out_cqe) == 0;
    }
    struct __kernel_timespec _ts{};
    _ts.tv_sec = static_cast<__kernel_time_t>(timeout_ns / 1000000000L);
    _ts.tv_nsec = static_cast<long long>(timeout_ns % 1000000000L);
    return io_uring_wait_cqe_timeout(&ring_, out_cqe, &_ts) == 0;
  }

  // Marca el CQE como consumido.
  void cqe_seen(struct io_uring_cqe *cqe) noexcept { io_uring_cqe_seen(&ring_, cqe); }
}; // class io_uring_ring_t

} // namespace frameworkcpp::reactor::detail
