// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/reactor/detail/state.hpp"
#include "frameworkcpp/reactor/detail/types.hpp"
#include "frameworkcpp/reactor/reactor_config.hpp"

namespace frameworkcpp::reactor {

// Contrato estándar de reserva de memoria en tiempo de compilación.
// Definido al final de este header: necesita los tipos completos.
constexpr std::size_t precompute(const reactor_config_t &config) noexcept;

// Devuelve true si el kernel y el entorno permiten usar io_uring. Some
// environments (emulation, restricted sandboxes) do not expose it, and the
// reactor cannot run there. Check it once at startup.
[[nodiscard]] bool io_uring_available() noexcept;

// Cola nombrada de eventos. Cada queue tiene su io_uring y su hilo de eventos.
class queue_t {
  friend class reactor_t;
  friend struct detail::queue_control_t;

  std::string_view name_;
  std::uint64_t id_ = 0;
  detail::queue_control_t *control_ = nullptr;
  detail::op_t *ops_segment_ = nullptr;
  std::size_t ops_segment_size_ = 0;

  public:
  queue_t() = default;

  [[nodiscard]] explicit operator bool() const noexcept { return id_ != 0; }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Postea una operación sin lambda. `fn` se invoca al completarse con `ctx`.
  bool post(op_kind_t kind,
            int fd,
            void *buffer,
            std::size_t len,
            reactor_fn_t fn,
            void *ctx,
            std::uint64_t strand_id = 0,
            int priority = 0) noexcept;

  // Loop de eventos del hilo dueño de esta queue. Bloqueante.
  void run() noexcept;

  void stop() noexcept;

  [[nodiscard]] std::size_t pending_ops() const noexcept;
}; // class queue_t

// Socket registrado en el io_uring de una queue. Zero-copy: buffer en arena.
class socket_t {
  friend class reactor_t;

  int fd_ = -1;
  std::uint64_t id_ = 0;
  queue_t *queue_ = nullptr;

  public:
  socket_t() = default;

  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

  [[nodiscard]] int fd() const noexcept { return fd_; }

  [[nodiscard]] queue_t *queue() const noexcept { return queue_; }

  bool recv(void *buffer,
            std::size_t len,
            reactor_fn_t fn,
            void *ctx,
            std::uint64_t strand_id = 0) noexcept;

  bool send(const void *buffer,
            std::size_t len,
            reactor_fn_t fn,
            void *ctx,
            std::uint64_t strand_id = 0) noexcept;

  bool accept(reactor_fn_t fn, void *ctx, std::uint64_t strand_id = 0) noexcept;

  bool connect(const sockaddr *addr,
               std::size_t addr_len,
               reactor_fn_t fn,
               void *ctx,
               std::uint64_t strand_id = 0) noexcept;

  void close() noexcept;
}; // class socket_t

// Serializa handlers por clave (fd / connection_id) sin mutex en fast path.
class strand_t {
  friend class reactor_t;

  std::uint64_t id_ = 0;

  public:
  strand_t() = default;

  [[nodiscard]] explicit operator bool() const noexcept { return id_ != 0; }

  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
}; // struct strand_t

class reactor_t {
  public:
  reactor_t() = default;
  reactor_t(const reactor_t &) = delete;
  reactor_t &operator=(const reactor_t &) = delete;
  reactor_t(reactor_t &&) = delete;
  reactor_t &operator=(reactor_t &&) = delete;
  ~reactor_t();

  // Carve todas las estructuras del reactor desde `arena` una sola vez.
  // Todo el hot path usa memoria de arena; cero malloc/new/delete.
  bool bind_arena(arena::arena_t &arena, const reactor_config_t &config) noexcept;

  [[nodiscard]] bool initialized() const noexcept;

  // Devuelve la misma queue_t para el mismo nombre (idempotente).
  [[nodiscard]] queue_t *get_or_create(std::string_view name) const noexcept;

