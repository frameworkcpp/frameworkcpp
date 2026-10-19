// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// A dotenv-style environment store backed entirely by an arena shard. Values
// live in immutable arena cells; readers keep a per-thread view and catch up
// through a bounded delta ring, so a warmed-up get_env never touches a shared
// atomic beyond the single global version counter.
//
// Updates are unbounded while memory stays fixed: a superseded cell is retired
// and later recycled once every live view has moved past the update that
// retired it (epoch-based reclamation). A get_env result is valid until the
// next get_env/set_env on the same thread, and the arena must outlive the
// module.
//
// include: #include <frameworkcpp/environment/environment.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/environment/config.hpp"
#include "frameworkcpp/environment/detail/digest.hpp"

namespace frameworkcpp::environment {
namespace detail {

struct control_t {
  arena::arena_shard_t shard_;
  std::atomic<key_cell_t *> *key_table_; // [slots]
  std::atomic<value_cell_t *> *global_;  // [slots]
  patch_t *journal_;                     // [journal_size]
  retired_t *retired_;                   // [retire_capacity], ring
  view_t **views_;                       // [max_threads], pre-allocated
  std::atomic<std::uint64_t> version_;
  std::atomic<std::size_t> retire_head_;
  std::atomic<std::size_t> retire_tail_;
  std::size_t slots_;
  std::size_t slot_mask_;
  std::size_t journal_size_;
  std::size_t journal_mask_;
  std::size_t retire_capacity_;
  std::size_t retire_mask_;
  std::size_t max_keys_;
  std::size_t max_key_len_;
  std::size_t max_value_len_;
  std::size_t max_threads_;
  std::mutex writer_;
}; // struct control_t

inline control_t *&control_ref() noexcept {
  static control_t *_control = nullptr;
  return _control;
}

// Bumped on every init/shutdown so stale per-thread views never touch a dead
// arena when their thread exits.
inline std::atomic<std::uint64_t> &epoch_ref() noexcept {
  static std::atomic<std::uint64_t> _epoch{0};
  return _epoch;
}

constexpr void make_specs(const environment_config_t &config,
                          arena::object_spec_t (&specs)[spec_count_k],
                          std::size_t &slots,
                          std::size_t &retire_capacity) {
  slots = next_pow2(config.max_keys_ * 2);
  retire_capacity = next_pow2(config.max_keys_ + config.value_cells_);
  specs[0] = {control_tag_k, sizeof(control_t), alignof(control_t), 1};
  specs[1] = {
      key_tag_k, key_object_bytes(config.max_key_len_), alignof(std::uint64_t), config.max_keys_};
  specs[2] = {value_tag_k,
              value_object_bytes(config.max_value_len_),
              alignof(std::uint64_t),
              config.max_keys_ + config.value_cells_};
  specs[3] = {view_tag_k, view_object_bytes(slots), alignof(std::uint64_t), config.max_threads_};
  specs[4] = {key_table_tag_k,
              slots * sizeof(std::atomic<key_cell_t *>),
              alignof(std::atomic<key_cell_t *>),
              1};
  specs[5] = {global_tag_k,
              slots * sizeof(std::atomic<value_cell_t *>),
              alignof(std::atomic<value_cell_t *>),
              1};
  specs[6] = {journal_tag_k, config.journal_size_ * sizeof(patch_t), alignof(patch_t), 1};
  specs[7] = {retire_tag_k, retire_capacity * sizeof(retired_t), alignof(retired_t), 1};
  specs[8] = {view_reg_tag_k, config.max_threads_ * sizeof(view_t *), alignof(view_t *), 1};
}

inline std::size_t lookup(control_t *control, const char *name, std::size_t len) {
  const auto _hash_k = static_cast<std::uint32_t>(fnv1a(name, len));
  std::size_t _index = _hash_k & control->slot_mask_;
  for (std::size_t _probe = 0; _probe < control->slots_; ++_probe) {
    const key_cell_t *_cell = control->key_table_[_index].load(std::memory_order_acquire);
    if (_cell == nullptr)
      return no_key_k;
    if (_cell->hash_ == _hash_k && _cell->len_ == len &&
        std::memcmp(key_bytes(_cell), name, len) == 0)
      return _index;
    _index = (_index + 1) & control->slot_mask_;
  }
  return no_key_k;
}

inline std::size_t find_or_insert(control_t *control, const char *name, std::size_t len) {
  const auto _hash_k = static_cast<std::uint32_t>(fnv1a(name, len));
  std::size_t _index = _hash_k & control->slot_mask_;
  for (std::size_t _probe = 0; _probe < control->slots_; ++_probe) {
    key_cell_t *_cell = control->key_table_[_index].load(std::memory_order_relaxed);
    if (_cell == nullptr) {
      auto *_raw = control->shard_.allocate_tagged(
          key_tag_k, key_object_bytes(control->max_key_len_), alignof(std::uint64_t));
      if (_raw == nullptr)
        return no_key_k;
      auto *_fresh = reinterpret_cast<key_cell_t *>(_raw);
      _fresh->hash_ = _hash_k;
      _fresh->len_ = static_cast<std::uint32_t>(len);
      std::memcpy(key_bytes(_fresh), name, len);
      control->key_table_[_index].store(_fresh, std::memory_order_release);
      return _index;
    }
    if (_cell->hash_ == _hash_k && _cell->len_ == len &&
        std::memcmp(key_bytes(_cell), name, len) == 0)
      return _index;
    _index = (_index + 1) & control->slot_mask_;
  }
  return no_key_k;
}

inline bool store_initial(control_t *control,
                          const char *name,
                          std::size_t name_len,
                          const char *value,
                          std::size_t value_len) {
  if (name_len == 0 || name_len > control->max_key_len_ || value_len > control->max_value_len_)
    return false;
  const std::size_t _id_k = lookup(control, name, name_len);
  if (_id_k != no_key_k)
    return false; // process environment wins over the file
  const std::size_t _slot_k = find_or_insert(control, name, name_len);
  if (_slot_k == no_key_k)
    return false;
  auto *_raw = control->shard_.allocate_tagged(
      value_tag_k, value_object_bytes(control->max_value_len_), alignof(std::uint64_t));
  if (_raw == nullptr)
    return false;
  auto *_cell = reinterpret_cast<value_cell_t *>(_raw);
  _cell->len_ = static_cast<std::uint32_t>(value_len);
  _cell->pad_ = 0;
  std::memcpy(value_bytes(_cell), value, value_len);
  control->global_[_slot_k].store(_cell, std::memory_order_release);
  return true;
}

inline void trim(const char *&begin, const char *&end) {
  while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\r'))
    ++begin;
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
    --end;
}

inline void load_line(control_t *control, const char *begin, const char *end) {
  trim(begin, end);
  if (begin == end || *begin == '#')
    return;
  if (end - begin > 7 && std::memcmp(begin, "export ", 7) == 0)
    begin += 7;
  const char *_equals = begin;
  while (_equals < end && *_equals != '=')
    ++_equals;
  if (_equals == end)
    return;
  const char *_name_begin = begin;
  const char *_name_end = _equals;
  trim(_name_begin, _name_end);
  const char *_value_begin = _equals + 1;
  const char *_value_end = end;
  trim(_value_begin, _value_end);
  if (_value_end - _value_begin >= 2 && ((*_value_begin == '"' && _value_end[-1] == '"') ||
                                         (*_value_begin == '\'' && _value_end[-1] == '\''))) {
    ++_value_begin;
    --_value_end;
  }
  store_initial(control,
                _name_begin,
                static_cast<std::size_t>(_name_end - _name_begin),
                _value_begin,
                static_cast<std::size_t>(_value_end - _value_begin));
}

inline void load_buffer(control_t *control, const char *data, std::size_t size) {
  std::size_t _index = 0;
  while (_index < size) {
    const std::size_t _start_k = _index;
    while (_index < size && data[_index] != '\n')
      ++_index;
    load_line(control, data + _start_k, data + _index);
    if (_index < size)
      ++_index;
  }
}

inline void load_dotenv(control_t *control, const char *path) {
  if (path == nullptr)
    return;
  const int _file_k = ::open(path, O_RDONLY);
  if (_file_k < 0)
    return;
  struct stat _info{};
  if (::fstat(_file_k, &_info) != 0 || _info.st_size <= 0) {
    ::close(_file_k);
    return;
  }
  const auto _size_k = static_cast<std::size_t>(_info.st_size);
  void *_map = ::mmap(nullptr, _size_k, PROT_READ, MAP_PRIVATE, _file_k, 0);
  ::close(_file_k);
  if (_map == MAP_FAILED)
    return;
  load_buffer(control, static_cast<const char *>(_map), _size_k);
  ::munmap(_map, _size_k);
}

inline void load_process_env(control_t *control) {
  for (char **_entry = environ; _entry != nullptr && *_entry != nullptr; ++_entry) {
    const char *_name = *_entry;
    const char *_equals = std::strchr(_name, '=');
    if (_equals == nullptr)
      continue;
    store_initial(control,
                  _name,
                  static_cast<std::size_t>(_equals - _name),
                  _equals + 1,
                  std::strlen(_equals + 1));
  }
}

struct view_slot_guard_t {
  view_t *view_ = nullptr;
  std::uint64_t epoch_ = 0;
  std::uint64_t published_ = 0;

