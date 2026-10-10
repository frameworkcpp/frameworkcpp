// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Internal layout of the environment module. Every structure here is carved
// into the module's arena shard as a fixed-capacity array; nothing is ever
// allocated on the heap. Published cells are immutable: a set_env builds a new
// value cell and links to it, it never edits a live one.
namespace frameworkcpp::environment::detail {

inline constexpr std::uint64_t kShardTag = 0x656e762d61726500ULL;
inline constexpr std::uint64_t kControlTag = 1;
inline constexpr std::uint64_t kKeyTag = 2;
inline constexpr std::uint64_t kValueTag = 3;
inline constexpr std::uint64_t kViewTag = 4;
inline constexpr std::uint64_t kKeyTableTag = 5;
inline constexpr std::uint64_t kGlobalTag = 6;
inline constexpr std::uint64_t kRingTag = 7;
inline constexpr std::size_t kSpecCount = 7;

inline constexpr std::size_t kNoKey = static_cast<std::size_t>(-1);

constexpr std::uint64_t fnv1a(const char *data, std::size_t len) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr std::size_t next_pow2(std::size_t value) {
    std::size_t p = 1;
    while (p < value)
        p <<= 1;
    return p;
}

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// A key is immutable once published. `id` is the slot it occupies in the key
// table, so a reader that resolves a key once can index its own view by id.
struct key_cell_t {
    std::uint32_t hash;
    std::uint32_t len;
    // key bytes follow at offset sizeof(key_cell_t)
};

// A value is immutable once published. set_env always builds a fresh cell.
struct value_cell_t {
    std::uint32_t len;
    std::uint32_t pad;
    // value bytes follow at offset sizeof(value_cell_t)
};

// One entry of the delta journal. Entries are appended in version order and
// never rewritten, so readers can replay a suffix without tearing.
struct patch_t {
    std::uint32_t key_id;
    std::uint32_t pad;
    value_cell_t *value;
};

// Per-thread cache, indexed by key-table slot id.
struct view_t {
    std::uint64_t my_version;
    value_cell_t *cached[1];
};

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

inline std::size_t key_object_bytes(std::size_t max_key_len) {
    return align_up(sizeof(key_cell_t) + max_key_len, alignof(std::uint64_t));
}

inline std::size_t value_object_bytes(std::size_t max_value_len) {
    return align_up(sizeof(value_cell_t) + max_value_len, alignof(std::uint64_t));
}

inline std::size_t view_object_bytes(std::size_t slots) {
    return align_up(offsetof(view_t, cached) + (slots * sizeof(value_cell_t *)),
                    alignof(std::uint64_t));
}

inline std::string_view as_view(const value_cell_t *cell) {
    return {value_bytes(cell), cell->len};
}

} // namespace frameworkcpp::environment::detail