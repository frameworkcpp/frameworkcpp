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
    arena::arena_shard_t shard;
    std::atomic<key_cell_t *> *key_table; // [slots]
    std::atomic<value_cell_t *> *global;  // [slots]
    patch_t *journal;                     // [journal_size]
    retired_t *retired;                   // [retire_capacity], ring
    view_t **views;                       // [max_threads], pre-allocated
    std::atomic<std::uint64_t> version;
    std::atomic<std::size_t> retire_head;
    std::atomic<std::size_t> retire_tail;
    std::size_t slots;
    std::size_t slot_mask;
    std::size_t journal_size;
    std::size_t journal_mask;
    std::size_t retire_capacity;
    std::size_t retire_mask;
    std::size_t max_keys;
    std::size_t max_key_len;
    std::size_t max_value_len;
    std::size_t max_threads;
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

constexpr void make_specs(const environment_config_t &config,
                          arena::object_spec_t (&specs)[kSpecCount],
                          std::size_t &slots,
                          std::size_t &retire_capacity) {
    slots = next_pow2(config.max_keys * 2);
    retire_capacity = next_pow2(config.max_keys + config.value_cells);
    specs[0] = {kControlTag, sizeof(control_t), alignof(control_t), 1};
    specs[1] = {
        kKeyTag, key_object_bytes(config.max_key_len), alignof(std::uint64_t), config.max_keys};
    specs[2] = {kValueTag,
                value_object_bytes(config.max_value_len),
                alignof(std::uint64_t),
                config.max_keys + config.value_cells};
    specs[3] = {kViewTag, view_object_bytes(slots), alignof(std::uint64_t), config.max_threads};
    specs[4] = {kKeyTableTag,
                slots * sizeof(std::atomic<key_cell_t *>),
                alignof(std::atomic<key_cell_t *>),
                1};
    specs[5] = {kGlobalTag,
                slots * sizeof(std::atomic<value_cell_t *>),
                alignof(std::atomic<value_cell_t *>),
                1};
    specs[6] = {kJournalTag, config.journal_size * sizeof(patch_t), alignof(patch_t), 1};
    specs[7] = {kRetireTag, retire_capacity * sizeof(retired_t), alignof(retired_t), 1};
    specs[8] = {kViewRegTag, config.max_threads * sizeof(view_t *), alignof(view_t *), 1};
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
    struct stat info = {};
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

struct view_slot_guard {
    view_slot_guard() = default;
    view_slot_guard(const view_slot_guard &) = delete;
    view_slot_guard &operator=(const view_slot_guard &) = delete;
    view_slot_guard(view_slot_guard &&) = delete;
    view_slot_guard &operator=(view_slot_guard &&) = delete;
    ~view_slot_guard() {
        if (view != nullptr && epoch == epoch_ref().load(std::memory_order_relaxed) &&
            control_ref() != nullptr)
            std::atomic_ref<std::uint32_t>(view->active).store(0, std::memory_order_release);
    }

    view_t *view = nullptr;
    std::uint64_t epoch = 0;
    std::uint64_t published = 0;
};

inline view_slot_guard &tls_guard() noexcept {
    static thread_local view_slot_guard guard;
    return guard;
}

// Refresh the version this thread advertises to the reclaimer, but only when it
// actually moved. On the steady-state read path nothing changed, so no shared
// atomic is touched.
inline void publish_view(view_t *view, view_slot_guard &guard) {
    if (guard.published == view->my_version)
        return;
    std::atomic_ref<std::uint64_t>(view->published)
        .store(view->my_version, std::memory_order_release);
    guard.published = view->my_version;
}

inline void clear_cache(control_t *control, view_t *view) {
    value_cell_t **slots = cached_slots(view);
    for (std::size_t i = 0; i < control->slots; ++i)
        slots[i] = nullptr;
}

// One pre-allocated view per thread. A thread without one claims a free slot
// (active == 0) with a CAS; on exit the slot is marked inactive again, so slots
// are recycled instead of burned. Claiming happens once per thread, never on the
// read path.
inline view_t *thread_view(control_t *control) {
    view_slot_guard &guard = tls_guard();
    const std::uint64_t epoch = epoch_ref().load(std::memory_order_relaxed);
    if (guard.view != nullptr && guard.epoch == epoch)
        return guard.view;
    guard.view = nullptr;
    guard.epoch = epoch;
    guard.published = 0;
    for (std::size_t i = 0; i < control->max_threads; ++i) {
        view_t *view = control->views[i];
        std::uint32_t expected = 0;
        if (!std::atomic_ref<std::uint32_t>(view->active)
                 .compare_exchange_strong(
                     expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            continue;
        clear_cache(control, view);
        view->my_version = control->version.load(std::memory_order_acquire);
        // A reclaimer that reads this slot before we publish sees the previous
        // owner's version, which is never newer than ours, so it stays
        // conservative. Publish, then hand the view to the thread.
        guard.published = view->my_version;
        std::atomic_ref<std::uint64_t>(view->published)
            .store(view->my_version, std::memory_order_release);
        guard.view = view;
        return guard.view;
    }
    return nullptr;
}

inline void reseed_view(control_t *control, view_t *view, std::uint64_t target) {
    clear_cache(control, view);
    view->my_version = target;
}

inline void sync_view(control_t *control, view_t *view, std::uint64_t target) {
    const std::uint64_t my = view->my_version;
    if (target <= my) {
        view->my_version = target;
        return;
    }
    if (target - my >= control->journal_size) {
        reseed_view(control, view, target);
        return;
    }
    for (std::uint64_t version = my; version < target; ++version) {
        const patch_t &patch = control->journal[version & control->journal_mask];
        const std::uint64_t t1 = patch.tag.load(std::memory_order_acquire);
        if (t1 != version + 1) { // slot overwritten by a newer wrap; fall back
            reseed_view(control, view, target);
            return;
        }
        const std::uint32_t key_id = patch.key_id.load(std::memory_order_relaxed);
        value_cell_t *cell = patch.value.load(std::memory_order_relaxed);
        if (patch.tag.load(std::memory_order_acquire) != t1) {
            reseed_view(control, view, target);
            return;
        }
        cached_slots(view)[key_id] = cell;
    }
    view->my_version = target;
}

// Recycle retired cells that no live view can still reference. Called by the
// single writer under its mutex. A cell retired at version r is safe to reuse
// once every active view has published a version greater than r.
inline void reclaim(control_t *control) {
    std::uint64_t min_version = control->version.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < control->max_threads; ++i) {
        view_t *view = control->views[i];
        if (std::atomic_ref<std::uint32_t>(view->active).load(std::memory_order_acquire) == 0)
            continue;
        const std::uint64_t published =
            std::atomic_ref<std::uint64_t>(view->published).load(std::memory_order_acquire);
        min_version = std::min(min_version, published);
    }
    std::size_t head = control->retire_head.load(std::memory_order_relaxed);
    const std::size_t tail = control->retire_tail.load(std::memory_order_relaxed);
    while (head != tail) {
        const retired_t &entry = control->retired[head];
        if (entry.retired_version >= min_version)
            break;
        control->shard.deallocate_bytes(entry.cell);
        head = (head + 1) & control->retire_mask;
    }
    control->retire_head.store(head, std::memory_order_relaxed);
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
        config.value_cells == 0 || config.max_threads == 0 || config.journal_size == 0)
        throw std::invalid_argument("environment capacity must be non-zero");
    if ((config.journal_size & (config.journal_size - 1)) != 0)
        throw std::invalid_argument("environment journal_size must be a power of two");

    arena::object_spec_t specs[detail::kSpecCount] = {};
    std::size_t slots = 0;
    std::size_t retire_capacity = 0;
    detail::make_specs(config, specs, slots, retire_capacity);

    auto shard = arena.create_shard(
        detail::kShardTag, std::span<const arena::object_spec_t>(specs, detail::kSpecCount));
    if (!shard)
        throw std::runtime_error("environment: arena has no room for the module");

    shard.get_bucket(detail::kKeyTag, arena::concurrency_model::lock_free);
    shard.get_bucket(detail::kValueTag, arena::concurrency_model::lock_free);

    auto *control_raw = shard.allocate_tagged(
        detail::kControlTag, sizeof(detail::control_t), alignof(detail::control_t));
    if (control_raw == nullptr)
        throw std::runtime_error("environment: control block allocation failed");
    auto *control = ::new (static_cast<void *>(control_raw)) detail::control_t{};
    control->shard = shard;
    control->slots = slots;
    control->slot_mask = slots - 1;
    control->journal_size = config.journal_size;
    control->journal_mask = config.journal_size - 1;
    control->retire_capacity = retire_capacity;
    control->retire_mask = retire_capacity - 1;
    control->max_keys = config.max_keys;
    control->max_key_len = config.max_key_len;
    control->max_value_len = config.max_value_len;
    control->max_threads = config.max_threads;

    auto *key_raw = shard.allocate_tagged(detail::kKeyTableTag,
                                          slots * sizeof(std::atomic<detail::key_cell_t *>),
                                          alignof(std::atomic<detail::key_cell_t *>));
    auto *global_raw = shard.allocate_tagged(detail::kGlobalTag,
                                             slots * sizeof(std::atomic<detail::value_cell_t *>),
                                             alignof(std::atomic<detail::value_cell_t *>));
    auto *journal_raw = shard.allocate_tagged(detail::kJournalTag,
                                              config.journal_size * sizeof(detail::patch_t),
                                              alignof(detail::patch_t));
    auto *retire_raw = shard.allocate_tagged(detail::kRetireTag,
                                             retire_capacity * sizeof(detail::retired_t),
                                             alignof(detail::retired_t));
    auto *viewreg_raw = shard.allocate_tagged(detail::kViewRegTag,
                                              config.max_threads * sizeof(detail::view_t *),
                                              alignof(detail::view_t *));
    if (key_raw == nullptr || global_raw == nullptr || journal_raw == nullptr ||
        retire_raw == nullptr || viewreg_raw == nullptr)
        throw std::runtime_error("environment: table allocation failed");

    control->key_table = reinterpret_cast<std::atomic<detail::key_cell_t *> *>(key_raw);
    control->global = reinterpret_cast<std::atomic<detail::value_cell_t *> *>(global_raw);
    control->journal = reinterpret_cast<detail::patch_t *>(journal_raw);
    control->retired = reinterpret_cast<detail::retired_t *>(retire_raw);
    control->views = reinterpret_cast<detail::view_t **>(viewreg_raw);
    for (std::size_t i = 0; i < slots; ++i) {
        ::new (static_cast<void *>(control->key_table + i))
            std::atomic<detail::key_cell_t *>(nullptr);
        ::new (static_cast<void *>(control->global + i))
            std::atomic<detail::value_cell_t *>(nullptr);
    }
    for (std::size_t i = 0; i < config.journal_size; ++i)
        ::new (static_cast<void *>(control->journal + i)) detail::patch_t{};
    for (std::size_t i = 0; i < config.max_threads; ++i) {
        auto *raw = control->shard.allocate_tagged(
            detail::kViewTag, detail::view_object_bytes(slots), alignof(std::uint64_t));
        if (raw == nullptr)
            throw std::runtime_error("environment: view allocation failed");
        std::memset(raw, 0, detail::view_object_bytes(slots));
        control->views[i] = reinterpret_cast<detail::view_t *>(raw);
    }

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

// Bytes the module's shard will occupy for a given configuration. Usable in a
// constant expression, so you can derive a compile-time arena budget:
//
//     constexpr environment_config_t kEnv{};
//     constexpr std::size_t kEnvBytes = environment_footprint(kEnv);
constexpr std::size_t environment_footprint(const environment_config_t &config) {
    arena::object_spec_t specs[detail::kSpecCount] = {};
    std::size_t slots = 0;
    std::size_t retire_capacity = 0;
    detail::make_specs(config, specs, slots, retire_capacity);
    return arena::shard_footprint(specs);
}

// The whole arena budget the module needs: its footprint rounded up to a page,
// plus `extra_bytes` for anything else you carve into the same arena. Feed the
// result straight into an arena_config_t.
constexpr std::size_t environment_arena_bytes(const environment_config_t &config,
                                              std::size_t extra_bytes = 0) {
    return arena::round_to_page(environment_footprint(config) + extra_bytes);
}

constexpr arena::arena_config_t environment_arena_config(const environment_config_t &config,
                                                         std::size_t extra_bytes = 0) {
    arena::arena_config_t arena_config{};
    arena_config.size = environment_arena_bytes(config, extra_bytes);
    return arena_config;
}

// A snapshot of the store's activity. Updates are unbounded; this reports how
// many keys exist, how many updates have committed, and how many retired cells
// are still pinned by lagging views (the only thing that can exhaust the
// fixed value pool).
struct environment_stats_t {
    std::size_t keys_used = 0;
    std::size_t max_keys = 0;
    std::uint64_t total_updates = 0;
    std::size_t journal_size = 0;
    std::size_t retired_pending = 0;
    std::size_t max_threads = 0;
    std::size_t active_views = 0;
};

[[nodiscard]] inline environment_stats_t environment_stats() noexcept {
    auto *control = detail::control_ref();
    if (control == nullptr)
        return {};
    environment_stats_t stats;
    stats.max_keys = control->max_keys;
    stats.journal_size = control->journal_size;
    stats.max_threads = control->max_threads;
    stats.total_updates = control->version.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < control->slots; ++i) {
        if (control->key_table[i].load(std::memory_order_acquire) != nullptr)
            ++stats.keys_used;
    }
    for (std::size_t i = 0; i < control->max_threads; ++i) {
        if (std::atomic_ref<std::uint32_t>(control->views[i]->active)
                .load(std::memory_order_acquire) != 0)
            ++stats.active_views;
    }
    const std::size_t head = control->retire_head.load(std::memory_order_acquire);
    const std::size_t tail = control->retire_tail.load(std::memory_order_acquire);
    stats.retired_pending = (tail - head + control->retire_capacity) & control->retire_mask;
    return stats;
}

// One key/value pair as seen through the store's key table. Both views point
// straight at immutable arena cells and stay valid while the module lives and
// the entry is not overwritten by a later set_env (which republishes the value
// view, never the key view).
struct environment_entry_t {
    std::string_view key;
    std::string_view value;
};

// Copy up to `capacity` entries into `out` and return how many were written.
// This is an introspection helper, not a hot path: it walks the whole key table,
// so a real-time loop should still know its keys by name. Returns 0 when the
// module is uninitialized or `out` is null.
[[nodiscard]] inline std::size_t environment_keys(environment_entry_t *out,
                                                  std::size_t capacity) noexcept {
    auto *control = detail::control_ref();
    if (control == nullptr || out == nullptr)
        return 0;
    std::size_t written = 0;
    for (std::size_t i = 0; i < control->slots && written < capacity; ++i) {
        const detail::key_cell_t *key = control->key_table[i].load(std::memory_order_acquire);
        if (key == nullptr)
            continue;
        const detail::value_cell_t *value = control->global[i].load(std::memory_order_acquire);
        out[written].key = {detail::key_bytes(key), key->len};
        out[written].value = value != nullptr ? detail::as_view(value) : std::string_view{};
        ++written;
    }
    return written;
}

// Read a key. On the fast path this only compares the thread's cached version
// with the global counter; a behind thread replays the delta ring, and a thread
// that fell past the ring reseeds from the global slots. The returned view is
// valid until the next get_env/set_env on this thread. Returns `fallback` when
// the key is missing or the module is not initialized.
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
    detail::value_cell_t **slots = detail::cached_slots(view);
    detail::value_cell_t *cell = slots[id];
    if (cell == nullptr) {
        cell = control->global[id].load(std::memory_order_acquire);
        slots[id] = cell;
    }
    detail::publish_view(view, detail::tls_guard());
    return cell != nullptr ? detail::as_view(cell) : fallback;
}

