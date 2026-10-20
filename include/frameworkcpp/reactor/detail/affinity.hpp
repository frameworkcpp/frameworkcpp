// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <sched.h>

namespace frameworkcpp::reactor::detail {

// Pincha el hilo actual a una CPU concreta. Se llama una vez, al arrancar el
// hilo de una queue, para que el event loop no migre entre núcleos y sus
// caches se queden calientes.
//
// Devuelve false si el sistema no permite el pin (núcleosrestricted, cpusets
// estrechos, contenedores). No es un error: el reactor sigue funcionando.
inline bool pin_thread_to_cpu(std::size_t cpu_index) noexcept {
  cpu_set_t _set;
  CPU_ZERO(&_set);
  CPU_SET(static_cast<int>(cpu_index % CPU_SETSIZE), &_set);
  return sched_setaffinity(0, sizeof(_set), &_set) == 0;
}

// Cuántas CPUs ve el proceso.
[[nodiscard]] inline std::size_t available_cpus() noexcept {
  const long _count_k = ::sysconf(_SC_NPROCESSORS_ONLN);
  return (_count_k > 0) ? static_cast<std::size_t>(_count_k) : 1;
}

} // namespace frameworkcpp::reactor::detail
