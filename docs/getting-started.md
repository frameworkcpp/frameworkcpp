# Getting Started

FrameworkCpp is a **header-only C++20 framework** for building real-time
systems. It is modular: each module is a self-contained set of headers under
`include/frameworkcpp/<module>/`, and you only pay for the modules you use.

The documented modules are [Memory Management](/memory-management/), the
arena-backed allocator layer, and [Environment Management](/environment/), a
dotenv-style configuration store built on top of it. More modules are on the
way.

## What you need

- A C++20 compiler (GCC 10+, Clang 11+).
- CMake 3.23 or newer (only required to build the tests).
- POSIX: the arena module uses `mmap`, so it targets Linux by design.

## Using it as a header-only library

No installation is required to *use* the headers:

```bash
git clone https://github.com/frameworkcpp/frameworkcpp.git
g++ -std=c++20 -I frameworkcpp/include app.cpp -o app
```

## Building the tests

The repository ships CMake presets so the classic flow just works:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `asan`, `tsan`, `ubsan` and `release`.

```bash
# sanitizer builds pull in clang through the preset
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

> TSAN requires a 64-bit platform. Sanitizers do not run on QEMU-emulated
> architectures; the CI matrix reflects exactly which combinations are valid.

## Installing it

Make `find_package(frameworkcpp)` available to other projects:

```bash
cmake --preset release -DFRAMEWORKCPP_BUILD_TESTING=OFF
cmake --build --preset release
cmake --install build/release --prefix /usr/local
```

Then a consumer project can use it like any CMake package:

```cmake
find_package(frameworkcpp REQUIRED)
target_link_libraries(app PRIVATE frameworkcpp::frameworkcpp)
```

```cpp
#include <frameworkcpp/arena/arena.hpp>

int main()
{
    frameworkcpp::arena::arena_t arena;
    // create a shard (tag 1) holding one bucket (tag 0): 64 slots of int.
    auto shard = arena.create_shard(
        1,
        {{0, sizeof(int), alignof(int), 64}});
    auto *p = shard.allocate<int>();
    shard.deallocate(p);
}
```

Prefer one include? `<frameworkcpp/frameworkcpp.hpp>` pulls in every module;
reach for the granular headers when you only need one and want faster builds.

## What's next

Read the [Memory Management](/memory-management/) section to understand the
model — shards, buckets, concurrency models and smart pointers — then the
[Environment Management](/environment/) module that builds the configuration
store on top of it.