# Memory reservation

A real-time system cannot afford to discover how much memory it needs while it
is already running. The framework's answer is that every module knows its own
footprint before it allocates a single byte, and that number is available at
compile time.

This page describes the contract. If you are adding a module, this is what it
has to implement.

## The contract

A module that carves its state out of a memory exposes one `constexpr`
function:

```cpp
namespace frameworkcpp::environment {

constexpr std::size_t precompute(const environment_config_t &config) noexcept;

}
```

It returns the exact number of bytes the module will need for that
configuration. "Exact" is the important word: not an upper bound, not an
estimate. The module reserves precisely that and nothing more.

Because it is `constexpr`, the budget is known while the program is still being
compiled:

```cpp
constexpr environment::environment_config_t env_k{};
constexpr std::size_t env_bytes_k = environment::precompute(env_k);
static_assert(env_bytes_k > 0);
```

## Combining modules

The memory is shared, so the budgets have to add up. `make_reservation` and
`merge_reservations` do that in one expression:

```cpp
#include <frameworkcpp/memory/module_memory.hpp>

constexpr std::size_t total_k =
    memory::merge_reservations(
        memory::make_reservation<environment::environment_module_t>("environment", env_k),
        memory::make_reservation<reactor::reactor_module_t>("reactor", rea_k));

memory::memory_config_t memory_config;
memory_config.size_ = memory::round_to_page(total_k);
memory::memory_t memory{memory_config};
```

Each module ships a small adapter so the call site reads the same for every
module:

```cpp
struct environment_module_t {
    static constexpr std::size_t precompute(const environment_config_t &config) noexcept {
        return environment::precompute(config);
    }
};
```

## Adding a module to the contract

A new module has to do three things.

**1. Size its state from the configuration.** Every buffer, table and pool is
derived from the config, so the footprint is a function of it.

**2. Compute the footprint with the memory's own formula.** The memory knows how
to size a shard from a table of object descriptors, and that formula is
`constexpr`:

```cpp
template <std::size_t N> constexpr std::size_t shard_footprint(const object_spec_t (&specs)[N]);
```

Building the same descriptor table for the calculation and for the allocation is
what makes the number exact:

```cpp
constexpr void make_specs(const reactor_config_t &config,
                          memory::object_spec_t (&specs)[reactor_spec_count_k]) {
    // ... fill the descriptors ...
}

constexpr std::size_t precompute(const reactor_config_t &config) noexcept {
    memory::object_spec_t specs[reactor_spec_count_k] = {};
    make_specs(config, specs);
    return memory::shard_footprint(specs);
}
```

**3. Expose the adapter.** One struct with a static `precompute`, so the module
can be named in a `merge_reservations` call.

## Why it matters

Without this contract, every module grows its own way: one estimates, another
over-reserves, a third asks the user for a number. Then the memory is sized by
guesswork and the first module to grow pushes the others out.

With the contract, the program declares its whole memory budget in one place,
at compile time, and the runtime never has to ask "do we have room?" — the
answer was settled before the first request arrived.

```cpp
constexpr std::size_t total_k = /* every module's footprint */;
static_assert(total_k < 4 * 1024 * 1024, "the budget grew beyond what we planned for");
```

That assertion is the payoff: a module that quietly grows its tables breaks the
build instead of the deadline.

## The trade-offs

- **The footprint is fixed at bind time.** A module that needs more room cannot
  ask for it later. Reserving generously costs address space, not time.
- **The calculation is duplicated.** The descriptor table is built twice, once
  for the size and once for the allocation. That is what buys the exactness,
  and it costs nothing at runtime.
- **`constexpr` means no dynamic configuration.** A configuration that is only
  known at runtime cannot be sized at compile time. Modules that need that have
  to accept a runtime budget instead.
