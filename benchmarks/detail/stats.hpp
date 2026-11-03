// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Utilidades para que cada bench entregue una distribución, no un promedio.
//
// El framework existe para sistemas en tiempo real: el número que interesa es
// cuánto cuesta la llamada (min, p50, p90, p99, máx) en este commit y en el
// CPU concreto del runner, no una media que esconde la cola.
//
// El contador de ciclos es específico de la arquitectura: rdtsc en x86,
// cntvct_el0 en arm64. En cualquier otra, se cae a nanosegundos de reloj
// monotónico, que igualmente sirven para ordenar latencias.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

namespace frameworkcpp::bench {

// --- Contador de ciclos ---------------------------------------------------

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)

inline std::uint64_t cycle_count() noexcept { return __builtin_ia32_rdtsc(); }

#elif defined(__aarch64__) || defined(_M_ARM64)

inline std::uint64_t cycle_count() noexcept {
  std::uint64_t _t_k = 0;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(_t_k));
  return _t_k;
}

#else

inline std::uint64_t cycle_count() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

#endif

// Ticks por segundo reales. En x86 se calibra contra el reloj monótono; en
// arm64 sale de cntfrq_el0. El resultado se cachea: calibrar cuesta 50 ms una
// vez por proceso.
inline std::uint64_t ticks_per_second() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
  static const std::uint64_t per_second_k = []() -> std::uint64_t {
    using std::chrono::steady_clock;
    const auto _start_k = steady_clock::now();
    const std::uint64_t _cycles0_k = cycle_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto _end_k = steady_clock::now();
    const std::uint64_t _cycles1_k = cycle_count();
    const double _seconds_k = std::chrono::duration<double>(_end_k - _start_k).count();
    return static_cast<std::uint64_t>(static_cast<double>(_cycles1_k - _cycles0_k) / _seconds_k);
  }();
  return per_second_k;
#elif defined(__aarch64__) || defined(_M_ARM64)
  static const std::uint64_t per_second_k = []() -> std::uint64_t {
    std::uint64_t _freq_k = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(_freq_k));
    return _freq_k;
  }();
  return per_second_k;
#else
  return 1'000'000'000ULL; // ticks == nanosegundos
#endif
}

// Precio en ciclos del propio par de lecturas del contador. Se resta de cada
// medición; es una cota pesimista: solo cuenta lo que hay entre ambas
// instrucciones, no el costo de pedir el dato al procesador.
inline std::uint64_t harness_overhead_cycles() noexcept {
  static const std::uint64_t overhead_k = []() -> std::uint64_t {
    std::uint64_t _best_k = std::numeric_limits<std::uint64_t>::max();
    for (int _i = 0; _i < 1000; ++_i) {
      const std::uint64_t _a_k = cycle_count();
      const std::uint64_t _b_k = cycle_count();
      _best_k = std::min(_best_k, _b_k - _a_k);
    }
    return _best_k;
  }();
  return overhead_k;
}

// Escala ticks -> nanosegundos; se cachea para no dividir en cada op.
inline double ticks_to_ns_scale() noexcept {
  static const double scale_k = []() -> double {
    return 1'000'000'000.0 / static_cast<double>(ticks_per_second());
  }();
  return scale_k;
}

inline std::uint64_t cycles_to_ns(std::uint64_t cycles) noexcept {
  return static_cast<std::uint64_t>(static_cast<double>(cycles) * ticks_to_ns_scale());
}

// --- Histograma de latencias -------------------------------------------------

// Buckets de dos tramos: 4 ns de paso desde 0 hasta 2048 ns, y 64 ns de paso
// hasta 16512 ns. Lo que cae por encima va al bucket de overflow. Una op =
// un incremento; el merge entre threads suma los contadores, y el percentil
// se obtiene por acumulación.
//
// La estimación de percentil es el límite inferior de su bucket. min_ y
// max_ se llevan exactos.
class histogram_t {
  public:
  struct stats_t {
    std::uint64_t count_ = 0;
    double min_ns_ = 0.0;
    double p50_ns_ = 0.0;
    double p90_ns_ = 0.0;
    double p99_ns_ = 0.0;
    double p999_ns_ = 0.0;
    double max_ns_ = 0.0;
    double mean_ns_ = 0.0;
    double stddev_ns_ = 0.0;
  };

  void record(std::uint64_t cycles) noexcept {
    const std::uint64_t _overhead_k = harness_overhead_cycles();
    const std::uint64_t _cycles_k = (cycles > _overhead_k) ? cycles - _overhead_k : 0;
    const double _ns_k = static_cast<double>(_cycles_k) * ticks_to_ns_scale();
    const std::uint64_t _bucket_k = bucket_of(_ns_k);
    ++buckets_[_bucket_k];
    min_ = std::min(min_, _ns_k);
    max_ = std::max(max_, _ns_k);
    total_ns_ += _ns_k;
    total_sq_ns_ += _ns_k * _ns_k;
    ++count_;
  }

  // Suma otro histograma sobre este. Los threads miden en su propio
  // histograma; uno los combina al final con add().
  void add(const histogram_t &other) noexcept {
    for (std::size_t _i = 0; _i < bucket_count_k; ++_i)
      buckets_[_i] += other.buckets_[_i];
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    total_ns_ += other.total_ns_;
    total_sq_ns_ += other.total_sq_ns_;
    count_ += other.count_;
  }

