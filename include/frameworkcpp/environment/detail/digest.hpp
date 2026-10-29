// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Internal layout of the environment module. Every structure here is carved
// into the module's memory shard as a fixed-capacity array; nothing is ever
// allocated on the heap. Published cells are immutable: a set_env builds a new
// value cell and links to it, it never edits a live one. Retired cells are
// recycled by an epoch reader (see environment.hpp): a cell is only reused once
// every live view has moved past the version that retired it.
namespace frameworkcpp::environment::detail {

inline constexpr std::uint64_t shard_tag_k = 0x656e762d61726500ULL;
inline constexpr std::uint64_t control_tag_k = 1;
inline constexpr std::uint64_t key_tag_k = 2;
inline constexpr std::uint64_t value_tag_k = 3;
inline constexpr std::uint64_t view_tag_k = 4;
inline constexpr std::uint64_t key_table_tag_k = 5;
inline constexpr std::uint64_t global_tag_k = 6;
inline constexpr std::uint64_t journal_tag_k = 7;
inline constexpr std::uint64_t retire_tag_k = 8;
inline constexpr std::uint64_t view_reg_tag_k = 9;
inline constexpr std::size_t spec_count_k = 9;

inline constexpr std::size_t no_key_k = static_cast<std::size_t>(-1);

constexpr std::uint64_t fnv1a(const char *data, std::size_t len) {
  std::uint64_t _hash = 1469598103934665603ULL;
  for (std::size_t _index = 0; _index < len; ++_index) {
    _hash ^= static_cast<unsigned char>(data[_index]);
    _hash *= 1099511628211ULL;
  }
  return _hash;
}

constexpr std::size_t next_pow2(std::size_t value) {
  std::size_t _power = 1;
  while (_power < value)
    _power <<= 1;
  return _power;
}

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// A key is immutable once published. `id` is the slot it occupies in the key
// table, so a reader that resolves a key once can index its own view by id.
struct key_cell_t {
  std::uint32_t hash_;
  std::uint32_t len_;
  // key bytes follow at offset sizeof(key_cell_t)
}; // struct key_cell_t

// A value is immutable once published. set_env always builds a fresh cell.
struct value_cell_t {
  std::uint32_t len_;
  std::uint32_t pad_;
  // value bytes follow at offset sizeof(value_cell_t)
}; // struct value_cell_t

// One slot of the delta ring. `tag` is the commit gate: a writer bumps it to
// `version + 1` (release) once key_id/value are in place; a reader only trusts
// a slot after reading `tag` twice around the fields, which also makes the
// slot safe to rewrite when the ring wraps.
struct patch_t {
  std::atomic<std::uint64_t> tag_{0};
  std::atomic<std::uint32_t> key_id_{0};
  std::atomic<value_cell_t *> value_{nullptr};
}; // struct patch_t

// A retired value cell waiting to be recycled. `retired_version` is the update
// that superseded it; the cell may be reused once every live view has moved
// past that version.
struct retired_t {
  value_cell_t *cell_;
  std::uint64_t retired_version_;
}; // struct retired_t

// Per-thread cache. `published` and `active` are touched from other threads (the
// reclaimer) through std::atomic_ref; the rest is thread-private. The cache is a
// trailing array of `value_cell_t*` (one per key-table slot) that follows this
// header in memory and is reached through cached_slots(); it is not declared as
// a member because a trailing [0]/[1] array is not standard C++20.
struct view_t {
  std::uint64_t my_version_;
  std::uint64_t published_;
  std::uint32_t active_;
  std::uint32_t pad_;
}; // struct view_t

inline value_cell_t **cached_slots(view_t *view) noexcept {
  return reinterpret_cast<value_cell_t **>(view + 1);
}

inline const char *key_bytes(const key_cell_t *cell) {
  return reinterpret_cast<const char *>(cell + 1);
}

inline char *key_bytes(key_cell_t *cell) {
  return reinterpret_cast<char *>(cell + 1);
}

inline char *value_bytes(value_cell_t *cell) {
  return reinterpret_cast<char *>(cell + 1);
}

inline const char *value_bytes(const value_cell_t *cell) {
  return reinterpret_cast<const char *>(cell + 1);
}

constexpr std::size_t key_object_bytes(std::size_t max_key_len) {
  return align_up(sizeof(key_cell_t) + max_key_len, alignof(std::uint64_t));
}

constexpr std::size_t value_object_bytes(std::size_t max_value_len) {
  return align_up(sizeof(value_cell_t) + max_value_len, alignof(std::uint64_t));
}

constexpr std::size_t view_object_bytes(std::size_t slots) {
  return align_up(sizeof(view_t) + (slots * sizeof(value_cell_t *)), alignof(std::uint64_t));
}

inline std::string_view as_view(const value_cell_t *cell) {
  return {value_bytes(cell), cell->len_};
}

} // namespace frameworkcpp::environment::detail
