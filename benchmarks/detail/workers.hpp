// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Pool de threads persistente para benchmarks multi-thread.
//
// Google Benchmark ejecuta threads bajo su control, pero entonces cada fila
// mide lo que ese thread corre en su propia ventana y no hay forma directa de
// obtener la distribución agregada de todos los productores ni su ops/s
// agregada. Aquí el pool vive dentro del bench: una iteración de Google
// Benchmark = una tanda paralela, y al final se mezclan los histogramas.

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

#include "stats.hpp"

namespace frameworkcpp::bench {

// Un worker por slot. execute() llama a fn(index, histogram) una vez por
// worker en cada tanda; el pool devuelve cuando todos los workers acabaron.
template <std::size_t N>
class workers_t {
  public:
  // fn se llama con (index, histogram_por_worker). Se ejecuta en paralelo una
  // vez por tanda; execute() espera a que todos los workers terminen.
  template <typename Fn> void execute(Fn&& fn) {
    work_ = std::forward<Fn>(fn);
    go_.arrive_and_wait();
    if (work_) {
      work_(0, hist_[0]);
      go_.arrive_and_wait();
    }
    work_ = nullptr;
  }

  [[nodiscard]] histogram_t merged() const noexcept {
    histogram_t _total_k;
    for (std::size_t _i = 0; _i < N; ++_i)
      _total_k.add(hist_[_i]);
    return _total_k;
  }

  void reset() noexcept {
    for (std::size_t _i = 0; _i < N; ++_i)
      hist_[_i] = histogram_t{};
  }

  ~workers_t() {
    stop_.store(true, std::memory_order_release);
    go_.arrive_and_wait();
    for (auto &_thread : threads_)
      if (_thread.joinable())
        _thread.join();
  }

  workers_t() {
    for (std::size_t _i = 1; _i < N; ++_i) {
      threads_.emplace_back([this, _i] {
        for (;;) {
          go_.arrive_and_wait();
          if (stop_.load(std::memory_order_acquire))
            return;
          if (work_)
            work_(_i, hist_[_i]);
          go_.arrive_and_wait();
        }
      });
    }
  }

  workers_t(const workers_t &) = delete;
  workers_t &operator=(const workers_t &) = delete;
  workers_t(workers_t &&) = delete;
  workers_t &operator=(workers_t &&) = delete;

  private:
  std::array<histogram_t, N> hist_{};
  std::vector<std::thread> threads_;
  std::barrier<> go_{N};
  std::atomic<bool> stop_{false};
  std::function<void(std::size_t, histogram_t &)> work_;
}; // class workers_t

} // namespace frameworkcpp::bench
