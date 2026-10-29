// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace frameworkcpp::memory::detail {
class region_t {
  std::byte *base_ = nullptr;
  std::size_t bytes_ = 0;
  std::size_t free_top_ = 0;

  public:
  region_t() = default;
  region_t(const region_t &) = delete;
  region_t &operator=(const region_t &) = delete;

  region_t(region_t &&other) noexcept
      : base_(other.base_), bytes_(other.bytes_), free_top_(other.free_top_) {
    other.base_ = nullptr;
    other.bytes_ = 0;
    other.free_top_ = 0;
  }

  region_t &operator=(region_t &&other) noexcept {
    if (this != &other) {
      release();
      base_ = other.base_;
      bytes_ = other.bytes_;
      free_top_ = other.free_top_;
      other.base_ = nullptr;
      other.bytes_ = 0;
      other.free_top_ = 0;
    }
    return *this;
  }

  ~region_t() { release(); }

  bool reserve(std::size_t bytes, bool touch_pages) {
    if (base_ != nullptr)
      return false;
    const int _flags_k = MAP_PRIVATE | MAP_ANONYMOUS | (touch_pages ? MAP_POPULATE : 0);
    auto *_pointer =
        static_cast<std::byte *>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, _flags_k, -1, 0));
    if (_pointer == MAP_FAILED)
      return false;
    base_ = _pointer;
    bytes_ = bytes;
    free_top_ = 0;
    return true;
  }

  [[nodiscard]] std::byte *get_base() const { return base_; }
  [[nodiscard]] std::size_t get_bytes() const { return bytes_; }
  [[nodiscard]] std::size_t get_free_top() const { return free_top_; }

  [[nodiscard]] bool can_fit(std::size_t bytes) const { return free_top_ + bytes <= bytes_; }

  std::byte *take(std::size_t bytes) {
    if (!can_fit(bytes))
      return nullptr;
    auto *_pointer = base_ + free_top_;
    free_top_ += bytes;
    return _pointer;
  }

  private:
  void release() {
    if (base_ != nullptr) {
      ::munmap(base_, bytes_);
      base_ = nullptr;
      bytes_ = 0;
      free_top_ = 0;
    }
  }
}; // class region_t
} // namespace frameworkcpp::memory::detail