  [[nodiscard]] stats_t snapshot() const noexcept {
    stats_t _stats_k;
    _stats_k.count_ = count_;
    if (count_ == 0)
      return _stats_k;
    _stats_k.min_ns_ = min_ns();
    _stats_k.max_ns_ = max_ns();
    _stats_k.p50_ns_ = percentile(0.5);
    _stats_k.p90_ns_ = percentile(0.9);
    _stats_k.p99_ns_ = percentile(0.99);
    _stats_k.p999_ns_ = percentile(0.999);
    _stats_k.mean_ns_ = total_ns_ / static_cast<double>(count_);
    // Var(X) = E[X^2] - E[X]^2 con X el ns por op. Los cuadrados acumulados
    // caben en long double (80 bits en x86, IEEE double en arm64).
    const auto _count_k = static_cast<long double>(count_);
    const long double _variance_k = std::max(
        0.0L, (static_cast<long double>(total_sq_ns_) / _count_k) -
                  (static_cast<long double>(_stats_k.mean_ns_) *
                   static_cast<long double>(_stats_k.mean_ns_)));
    _stats_k.stddev_ns_ = static_cast<double>(std::sqrt(_variance_k));
    return _stats_k;
  }

  private:
  static constexpr std::size_t bucket_small_k = 512;      // [0, 2048) ns, 4 ns steps
  static constexpr std::size_t bucket_large_k = 226;      // [2048, 16512) ns, 64 ns steps
  static constexpr std::size_t bucket_count_k = bucket_small_k + bucket_large_k + 1;
  static constexpr std::uint64_t bucket_width_small_ns_k = 4;
  static constexpr std::uint64_t bucket_width_large_ns_k = 64;
  static constexpr std::uint64_t large_start_ns_k = 2048;
  static constexpr std::uint64_t overflow_ns_k = 16512;
  static constexpr std::size_t overflow_bucket_k = bucket_small_k + bucket_large_k;

  static_assert(bucket_small_k * bucket_width_small_ns_k == large_start_ns_k);
  static_assert(bucket_small_k + bucket_large_k == overflow_bucket_k);
  static_assert(large_start_ns_k + bucket_large_k * bucket_width_large_ns_k == overflow_ns_k);

  [[nodiscard]] static std::uint64_t bucket_of(double ns) noexcept {
    if (ns < static_cast<double>(large_start_ns_k))
      return static_cast<std::uint64_t>(ns) / bucket_width_small_ns_k;
    if (ns < static_cast<double>(overflow_ns_k))
      return bucket_small_k +
             static_cast<std::uint64_t>((ns - static_cast<double>(large_start_ns_k)) /
                                        bucket_width_large_ns_k);
    return overflow_bucket_k;
  }

  [[nodiscard]] double percentile(double fraction) const noexcept {
    const double _estimate_ns_k = [&]() -> double {
      const std::uint64_t _target_k = std::max<std::uint64_t>(
          1, static_cast<std::uint64_t>(std::ceil(static_cast<double>(count_) * fraction)));
      std::uint64_t _cumulative_k = 0;
      for (std::size_t _index = 0; _index < bucket_count_k; ++_index) {
        _cumulative_k += buckets_[_index];
        if (_cumulative_k >= _target_k)
          return bucket_start(_index);
      }
      return static_cast<double>(overflow_ns_k);
    }();
    return _estimate_ns_k;
  }

  [[nodiscard]] static double bucket_start(std::size_t index) noexcept {
    if (index < bucket_small_k)
      return static_cast<double>(index * bucket_width_small_ns_k);
    if (index < overflow_bucket_k)
      return static_cast<double>(large_start_ns_k +
                                 ((index - bucket_small_k) * bucket_width_large_ns_k));
    return static_cast<double>(overflow_ns_k);
  }

  [[nodiscard]] double min_ns() const noexcept { return min_; }
  [[nodiscard]] double max_ns() const noexcept { return max_; }

  // Público en construcción/copia implícita para add().
  std::uint64_t buckets_[bucket_count_k] = {};
  std::uint64_t count_ = 0;
  double min_ = std::numeric_limits<double>::max();
  double max_ = 0.0;
  double total_ns_ = 0.0;
  double total_sq_ns_ = 0.0;
}; // class histogram_t

// Publica el histograma como counters de Google Benchmark. Cada counter se
// llama <p>Ns; con Threads el mismo counter sale por fila de thread, que es
// lo que permite ver el scaling. count es el número de muestras totales.
inline void report_histogram(benchmark::State &state,
                             const histogram_t &hist,
                             std::uint64_t items) noexcept {
  const histogram_t::stats_t _stats_k = hist.snapshot();
  state.SetItemsProcessed(static_cast<std::int64_t>(items));
  state.counters["count"] = static_cast<double>(_stats_k.count_);
  state.counters["min_ns"] = _stats_k.min_ns_;
  state.counters["p50_ns"] = _stats_k.p50_ns_;
  state.counters["p90_ns"] = _stats_k.p90_ns_;
  state.counters["p99_ns"] = _stats_k.p99_ns_;
  state.counters["p999_ns"] = _stats_k.p999_ns_;
  state.counters["max_ns"] = _stats_k.max_ns_;
  state.counters["mean_ns"] = _stats_k.mean_ns_;
  state.counters["stddev_ns"] = _stats_k.stddev_ns_;
}

} // namespace frameworkcpp::bench
