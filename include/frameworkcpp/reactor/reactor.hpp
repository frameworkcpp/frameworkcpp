// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>

#include "frameworkcpp/memory/memory.hpp"
#include "frameworkcpp/reactor/detail/state.hpp"
#include "frameworkcpp/reactor/detail/types.hpp"
#include "frameworkcpp/reactor/reactor_config.hpp"

namespace frameworkcpp::reactor {

// El contador de timers es un atomic de 64 bits que se incrementa desde
// cualquier hilo. En una plataforma de 32 bits sin acceso exclusivo de 64
// bits (armv6) el estandar cae a una lock: eso romperia la promesa de no
// tener locks en el hot path, asi que se comprueba al compilar.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the reactor needs lock-free 64-bit atomics");

// Contrato estándar de reserva de memoria en tiempo de compilación.
// Definido al final de este header: necesita los tipos completos.
constexpr std::size_t precompute(const reactor_config_t &config) noexcept;

// Devuelve true si el kernel y el entorno permiten usar io_uring. Some
// environments (emulation, restricted sandboxes) do not expose it, and the
// reactor cannot run there. Check it once at startup.
[[nodiscard]] bool io_uring_available() noexcept;

// Reloj monotónico en nanosegundos. Es la base de todos los deadlines del
// reactor: comparar contra él es la forma de saber si un timer ya venció.
[[nodiscard]] std::uint64_t monotonic_now_ns() noexcept;

class reactor_t;

// Cola nombrada de eventos. Cada queue tiene su io_uring y su hilo de eventos.
class queue_t {
  friend class reactor_t;
  friend class socket_t;
  friend struct detail::queue_control_t;

  std::string_view name_;
  std::uint64_t id_ = 0;
  detail::queue_control_t *control_ = nullptr;
  detail::op_t *ops_segment_ = nullptr;
  std::size_t ops_segment_size_ = 0;
  queue_concurrency_t concurrency_ = {};
  reactor_t *owner_ = nullptr;
  std::size_t index_ = 0;

  public:
  queue_t() = default;

  [[nodiscard]] explicit operator bool() const noexcept { return id_ != 0; }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Contrato con el que se creo esta queue. No cambia durante su vida.
  [[nodiscard]] queue_concurrency_t get_concurrency() const noexcept { return concurrency_; }

  // Identificador numérico de la queue. Estable durante toda la vida del
  // reactor; 0 si el handle no está ligado a ninguna queue.
  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

  // Postea una operación sin lambda. `fn` se invoca al completarse con `ctx`.
  bool post(op_kind_t kind,
            int fd,
            void *buffer,
            std::size_t len,
            reactor_fn_t fn,
            void *ctx,
            std::uint64_t strand_id = 0,
            int priority = 0,
            std::uint64_t socket_id = 0,
            const sockaddr *addr = nullptr,
            std::size_t addr_len = 0) noexcept;

  // Loop de eventos del hilo dueño de esta queue. Bloqueante.
  void run() noexcept;

  void stop() noexcept;

  [[nodiscard]] std::size_t pending_ops() const noexcept;
}; // class queue_t

// Socket registrado en el io_uring de una queue. Zero-copy: buffer en memory.
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

  // True cuando no hay ninguna operación en vuelo para este socket. Solo un
  // socket ocioso puede mudarse de queue.
  [[nodiscard]] bool is_idle() const noexcept;

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

  // Enlaza el socket a una dirección local (setup, no hot path).
  bool bind_to(const sockaddr *addr, std::size_t addr_len) const noexcept;

  // Pone el socket en modo escucha (setup, no hot path).
  [[nodiscard]] bool listen_to(int backlog) const noexcept;

  // Envía un datagrama a una dirección concreta (UDP).
  bool send_to(const void *buffer,
               std::size_t len,
               const sockaddr *addr,
               std::size_t addr_len,
               reactor_fn_t fn,
               void *ctx,
               std::uint64_t strand_id = 0) noexcept;

  // Recibe un datagrama y guarda la dirección del emisor en la socket.
  bool recv_from(void *buffer,
                 std::size_t len,
                 reactor_fn_t fn,
                 void *ctx,
                 std::uint64_t strand_id = 0) noexcept;

  // Dirección del último datagrama recibido.
  [[nodiscard]] const sockaddr *last_peer() const noexcept;

  bool close() noexcept;

  private:
  [[nodiscard]] detail::socket_slot_t *slot() const noexcept;
  bool post_op(op_kind_t kind,
               void *buffer,
               std::size_t len,
               reactor_fn_t fn,
               void *ctx,
               std::uint64_t strand_id) noexcept;
}; // class socket_t

// Serializa handlers por clave (fd / connection_id) sin mutex en fast path.
// El modelo de concurrencia declara como se comparte la clave; a diferencia de
// un bucket no cambia la implementacion del reactor, pero deja el contrato
// escrito para cuando una queue tenga varios consumidores.
class strand_t {
  friend class reactor_t;

  std::uint64_t id_ = 0;
  concurrency_model_t concurrency_ = concurrency_model_t::single_thread_;
  queue_t *queue_ = nullptr;

