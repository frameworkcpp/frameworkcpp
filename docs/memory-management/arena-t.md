# The Arena

`arena_t` is the owner of every byte. It holds the reserved memory, carves
shards out of it, and knows the overall budget.

```cpp
#include <frameworkcpp/arena/arena.hpp>
```

## Building one

```cpp
arena_t();                              // default: 1 MiB initial region
explicit arena_t(const arena_config&);  // see Configuration
```

The first region is reserved lazily when the first shard is created, sized to
`max(config.size, required_bytes)`. When a shard does not fit the current
regions, one more `mmap` region is added.

## Carving shards

```cpp
arena_shard_t create_shard(std::uint64_t tag, std::initializer_list<object_spec> specs);
arena_shard_t create_shard(std::uint64_t tag, std::span<const object_spec> specs);
arena_shard_t from(std::uint64_t tag) const;
```

- A tag identifies a shard. Creating a tag you already made returns the same
  shard again (idempotent).
- `from` fetches an existing shard by tag, or an empty view when it does not
  exist yet.
- If any spec is invalid (see [Configuration](configuration)), `create_shard`
  returns an empty view without carving anything.

## Freeing memory you no longer have the shard for

```cpp
void deallocate(void* p) noexcept;
```

`arena_t::deallocate` is the untyped safety net: it finds the owning region,
checks the block really came from here, and frees it into its bucket. When you
still have the shard, prefer `arena_shard_t::deallocate`, which is faster.

## Knowing the budget

```cpp
std::size_t shard_count() const noexcept; // how many shards have been carved
std::size_t capacity()      const noexcept; // total bytes reserved (all regions)
std::size_t used()          const noexcept; // bytes carved so far (high-water mark)
std::size_t live()          const noexcept; // blocks currently in your hands
```

## Example

```cpp
using namespace frameworkcpp::arena;

arena_t arena;

auto workers = arena.create_shard(1, {
    {0, sizeof(std::uint32_t), alignof(std::uint32_t), 512},
});

auto* slot = workers.allocate<std::uint32_t>();
workers.deallocate(slot);
```