// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// A dotenv-style environment store backed entirely by an arena shard. Values
// live in immutable arena cells; readers keep a per-thread view and catch up
// through a bounded, append-only patch journal, so a warmed-up get_env never
// touches a shared atomic beyond the single global version counter.
//
// include: #include <frameworkcpp/environment/environment.hpp>

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
    arena::arena_shard_t shard;
    std::atomic<key_cell_t *> *key_table;
    std::atomic<value_cell_t *> *global;
    patch_t *journal;
    std::atomic<std::uint64_t> version;
    std::size_t slots;
    std::size_t slot_mask;
    std::size_t max_key_len;
    std::size_t max_value_len;
    std::size_t max_updates;
    std::mutex writer;
};

inline control_t *&control_ref() noexcept {
    static control_t *control = nullptr;
    return control;
}

// Bumped on every init/shutdown so stale per-thread views never touch a dead
// arena when their thread exits.
inline std::atomic<std::uint64_t> &epoch_ref() noexcept {
    static std::atomic<std::uint64_t> epoch{0};
    return epoch;
}

inline void make_specs(const environment_config_t &config,
                       arena::object_spec_t (&specs)[kSpecCount],
                       std::size_t &slots) {
    slots = next_pow2(config.max_keys * 2);
    specs[0] = {kControlTag, sizeof(control_t), alignof(control_t), 1};
    specs[1] = {
        kKeyTag, key_object_bytes(config.max_key_len), alignof(std::uint64_t), config.max_keys};
    specs[2] = {kValueTag,
                value_object_bytes(config.max_value_len),
                alignof(std::uint64_t),
                config.max_keys + config.max_updates};
    specs[3] = {kViewTag, view_object_bytes(slots), alignof(std::uint64_t), config.max_threads};
    specs[4] = {kKeyTableTag,
                slots * sizeof(std::atomic<key_cell_t *>),
                alignof(std::atomic<key_cell_t *>),
                1};
    specs[5] = {kGlobalTag,
                slots * sizeof(std::atomic<value_cell_t *>),
                alignof(std::atomic<value_cell_t *>),
                1};
    specs[6] = {kRingTag, config.max_updates * sizeof(patch_t), alignof(patch_t), 1};
}

inline std::size_t lookup(control_t *control, const char *name, std::size_t len) {
    const auto hash = static_cast<std::uint32_t>(fnv1a(name, len));
    std::size_t index = hash & control->slot_mask;
    for (std::size_t probe = 0; probe < control->slots; ++probe) {
        const key_cell_t *cell = control->key_table[index].load(std::memory_order_acquire);
        if (cell == nullptr)
            return kNoKey;
        if (cell->hash == hash && cell->len == len && std::memcmp(key_bytes(cell), name, len) == 0)
            return index;
        index = (index + 1) & control->slot_mask;
    }
    return kNoKey;
}

inline std::size_t find_or_insert(control_t *control, const char *name, std::size_t len) {
    const auto hash = static_cast<std::uint32_t>(fnv1a(name, len));
    std::size_t index = hash & control->slot_mask;
    for (std::size_t probe = 0; probe < control->slots; ++probe) {
        key_cell_t *cell = control->key_table[index].load(std::memory_order_relaxed);
        if (cell == nullptr) {
            auto *raw = control->shard.allocate_tagged(
                kKeyTag, key_object_bytes(control->max_key_len), alignof(std::uint64_t));
            if (raw == nullptr)
                return kNoKey;
            auto *fresh = reinterpret_cast<key_cell_t *>(raw);
            fresh->hash = hash;
            fresh->len = static_cast<std::uint32_t>(len);
            std::memcpy(key_bytes(fresh), name, len);
            control->key_table[index].store(fresh, std::memory_order_release);
            return index;
        }
        if (cell->hash == hash && cell->len == len && std::memcmp(key_bytes(cell), name, len) == 0)
            return index;
        index = (index + 1) & control->slot_mask;
    }
    return kNoKey;
}

inline bool store_initial(control_t *control,
                          const char *name,
                          std::size_t name_len,
                          const char *value,
                          std::size_t value_len) {
    if (name_len == 0 || name_len > control->max_key_len || value_len > control->max_value_len)
        return false;
    const std::size_t id = lookup(control, name, name_len);
    if (id != kNoKey)
        return false; // process environment wins over the file
    const std::size_t slot = find_or_insert(control, name, name_len);
    if (slot == kNoKey)
        return false;
    auto *raw = control->shard.allocate_tagged(
        kValueTag, value_object_bytes(control->max_value_len), alignof(std::uint64_t));
    if (raw == nullptr)
        return false;
    auto *cell = reinterpret_cast<value_cell_t *>(raw);
    cell->len = static_cast<std::uint32_t>(value_len);
    cell->pad = 0;
    std::memcpy(value_bytes(cell), value, value_len);
    control->global[slot].store(cell, std::memory_order_release);
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
    const char *eq = begin;
    while (eq < end && *eq != '=')
        ++eq;
    if (eq == end)
        return;
    const char *name_begin = begin;
    const char *name_end = eq;
    trim(name_begin, name_end);
    const char *value_begin = eq + 1;
    const char *value_end = end;
    trim(value_begin, value_end);
    if (value_end - value_begin >= 2 && ((*value_begin == '"' && value_end[-1] == '"') ||
                                         (*value_begin == '\'' && value_end[-1] == '\''))) {
        ++value_begin;
        --value_end;
    }
    store_initial(control,
                  name_begin,
                  static_cast<std::size_t>(name_end - name_begin),
                  value_begin,
                  static_cast<std::size_t>(value_end - value_begin));
}

