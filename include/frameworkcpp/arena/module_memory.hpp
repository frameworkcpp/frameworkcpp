// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

namespace frameworkcpp::arena {

// Contrato estándar de reserva de memoria en tiempo de compilación.
//
// Todo módulo que necesite carvear memoria de un arena_t debe exponer una
// función estática constexpr:
//
//     static constexpr std::size_t precompute(const Config &config) noexcept;
//
// que devuelva los bytes exactos que requerirá para el dado config. Así el
// usuario puede sumar los footprints de todos los módulos antes de construir
// el arena_t y saber, en compile time, cuánta memoria reservar.
//
// Un módulo que cumple el contrato expone además este adaptador:
//
//     struct environment_module_t {
//       static constexpr std::size_t precompute(const environment_config_t &c) noexcept;
//     };
//
// Ejemplo:
//     constexpr std::size_t env_k = environment::precompute(env_cfg);
//     constexpr std::size_t rea_k = reactor::precompute(rea_cfg);
//     constexpr std::size_t total_k = env_k + rea_k;
//     arena_t arena{arena_config_t{.size_ = total_k}};
//     environment::init(arena, env_cfg);
//     reactor.bind_arena(arena, rea_cfg);
struct module_memory_t {}; // struct module_memory_t

// Descripción de una reserva individual de módulo. Solo para diagnóstico,
// introspección o validación de presupuestos.
struct module_reservation_t {
  const char *name_;  // nombre del módulo (solo diagnóstico)
  std::size_t bytes_; // bytes requeridos por el módulo
}; // struct module_reservation_t

// Construye la reserva de un módulo a partir de su config.
template <typename Module, typename Config>
[[nodiscard]] constexpr module_reservation_t make_reservation(const char *name,
                                                              const Config &config) noexcept {
  return module_reservation_t{name, Module::precompute(config)};
}

// Suma las reservas de varios módulos. Usable en constexpr si todas las
// precompute lo son.
template <typename... Reservations>
[[nodiscard]] constexpr std::size_t merge_reservations(Reservations... reservations) noexcept {
  return (std::size_t{0} + ... + reservations.bytes_);
}

} // namespace frameworkcpp::arena
