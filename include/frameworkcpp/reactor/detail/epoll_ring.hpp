// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/epoll.h>
#include <unistd.h>

namespace frameworkcpp::reactor::detail {

// Wrapper sobre epoll(7).
//
// El backend de epoll es el portable: funciona en cualquier Linux, incluso en
// los que no exponen io_uring (emulacion con QEMU, kernels viejos). El
// precio es una syscall por evento en vez de un lote.
//
// El array de eventos se reserva en el memory, asi que la espera no reserva
// memoria: `wait` solo escribe sobre el buffer que ya existe.
class epoll_ring_t {
  int epoll_fd_ = -1;

  public:
  epoll_ring_t() = default;
  epoll_ring_t(const epoll_ring_t &) = delete;
  epoll_ring_t &operator=(const epoll_ring_t &) = delete;
  epoll_ring_t(epoll_ring_t &&) = delete;
  epoll_ring_t &operator=(epoll_ring_t &&) = delete;

  ~epoll_ring_t() {
    if (epoll_fd_ >= 0)
      ::close(epoll_fd_);
  }

  // Pre-reserva el array de eventos en memory. `capacity` es la cantidad de
  // eventos que una espera puede devolver a la vez.
  [[nodiscard]] bool init() noexcept {
    if (epoll_fd_ >= 0)
      return false;
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    return epoll_fd_ >= 0;
  }

  [[nodiscard]] bool is_ready() const noexcept { return epoll_fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return epoll_fd_; }

  // Registra un descriptor con los eventos indicados. `token` vuelve intacto
  // en el evento, y es como el reactor sabe que socket se disparo.
  [[nodiscard]] bool add(int fd, std::uint32_t events, std::uint64_t token) const noexcept {
    struct epoll_event _event {};
    _event.events = events;
    _event.data.u64 = token;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &_event) == 0;
  }

  // Cambia los eventos de un descriptor ya registrado.
  [[nodiscard]] bool modify(int fd, std::uint32_t events, std::uint64_t token) const noexcept {
    struct epoll_event _event {};
    _event.events = events;
    _event.data.u64 = token;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &_event) == 0;
  }

  [[nodiscard]] bool remove(int fd) const noexcept {
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
  }

  // Espera eventos sobre el buffer del worker. Cada worker tiene el suyo, asi
  // que varios pueden esperar sin pisarse. `timeout_ms` negativo espera sin
  // limite. Devuelve la cantidad de eventos, 0 si vencio, o -1 si fallo.
  [[nodiscard]] int
  wait(struct epoll_event *events, std::size_t capacity, int timeout_ms) const noexcept {
    return ::epoll_wait(epoll_fd_, events, static_cast<int>(capacity), timeout_ms);
  }
}; // class epoll_ring_t

} // namespace frameworkcpp::reactor::detail