inline void load_buffer(control_t *control, const char *data, std::size_t size) {
    std::size_t i = 0;
    while (i < size) {
        const std::size_t start = i;
        while (i < size && data[i] != '\n')
            ++i;
        load_line(control, data + start, data + i);
        if (i < size)
            ++i;
    }
}

inline void load_dotenv(control_t *control, const char *path) {
    if (path == nullptr)
        return;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0)
        return;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
        ::close(fd);
        return;
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    void *map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED)
        return;
    load_buffer(control, static_cast<const char *>(map), size);
    ::munmap(map, size);
}

inline void load_process_env(control_t *control) {
    for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const char *name = *entry;
        const char *eq = std::strchr(name, '=');
        if (eq == nullptr)
            continue;
        store_initial(
            control, name, static_cast<std::size_t>(eq - name), eq + 1, std::strlen(eq + 1));
    }
}

struct view_guard_t {
    std::uint64_t epoch = 0;
    view_t *view = nullptr;
};

// One view per thread. Views are carved from the module's shard and reclaimed
// when the arena is torn down; a thread never frees across a re-init, it just
// drops its stale view and takes a fresh one.
inline view_t *thread_view(control_t *control) {
    static thread_local view_guard_t guard;
    const std::uint64_t epoch = epoch_ref().load(std::memory_order_relaxed);
    if (guard.view != nullptr && guard.epoch == epoch)
        return guard.view;
    guard.view = nullptr;
    guard.epoch = epoch;
    const auto bytes = view_object_bytes(control->slots);
    auto *raw = control->shard.allocate_tagged(kViewTag, bytes, alignof(std::uint64_t));
    if (raw == nullptr)
        return nullptr;
    auto *view = reinterpret_cast<view_t *>(raw);
    std::memset(view, 0, bytes);
    guard.view = view;
    return guard.view;
}

inline void sync_view(control_t *control, view_t *view, std::uint64_t target) {
    for (std::uint64_t version = view->my_version; version < target; ++version) {
        const patch_t &patch = control->journal[version];
        view->cached[patch.key_id] = patch.value;
    }
    view->my_version = target;
}

} // namespace detail

// Carve the module's tables into `arena` and load the process environment
// first (it wins), then the dotenv file for any key still unset. Call once,
// from a single thread, before spawning readers or writers.
inline void
init(arena::arena_t &arena, const char *dotenv_path, const environment_config_t &config = {}) {
    if (detail::control_ref() != nullptr)
        throw std::logic_error("environment already initialized");
    if (config.max_keys == 0 || config.max_key_len == 0 || config.max_value_len == 0 ||
        config.max_updates == 0 || config.max_threads == 0)
        throw std::invalid_argument("environment capacity must be non-zero");

    arena::object_spec_t specs[detail::kSpecCount] = {};
    std::size_t slots = 0;
    detail::make_specs(config, specs, slots);

    auto shard = arena.create_shard(
        detail::kShardTag, std::span<const arena::object_spec_t>(specs, detail::kSpecCount));
    if (!shard)
        throw std::runtime_error("environment: arena has no room for the module");

    shard.get_bucket(detail::kKeyTag, arena::concurrency_model::lock_free);
    shard.get_bucket(detail::kValueTag, arena::concurrency_model::lock_free);
    shard.get_bucket(detail::kViewTag, arena::concurrency_model::lock_free);

    auto *control_raw = shard.allocate_tagged(
        detail::kControlTag, sizeof(detail::control_t), alignof(detail::control_t));
    if (control_raw == nullptr)
        throw std::runtime_error("environment: control block allocation failed");
    auto *control = ::new (static_cast<void *>(control_raw)) detail::control_t{};
    control->shard = shard;
    control->slots = slots;
    control->slot_mask = slots - 1;
    control->max_key_len = config.max_key_len;
    control->max_value_len = config.max_value_len;
    control->max_updates = config.max_updates;

    auto *key_raw = shard.allocate_tagged(detail::kKeyTableTag,
                                          slots * sizeof(std::atomic<detail::key_cell_t *>),
                                          alignof(std::atomic<detail::key_cell_t *>));
    auto *global_raw = shard.allocate_tagged(detail::kGlobalTag,
                                             slots * sizeof(std::atomic<detail::value_cell_t *>),
                                             alignof(std::atomic<detail::value_cell_t *>));
    auto *journal_raw = shard.allocate_tagged(
        detail::kRingTag, config.max_updates * sizeof(detail::patch_t), alignof(detail::patch_t));
    if (key_raw == nullptr || global_raw == nullptr || journal_raw == nullptr)
        throw std::runtime_error("environment: table allocation failed");

    control->key_table = reinterpret_cast<std::atomic<detail::key_cell_t *> *>(key_raw);
    control->global = reinterpret_cast<std::atomic<detail::value_cell_t *> *>(global_raw);
    control->journal = reinterpret_cast<detail::patch_t *>(journal_raw);
    for (std::size_t i = 0; i < slots; ++i) {
        ::new (static_cast<void *>(control->key_table + i))
            std::atomic<detail::key_cell_t *>(nullptr);
        ::new (static_cast<void *>(control->global + i))
            std::atomic<detail::value_cell_t *>(nullptr);
    }
    for (std::size_t i = 0; i < config.max_updates; ++i)
        ::new (static_cast<void *>(control->journal + i)) detail::patch_t{};

    if (config.read_process_env)
        detail::load_process_env(control);
    detail::load_dotenv(control, dotenv_path);

    detail::control_ref() = control;
    detail::epoch_ref().fetch_add(1, std::memory_order_relaxed);
}