// Publish a new immutable value cell for `name`, retire the previous one,
// append the update to the delta ring and bump the global version. Retired
// cells whose version is behind every live view are recycled. Writers serialize
// on a short mutex; readers never take it. Returns false when the module is
// uninitialized, a bound is exceeded, or the value pool is momentarily
// exhausted by pinned retired cells.
inline bool set_env(std::string_view name, std::string_view value) {
    auto *control = detail::control_ref();
    if (control == nullptr || name.empty() || name.size() > control->max_key_len ||
        value.size() > control->max_value_len)
        return false;

    const std::lock_guard lock{control->writer};
    detail::reclaim(control);

    const std::uint64_t version = control->version.load(std::memory_order_relaxed);
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

    detail::value_cell_t *old = control->global[id].load(std::memory_order_relaxed);
    control->global[id].store(cell, std::memory_order_release);

    detail::patch_t &patch = control->journal[version & control->journal_mask];
    patch.tag.store(0, std::memory_order_release);
    patch.key_id.store(static_cast<std::uint32_t>(id), std::memory_order_relaxed);
    patch.value.store(cell, std::memory_order_relaxed);
    patch.tag.store(version + 1, std::memory_order_release);
    control->version.store(version + 1, std::memory_order_release);

    if (old != nullptr) {
        const std::size_t tail = control->retire_tail.load(std::memory_order_relaxed);
        control->retired[tail] = {old, version};
        control->retire_tail.store((tail + 1) & control->retire_mask, std::memory_order_release);
    }

    // Keep the writer's own view current so it never pins retired cells.
    detail::view_slot_guard &guard = detail::tls_guard();
    if (guard.view != nullptr &&
        guard.epoch == detail::epoch_ref().load(std::memory_order_relaxed)) {
        if (guard.view->my_version != control->version.load(std::memory_order_relaxed))
            detail::sync_view(control, guard.view, version + 1);
        detail::publish_view(guard.view, guard);
    }

    // The cell retired above is the only new garbage; nothing to reclaim when
    // the key had no previous value.
    if (old != nullptr)
        detail::reclaim(control);
    return true;
}

} // namespace frameworkcpp::environment