  public:
  strand_t() = default;

  [[nodiscard]] explicit operator bool() const noexcept { return id_ != 0; }

  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

  [[nodiscard]] concurrency_model_t get_concurrency_model() const noexcept { return concurrency_; }

  [[nodiscard]] queue_t *queue() const noexcept { return queue_; }
}; // struct strand_t

class reactor_t {
  friend class queue_t;

  public:
  reactor_t() = default;
  reactor_t(const reactor_t &) = delete;
  reactor_t &operator=(const reactor_t &) = delete;
  reactor_t(reactor_t &&) = delete;
  reactor_t &operator=(reactor_t &&) = delete;
  ~reactor_t();

  // Carve todas las estructuras del reactor desde `memory` una sola vez.
  // Todo el hot path usa memoria de memory; cero malloc/new/delete.
  bool bind_memory(memory::memory_t &memory_owner, const reactor_config_t &config) noexcept;

  [[nodiscard]] bool initialized() const noexcept;

  // Backend de eventos que el reactor está usando. Sirve para saber si la E/S
  // real está disponible: con epoll no hay io_uring detrás.
  [[nodiscard]] backend_t get_backend() const noexcept;

  // Contrato por defecto que heredan las queues de este reactor.
  [[nodiscard]] queue_concurrency_t get_concurrency() const noexcept { return concurrency_; }

  // Devuelve la misma queue_t para el mismo nombre (idempotente). Una queue
  // nueva hereda el contrato por defecto del reactor.
  [[nodiscard]] queue_t *get_or_create(std::string_view name) const noexcept;

  // Igual que el anterior, pero la queue nueva usa `concurrency`. Si el nombre
  // ya existe con otro contrato, devuelve nullptr: el modelo no cambia en
  // caliente.
  [[nodiscard]] queue_t *get_or_create(std::string_view name,
                                       queue_concurrency_t concurrency) const noexcept;

  // Crea una clave de serializacion propia de `queue` con un modelo explicito.
  // Devuelve un strand no valido si la queue no existe o se agotaron las
  // claves. El modelo debe declararse aunque hoy la queue tenga un solo
  // consumidor: es el contrato que el usuario promete.
  [[nodiscard]] strand_t create_strand(queue_t *queue,
                                       concurrency_model_t concurrency) const noexcept;

  // Igual que el anterior, heredando el modelo de la queue.
  [[nodiscard]] strand_t create_strand(queue_t *queue) const noexcept;

  // Registra un fd en una queue. Devuelve un socket no válido si no hay
  // slots libres o la queue es nula.
  socket_t create_socket(int fd, queue_t *queue) const noexcept;

  // Crea un socket del sistema y lo registra en una queue. `family_` es
  // AF_INET / AF_INET6 / AF_UNIX, `type_` es SOCK_STREAM / SOCK_DGRAM.
  //
  // Devuelve un socket no válido si la creación falla o no hay slots libres.
  socket_t open_socket(int family, int type, int protocol, queue_t *queue) const noexcept;

  // Servidor TCP: crea el socket, lo enlaza y lo pone en modo escucha.
  socket_t listen_tcp(queue_t *queue,
                      const sockaddr *addr,
                      std::size_t addr_len,
                      int backlog = 128) const noexcept;

  // Servidor Unix domain: igual que listen_tcp, sobre un path.
  socket_t listen_unix(queue_t *queue, const char *path, int backlog = 128) const noexcept;

  // Socket UDP enlazado a una dirección local.
  socket_t bind_udp(queue_t *queue, const sockaddr *addr, std::size_t addr_len) const noexcept;

  // Socket ICMP (raw). Necesita privilegios: devuelve un socket no válido si
  // el sistema no los concede.
  socket_t open_icmp(queue_t *queue) const noexcept;

  // Mueve un socket a otra queue sin cerrarlo: la conexión sigue viva y sus
  // próximas operaciones las atiende el hilo de la queue destino. Es lo que
  // permite balancear conexiones entre hilos en caliente.
  //
  // Devuelve false si el socket tiene operaciones en vuelo o si la queue
  // destino no existe.
  bool migrate(socket_t &socket, queue_t *target) noexcept;

  // Reparte los sockets ociosos entre las queues, de la más cargada a la más
  // descargada. Devuelve cuántos sockets se movieron.
  [[nodiscard]] std::size_t rebalance() const noexcept;

  // Conexiones registradas por queue. Sirve para observar el reparto.
  [[nodiscard]] std::size_t get_socket_count(queue_t *queue) const noexcept;

  // Programa un timer que se dispara dentro de `delay_ns` nanosegundos. El
  // handler corre en el hilo de la queue, como cualquier otra operación.
  //
  // Devuelve un id (> 0) para poder cancelarlo, o 0 si la queue no tiene
  // timers libres.
  std::uint64_t schedule_timer(queue_t *queue,
                               std::uint64_t delay_ns,
                               reactor_fn_t fn,
                               void *ctx,
                               std::uint64_t strand_id = 0) noexcept;

  // Cancela un timer antes de que se dispare. Devuelve true si estaba vivo.
  bool cancel_timer(queue_t *queue, std::uint64_t timer_id) const noexcept;