// Release the module's handle. Joined threads must already have retired their
// views (which they do on thread exit), and the arena must outlive the call.
inline void shutdown() noexcept {
    detail::control_ref() = nullptr;
    detail::epoch_ref().fetch_add(1, std::memory_order_relaxed);
}

// Bytes the module's shard will occupy for a given configuration.
inline std::size_t environment_footprint(const environment_config_t &config) {
    arena::object_spec_t specs[detail::kSpecCount] = {};
    std::size_t slots = 0;
    detail::make_specs(config, specs, slots);
    return arena::shard_footprint(specs);
}

// Read a key. On the fast path this only compares the thread's cached version
// with the global counter; a behind thread replays the journal delta. Returns
// `fallback` when the key is missing or the module is not initialized.
[[nodiscard]] inline std::string_view get_env(std::string_view name, std::string_view fallback) {
    auto *control = detail::control_ref();
    if (control == nullptr || name.size() > control->max_key_len)
        return fallback;
    const std::size_t id = detail::lookup(control, name.data(), name.size());
    if (id == detail::kNoKey)
        return fallback;

    detail::view_t *view = detail::thread_view(control);
    if (view == nullptr) {
        const detail::value_cell_t *cell = control->global[id].load(std::memory_order_acquire);
        return cell != nullptr ? detail::as_view(cell) : fallback;
    }
    const std::uint64_t version = control->version.load(std::memory_order_acquire);
    if (view->my_version != version)
        detail::sync_view(control, view, version);
    detail::value_cell_t *cell = view->cached[id];
    if (cell == nullptr) {
        cell = control->global[id].load(std::memory_order_acquire);
        view->cached[id] = cell;
    }
    return cell != nullptr ? detail::as_view(cell) : fallback;
}

// Publish a new immutable value cell for `name`, append it to the journal and
// bump the global version. Writers serialize on a short mutex; readers never
// take it. Returns false when the module is uninitialized, a bound is exceeded
// or the fixed capacities are exhausted.
inline bool set_env(std::string_view name, std::string_view value) {
    auto *control = detail::control_ref();
    if (control == nullptr || name.empty() || name.size() > control->max_key_len ||
        value.size() > control->max_value_len)
        return false;

    const std::lock_guard lock{control->writer};
    const std::uint64_t version = control->version.load(std::memory_order_relaxed);
    if (version >= control->max_updates)
        return false;

    const std::size_t id = detail::find_or_insert(control, name.data(), name.size());
    if (id == detail::kNoKey)
        return false;
    auto *raw = control->shard.allocate_tagged(detail::kValueTag,
                                               detail::value_object_bytes(control->max_value_len),
                                               alignof(std::uint64_t));
    if (raw == nullptr)
        return false;
    auto *cell = reinterpret_cast<detail::value_cell_t *>(raw);
    cell->len = static_cast<std::uint32_t>(value.size());
    cell->pad = 0;
    std::memcpy(detail::value_bytes(cell), value.data(), value.size());
    control->global[id].store(cell, std::memory_order_release);

    detail::patch_t &patch = control->journal[version];
    patch.key_id = static_cast<std::uint32_t>(id);
    patch.pad = 0;
    patch.value = cell;
    control->version.store(version + 1, std::memory_order_release);
    return true;
}

} // namespace frameworkcpp::environment