  // Registra un fd en una queue. Devuelve un socket no válido si no hay
  // slots libres o la queue es nula.
  socket_t create_socket(int fd, queue_t *queue) const noexcept;

  // Ejecuta los loops de todas las queues (bloqueante).
  void run_all() noexcept;

  void stop_all() noexcept;

  // Contrato estándar de precomputo compile-time (ver reactor_config.hpp).
  static constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
    return reactor::precompute(config);
  }

  private:
  static void execute_op(detail::queue_control_t *qc, detail::op_t *op) noexcept;
  static void complete_op(detail::queue_control_t *qc, struct io_uring_cqe *cqe) noexcept;
  void run_queue(std::size_t index) const noexcept;

  detail::reactor_state_t state_{};
  bool bound_ = false;
}; // class reactor_t

} // namespace frameworkcpp::reactor

namespace frameworkcpp::reactor {

// Descriptors de los buckets que el reactor carvea del arena. Es la única
// fuente de verdad: `precompute` y `bind_arena` usan la misma tabla, así que
// el tamaño calculado en compile time coincide exactamente con el reservado.
//
// Cada spec es un bloque contiguo (count = 1): la arena asigna un bloque del
// tamaño pedido, no N bloques. Los elementos se placement-new dentro.
constexpr void make_specs(const reactor_config_t &config,
                          arena::object_spec_t (&specs)[reactor_spec_count_k]) {
  const std::size_t _total_ops_k = config.queues_ * config.max_jobs_per_queue_;
  specs[0] = {reactor_shard_tag_k,
              sizeof(detail::queue_control_t) * config.queues_,
              alignof(detail::queue_control_t),
              1};
  specs[1] = {reactor_shard_tag_k + 1, sizeof(queue_t) * config.queues_, alignof(queue_t), 1};
  specs[2] = {
      reactor_shard_tag_k + 2, sizeof(detail::op_t) * _total_ops_k, alignof(detail::op_t), 1};
  specs[3] = {
      reactor_shard_tag_k + 3, sizeof(detail::op_t *) * _total_ops_k, alignof(detail::op_t *), 1};
  specs[4] = {reactor_shard_tag_k + 4,
              sizeof(detail::socket_slot_t) * config.max_sockets_,
              alignof(detail::socket_slot_t),
              1};
  specs[5] = {reactor_shard_tag_k + 5,
              sizeof(detail::strand_t) * config.max_strands_,
              alignof(detail::strand_t),
              1};
}

// Contrato estándar de reserva de memoria en tiempo de compilación.
// Devuelve los bytes EXACTOS que reactor necesita carvear del arena para el
// config dado. Usable en constexpr antes de construir el arena_t.
//
// Los rings de io_uring no se cuentan: el kernel los mapea por su cuenta, no
// salen de la arena del usuario.
constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
  arena::object_spec_t _specs[reactor_spec_count_k] = {};
  make_specs(config, _specs);
  return arena::shard_footprint(_specs);
}

// El presupuesto total del arena que el reactor necesita: su footprint
// redondeado a página, más `extra_bytes` para lo que quieras carvear en la
// misma arena.
constexpr std::size_t get_reactor_arena_bytes(const reactor_config_t &config,
                                              std::size_t extra_bytes = 0) {
  return arena::round_to_page(precompute(config) + extra_bytes);
}

constexpr arena::arena_config_t get_reactor_arena_config(const reactor_config_t &config,
                                                         std::size_t extra_bytes = 0) {
  arena::arena_config_t _arena_config{};
  _arena_config.size_ = get_reactor_arena_bytes(config, extra_bytes);
  return _arena_config;
}

// Adaptador del módulo al contrato estándar: permite usar
// arena::make_reservation<reactor_module_t>(...) y arena::merge_reservations.
struct reactor_module_t {
  static constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
    return reactor::precompute(config);
  }
}; // struct reactor_module_t

} // namespace frameworkcpp::reactor

#include "frameworkcpp/reactor/detail/reactor_impl.hpp"