  // Timers vivos en una queue.
  [[nodiscard]] std::size_t get_timer_count(queue_t *queue) const noexcept;

  // Ejecuta los loops de todas las queues (bloqueante).
  void run_all() noexcept;

  void stop_all() noexcept;

  // Contrato estándar de precomputo compile-time (ver reactor_config.hpp).
  static constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
    return reactor::precompute(config);
  }

  private:
  static void execute_op(detail::queue_control_t *qc, detail::op_t *op) noexcept;
  static void complete_op(detail::queue_control_t *qc, std::size_t op_index, long result) noexcept;
  static void execute_io_uring_op(detail::queue_control_t *qc, detail::op_t *op) noexcept;
  static void arm_epoll_op(detail::queue_control_t *qc, detail::op_t *op) noexcept;
  static void run_epoll_op(detail::queue_control_t *qc, detail::op_t *op) noexcept;
  static void handle_epoll_event(detail::queue_control_t *qc,
                                 const struct epoll_event &event) noexcept;
  static void expire_timers(detail::queue_control_t *qc) noexcept;
  void run_queue(std::size_t index, std::size_t worker) const noexcept;
  std::uint64_t next_timer_id() noexcept;

  detail::reactor_state_t state_{};
  std::atomic<std::uint64_t> timer_id_{1};
  queue_concurrency_t concurrency_ = {};
  std::size_t workers_per_queue_ = 1;
  bool bound_ = false;
  bool affinity_ = false;
}; // class reactor_t

} // namespace frameworkcpp::reactor

namespace frameworkcpp::reactor {

// El contador de timers es un atomic de 64 bits que se incrementa desde
// cualquier hilo. En una plataforma de 32 bits sin acceso exclusivo de 64
// bits (armv6) el estandar cae a una lock: eso romperia la promesa de no
// tener locks en el hot path, asi que se comprueba al compilar.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the reactor needs lock-free 64-bit atomics");

// Descriptors de los buckets que el reactor carvea del memory. Es la única
// fuente de verdad: `precompute` y `bind_memory` usan la misma tabla, así que
// el tamaño calculado en compile time coincide exactamente con el reservado.
//
// Cada spec es un bloque contiguo (count = 1): la memory asigna un bloque del
// tamaño pedido, no N bloques. Los elementos se placement-new dentro.
constexpr void make_specs(const reactor_config_t &config,
                          memory::object_spec_t (&specs)[reactor_spec_count_k]) {
  const std::size_t _total_ops_k = config.queues_ * config.max_jobs_per_queue_;
  const std::size_t _total_strands_k = config.queues_ * config.max_strands_;
  const std::size_t _total_timers_k = config.queues_ * config.max_timers_per_queue_;
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
              sizeof(detail::strand_t) * _total_strands_k,
              alignof(detail::strand_t),
              1};
  specs[6] = {
      reactor_shard_tag_k + 6, sizeof(detail::op_t) * _total_timers_k, alignof(detail::op_t), 1};
  specs[7] = {reactor_shard_tag_k + 7,
              sizeof(detail::op_t *) * _total_timers_k,
              alignof(detail::op_t *),
              1};
  const std::size_t _total_events_k = _total_ops_k * config.workers_per_queue_;
  specs[8] = {reactor_shard_tag_k + 8,
              sizeof(struct epoll_event) * _total_events_k,
              alignof(struct epoll_event),
              1};
  specs[9] = {reactor_shard_tag_k + 9,
              sizeof(std::atomic<std::size_t>) * _total_ops_k,
              alignof(std::atomic<std::size_t>),
              1};
}

// Contrato estándar de reserva de memoria en tiempo de compilación.
// Devuelve los bytes EXACTOS que reactor necesita carvear del memory para el
// config dado. Usable en constexpr antes de construir el memory_t.
//
// Los rings de io_uring no se cuentan: el kernel los mapea por su cuenta, no
// salen de la memory del usuario.
constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
  memory::object_spec_t _specs[reactor_spec_count_k] = {};
  make_specs(config, _specs);
  return memory::shard_footprint(_specs);
}

// El presupuesto total del memory que el reactor necesita: su footprint
// redondeado a página, más `extra_bytes` para lo que quieras carvear en la
// misma memory.
constexpr std::size_t get_reactor_memory_bytes(const reactor_config_t &config,
                                               std::size_t extra_bytes = 0) {
  return memory::round_to_page(precompute(config) + extra_bytes);
}

constexpr memory::memory_config_t get_reactor_memory_config(const reactor_config_t &config,
                                                            std::size_t extra_bytes = 0) {
  memory::memory_config_t _memory_config{};
  _memory_config.size_ = get_reactor_memory_bytes(config, extra_bytes);
  return _memory_config;
}

// Adaptador del módulo al contrato estándar: permite usar
// memory::make_reservation<reactor_module_t>(...) y memory::merge_reservations.
struct reactor_module_t {
  static constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
    return reactor::precompute(config);
  }
}; // struct reactor_module_t

} // namespace frameworkcpp::reactor

#include "frameworkcpp/reactor/detail/reactor_impl.hpp"