  view_slot_guard_t() = default;
  view_slot_guard_t(const view_slot_guard_t &) = delete;
  view_slot_guard_t &operator=(const view_slot_guard_t &) = delete;
  view_slot_guard_t(view_slot_guard_t &&) = delete;
  view_slot_guard_t &operator=(view_slot_guard_t &&) = delete;
  ~view_slot_guard_t() {
    if (view_ != nullptr && epoch_ == epoch_ref().load(std::memory_order_relaxed) &&
        control_ref() != nullptr)
      std::atomic_ref<std::uint32_t>(view_->active_).store(0, std::memory_order_release);
  }
}; // struct view_slot_guard_t

inline view_slot_guard_t &tls_guard() noexcept {
  static thread_local view_slot_guard_t _guard;
  return _guard;
}

// Refresh the version this thread advertises to the reclaimer, but only when it
// actually moved. On the steady-state read path nothing changed, so no shared
// atomic is touched.
inline void publish_view(view_t *view, view_slot_guard_t &guard) {
  if (guard.published_ == view->my_version_)
    return;
  std::atomic_ref<std::uint64_t>(view->published_)
      .store(view->my_version_, std::memory_order_release);
  guard.published_ = view->my_version_;
}

inline void clear_cache(control_t *control, view_t *view) {
  value_cell_t **_slots = cached_slots(view);
  for (std::size_t _index = 0; _index < control->slots_; ++_index)
    _slots[_index] = nullptr;
}

// One pre-allocated view per thread. A thread without one claims a free slot
// (active == 0) with a CAS; on exit the slot is marked inactive again, so slots
// are recycled instead of burned. Claiming happens once per thread, never on the
// read path.
inline view_t *get_thread_view(control_t *control) {
  view_slot_guard_t &_guard = tls_guard();
  const std::uint64_t _epoch_k = epoch_ref().load(std::memory_order_relaxed);
  if (_guard.view_ != nullptr && _guard.epoch_ == _epoch_k)
    return _guard.view_;
  _guard.view_ = nullptr;
  _guard.epoch_ = _epoch_k;
  _guard.published_ = 0;
  for (std::size_t _index = 0; _index < control->max_threads_; ++_index) {
    view_t *_view = control->views_[_index];
    std::uint32_t _expected = 0;
    if (!std::atomic_ref<std::uint32_t>(_view->active_)
             .compare_exchange_strong(
                 _expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed))
      continue;
    clear_cache(control, _view);
    _view->my_version_ = control->version_.load(std::memory_order_acquire);
    // A reclaimer that reads this slot before we publish sees the previous
    // owner's version, which is never newer than ours, so it stays
    // conservative. Publish, then hand the view to the thread.
    _guard.published_ = _view->my_version_;
    std::atomic_ref<std::uint64_t>(_view->published_)
        .store(_view->my_version_, std::memory_order_release);
    _guard.view_ = _view;
    return _guard.view_;
  }
  return nullptr;
}

inline void reseed_view(control_t *control, view_t *view, std::uint64_t target) {
  clear_cache(control, view);
  view->my_version_ = target;
}

inline void sync_view(control_t *control, view_t *view, std::uint64_t target) {
  const std::uint64_t _my_k = view->my_version_;
  if (target <= _my_k) {
    view->my_version_ = target;
    return;
  }
  if (target - _my_k >= control->journal_size_) {
    reseed_view(control, view, target);
    return;
  }
  for (std::uint64_t _version = _my_k; _version < target; ++_version) {
    const patch_t &_patch = control->journal_[_version & control->journal_mask_];
    const std::uint64_t _tag_first_k = _patch.tag_.load(std::memory_order_acquire);
    if (_tag_first_k != _version + 1) { // slot overwritten by a newer wrap; fall back
      reseed_view(control, view, target);
      return;
    }
    const std::uint32_t _key_id_k = _patch.key_id_.load(std::memory_order_relaxed);
    value_cell_t *_cell = _patch.value_.load(std::memory_order_relaxed);
    if (_patch.tag_.load(std::memory_order_acquire) != _tag_first_k) {
      reseed_view(control, view, target);
      return;
    }
    cached_slots(view)[_key_id_k] = _cell;
  }
  view->my_version_ = target;
}

// Recycle retired cells that no live view can still reference. Called by the
// single writer under its mutex. A cell retired at version r is safe to reuse
// once every active view has published a version greater than r.
inline void reclaim(control_t *control) {
  std::uint64_t _min_version = control->version_.load(std::memory_order_relaxed);
  for (std::size_t _index = 0; _index < control->max_threads_; ++_index) {
    view_t *_view = control->views_[_index];
    if (std::atomic_ref<std::uint32_t>(_view->active_).load(std::memory_order_acquire) == 0)
      continue;
    const std::uint64_t _published_k =
        std::atomic_ref<std::uint64_t>(_view->published_).load(std::memory_order_acquire);
    _min_version = std::min(_min_version, _published_k);
  }
  std::size_t _head = control->retire_head_.load(std::memory_order_relaxed);
  const std::size_t _tail_k = control->retire_tail_.load(std::memory_order_relaxed);
  while (_head != _tail_k) {
    const retired_t &_entry = control->retired_[_head];
    if (_entry.retired_version_ >= _min_version)
      break;
    control->shard_.deallocate_bytes(_entry.cell_);
    _head = (_head + 1) & control->retire_mask_;
  }
  control->retire_head_.store(_head, std::memory_order_relaxed);
}

} // namespace detail

// Carve the module's tables into `arena` and load the process environment
// first (it wins), then the dotenv file for any key still unset. Call once,
// from a single thread, before spawning readers or writers.
inline void
init(arena::arena_t &arena, const char *dotenv_path, const environment_config_t &config = {}) {
  if (detail::control_ref() != nullptr)
    throw std::logic_error("environment already initialized");
  if (config.max_keys_ == 0 || config.max_key_len_ == 0 || config.max_value_len_ == 0 ||
      config.value_cells_ == 0 || config.max_threads_ == 0 || config.journal_size_ == 0)
    throw std::invalid_argument("environment capacity must be non-zero");
  if ((config.journal_size_ & (config.journal_size_ - 1)) != 0)
    throw std::invalid_argument("environment journal_size must be a power of two");

  arena::object_spec_t _specs[detail::spec_count_k] = {};
  std::size_t _slots = 0;
  std::size_t _retire_capacity = 0;
  detail::make_specs(config, _specs, _slots, _retire_capacity);

  auto _shard = arena.create_shard(
      detail::shard_tag_k, std::span<const arena::object_spec_t>(_specs, detail::spec_count_k));
  if (!_shard)
    throw std::runtime_error("environment: arena has no room for the module");

  _shard.get_bucket(detail::key_tag_k, arena::concurrency_model_t::lock_free_);
  _shard.get_bucket(detail::value_tag_k, arena::concurrency_model_t::lock_free_);

  auto *_control_raw = _shard.allocate_tagged(
      detail::control_tag_k, sizeof(detail::control_t), alignof(detail::control_t));
  if (_control_raw == nullptr)
    throw std::runtime_error("environment: control block allocation failed");
  auto *_control = ::new (static_cast<void *>(_control_raw)) detail::control_t{};
  _control->shard_ = _shard;
  _control->slots_ = _slots;
  _control->slot_mask_ = _slots - 1;
  _control->journal_size_ = config.journal_size_;
  _control->journal_mask_ = config.journal_size_ - 1;
  _control->retire_capacity_ = _retire_capacity;
  _control->retire_mask_ = _retire_capacity - 1;
  _control->max_keys_ = config.max_keys_;
  _control->max_key_len_ = config.max_key_len_;
  _control->max_value_len_ = config.max_value_len_;
  _control->max_threads_ = config.max_threads_;

  auto *_key_raw = _shard.allocate_tagged(detail::key_table_tag_k,
                                          _slots * sizeof(std::atomic<detail::key_cell_t *>),
                                          alignof(std::atomic<detail::key_cell_t *>));
  auto *_global_raw = _shard.allocate_tagged(detail::global_tag_k,
                                             _slots * sizeof(std::atomic<detail::value_cell_t *>),
                                             alignof(std::atomic<detail::value_cell_t *>));
  auto *_journal_raw = _shard.allocate_tagged(detail::journal_tag_k,
                                              config.journal_size_ * sizeof(detail::patch_t),
                                              alignof(detail::patch_t));
  auto *_retire_raw = _shard.allocate_tagged(detail::retire_tag_k,
                                             _retire_capacity * sizeof(detail::retired_t),
                                             alignof(detail::retired_t));
  auto *_viewreg_raw = _shard.allocate_tagged(detail::view_reg_tag_k,
                                              config.max_threads_ * sizeof(detail::view_t *),
                                              alignof(detail::view_t *));
  if (_key_raw == nullptr || _global_raw == nullptr || _journal_raw == nullptr ||
      _retire_raw == nullptr || _viewreg_raw == nullptr)
    throw std::runtime_error("environment: table allocation failed");

  _control->key_table_ = reinterpret_cast<std::atomic<detail::key_cell_t *> *>(_key_raw);
  _control->global_ = reinterpret_cast<std::atomic<detail::value_cell_t *> *>(_global_raw);
  _control->journal_ = reinterpret_cast<detail::patch_t *>(_journal_raw);
  _control->retired_ = reinterpret_cast<detail::retired_t *>(_retire_raw);
  _control->views_ = reinterpret_cast<detail::view_t **>(_viewreg_raw);
  for (std::size_t _index = 0; _index < _slots; ++_index) {
    ::new (static_cast<void *>(_control->key_table_ + _index))
        std::atomic<detail::key_cell_t *>(nullptr);
    ::new (static_cast<void *>(_control->global_ + _index))
        std::atomic<detail::value_cell_t *>(nullptr);
  }
  for (std::size_t _index = 0; _index < config.journal_size_; ++_index)
    ::new (static_cast<void *>(_control->journal_ + _index)) detail::patch_t{};
  for (std::size_t _index = 0; _index < config.max_threads_; ++_index) {
    auto *_raw = _control->shard_.allocate_tagged(
        detail::view_tag_k, detail::view_object_bytes(_slots), alignof(std::uint64_t));
    if (_raw == nullptr)
      throw std::runtime_error("environment: view allocation failed");
    std::memset(_raw, 0, detail::view_object_bytes(_slots));
    _control->views_[_index] = reinterpret_cast<detail::view_t *>(_raw);
  }

  if (config.read_process_env_)
    detail::load_process_env(_control);
  detail::load_dotenv(_control, dotenv_path);

  detail::control_ref() = _control;
  detail::epoch_ref().fetch_add(1, std::memory_order_relaxed);
}

// Release the module's handle. Joined threads must already have retired their
// views (which they do on thread exit), and the arena must outlive the call.
inline void shutdown() noexcept {
  detail::control_ref() = nullptr;
  detail::epoch_ref().fetch_add(1, std::memory_order_relaxed);
}

// Bytes the module's shard will occupy for a given configuration. Usable in a
// constant expression, so you can derive a compile-time arena budget:
//
//     constexpr environment_config_t env_k{};
//     constexpr std::size_t env_bytes_k = get_environment_footprint(env_k);
constexpr std::size_t get_environment_footprint(const environment_config_t &config) {
  arena::object_spec_t _specs[detail::spec_count_k] = {};
  std::size_t _slots = 0;
  std::size_t _retire_capacity = 0;
  detail::make_specs(config, _specs, _slots, _retire_capacity);
  return arena::shard_footprint(_specs);
}

// The whole arena budget the module needs: its footprint rounded up to a page,
// plus `extra_bytes` for anything else you carve into the same arena. Feed the
// result straight into an arena_config_t.
constexpr std::size_t get_environment_arena_bytes(const environment_config_t &config,
                                                  std::size_t extra_bytes = 0) {
  return arena::round_to_page(get_environment_footprint(config) + extra_bytes);
}

constexpr arena::arena_config_t get_environment_arena_config(const environment_config_t &config,
                                                             std::size_t extra_bytes = 0) {
  arena::arena_config_t _arena_config{};
  _arena_config.size_ = get_environment_arena_bytes(config, extra_bytes);
  return _arena_config;
}

// A snapshot of the store's activity. Updates are unbounded; this reports how
// many keys exist, how many updates have committed, and how many retired cells
// are still pinned by lagging views (the only thing that can exhaust the
// fixed value pool).
struct environment_stats_t {
  std::size_t keys_used_ = 0;
  std::size_t max_keys_ = 0;
  std::uint64_t total_updates_ = 0;
  std::size_t journal_size_ = 0;
  std::size_t retired_pending_ = 0;
  std::size_t max_threads_ = 0;
  std::size_t active_views_ = 0;
}; // struct environment_stats_t

[[nodiscard]] inline environment_stats_t get_environment_stats() noexcept {
  auto *_control = detail::control_ref();
  if (_control == nullptr)
    return {};
  environment_stats_t _stats;
  _stats.max_keys_ = _control->max_keys_;
  _stats.journal_size_ = _control->journal_size_;
  _stats.max_threads_ = _control->max_threads_;
  _stats.total_updates_ = _control->version_.load(std::memory_order_acquire);
  for (std::size_t _index = 0; _index < _control->slots_; ++_index) {
    if (_control->key_table_[_index].load(std::memory_order_acquire) != nullptr)
      ++_stats.keys_used_;
  }
  for (std::size_t _index = 0; _index < _control->max_threads_; ++_index) {
    if (std::atomic_ref<std::uint32_t>(_control->views_[_index]->active_)
            .load(std::memory_order_acquire) != 0)
      ++_stats.active_views_;
  }
  const std::size_t _head_k = _control->retire_head_.load(std::memory_order_acquire);
  const std::size_t _tail_k = _control->retire_tail_.load(std::memory_order_acquire);
  _stats.retired_pending_ =
      (_tail_k - _head_k + _control->retire_capacity_) & _control->retire_mask_;
  return _stats;
}

// One key/value pair as seen through the store's key table. Both views point
// straight at immutable arena cells and stay valid while the module lives and
// the entry is not overwritten by a later set_env (which republishes the value
// view, never the key view).
struct environment_entry_t {
  std::string_view key_;
  std::string_view value_;
}; // struct environment_entry_t

// Copy up to `capacity` entries into `out` and return how many were written.
// This is an introspection helper, not a hot path: it walks the whole key table,
// so a real-time loop should still know its keys by name. Returns 0 when the
// module is uninitialized or `out` is null.
[[nodiscard]] inline std::size_t get_environment_keys(environment_entry_t *out,
                                                      std::size_t capacity) noexcept {
  auto *_control = detail::control_ref();
  if (_control == nullptr || out == nullptr)
    return 0;
  std::size_t _written = 0;
  for (std::size_t _index = 0; _index < _control->slots_ && _written < capacity; ++_index) {
    const detail::key_cell_t *_key = _control->key_table_[_index].load(std::memory_order_acquire);
    if (_key == nullptr)
      continue;
    const detail::value_cell_t *_value = _control->global_[_index].load(std::memory_order_acquire);
    out[_written].key_ = {detail::key_bytes(_key), _key->len_};
    out[_written].value_ = _value != nullptr ? detail::as_view(_value) : std::string_view{};
    ++_written;
  }
  return _written;
}

// Read a key. On the fast path this only compares the thread's cached version
// with the global counter; a behind thread replays the delta ring, and a thread
// that fell past the ring reseeds from the global slots. The returned view is
// valid until the next get_env/set_env on this thread. Returns `fallback` when
// the key is missing or the module is not initialized.
[[nodiscard]] inline std::string_view get_env(std::string_view name, std::string_view fallback) {
  auto *_control = detail::control_ref();
  if (_control == nullptr || name.size() > _control->max_key_len_)
    return fallback;
  const std::size_t _id_k = detail::lookup(_control, name.data(), name.size());
  if (_id_k == detail::no_key_k)
    return fallback;

  detail::view_t *_view = detail::get_thread_view(_control);
  if (_view == nullptr) {
    const detail::value_cell_t *_cell = _control->global_[_id_k].load(std::memory_order_acquire);
    return _cell != nullptr ? detail::as_view(_cell) : fallback;
  }
  const std::uint64_t _version_k = _control->version_.load(std::memory_order_acquire);
  if (_view->my_version_ != _version_k)
    detail::sync_view(_control, _view, _version_k);
  detail::value_cell_t **_slots = detail::cached_slots(_view);
  detail::value_cell_t *_cell = _slots[_id_k];
  if (_cell == nullptr) {
    _cell = _control->global_[_id_k].load(std::memory_order_acquire);
    _slots[_id_k] = _cell;
  }
  detail::publish_view(_view, detail::tls_guard());
  return _cell != nullptr ? detail::as_view(_cell) : fallback;
}

// Publish a new immutable value cell for `name`, retire the previous one,
// append the update to the delta ring and bump the global version. Retired
// cells whose version is behind every live view are recycled. Writers serialize
// on a short mutex; readers never take it. Returns false when the module is
// uninitialized, a bound is exceeded, or the value pool is momentarily
// exhausted by pinned retired cells.
inline bool set_env(std::string_view name, std::string_view value) {
  auto *_control = detail::control_ref();
  if (_control == nullptr || name.empty() || name.size() > _control->max_key_len_ ||
      value.size() > _control->max_value_len_)
    return false;

  const std::lock_guard _lock_k{_control->writer_};
  detail::reclaim(_control);

  const std::uint64_t _version_k = _control->version_.load(std::memory_order_relaxed);
  const std::size_t _id_k = detail::find_or_insert(_control, name.data(), name.size());
  if (_id_k == detail::no_key_k)
    return false;
  auto *_raw =
      _control->shard_.allocate_tagged(detail::value_tag_k,
                                       detail::value_object_bytes(_control->max_value_len_),
                                       alignof(std::uint64_t));
  if (_raw == nullptr)
    return false;
  auto *_cell = reinterpret_cast<detail::value_cell_t *>(_raw);
  _cell->len_ = static_cast<std::uint32_t>(value.size());
  _cell->pad_ = 0;
  std::memcpy(detail::value_bytes(_cell), value.data(), value.size());

  detail::value_cell_t *_old = _control->global_[_id_k].load(std::memory_order_relaxed);
  _control->global_[_id_k].store(_cell, std::memory_order_release);

  detail::patch_t &_patch = _control->journal_[_version_k & _control->journal_mask_];
  _patch.tag_.store(0, std::memory_order_release);
  _patch.key_id_.store(static_cast<std::uint32_t>(_id_k), std::memory_order_relaxed);
  _patch.value_.store(_cell, std::memory_order_relaxed);
  _patch.tag_.store(_version_k + 1, std::memory_order_release);
  _control->version_.store(_version_k + 1, std::memory_order_release);

  if (_old != nullptr) {
    const std::size_t _tail_k = _control->retire_tail_.load(std::memory_order_relaxed);
    _control->retired_[_tail_k] = {_old, _version_k};
    _control->retire_tail_.store((_tail_k + 1) & _control->retire_mask_, std::memory_order_release);
  }

  // Keep the writer's own view current so it never pins retired cells.
  detail::view_slot_guard_t &_guard = detail::tls_guard();
  if (_guard.view_ != nullptr &&
      _guard.epoch_ == detail::epoch_ref().load(std::memory_order_relaxed)) {
    if (_guard.view_->my_version_ != _control->version_.load(std::memory_order_relaxed))
      detail::sync_view(_control, _guard.view_, _version_k + 1);
    detail::publish_view(_guard.view_, _guard);
  }

  // The cell retired above is the only new garbage; nothing to reclaim when
  // the key had no previous value.
  if (_old != nullptr)
    detail::reclaim(_control);
  return true;
}

} // namespace frameworkcpp::environment
