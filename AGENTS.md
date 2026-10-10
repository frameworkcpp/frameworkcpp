# AGENTS.md

Contract for AI agents (and humans) working in this repository. Read it before
touching anything.

## Mission

FrameworkCpp is a **header-only C++20 framework for building real-time systems**.
It is modular: each module lives under `include/frameworkcpp/<module>/` and is
self-contained. The current modules are `arena` (memory management) and, in
progress, `env` (environment management). The project targets RT constraints:
no dynamic allocation at runtime on hot paths and lock-free fast paths.

## Hard rules

- Every new file carries the `SPDX-License-Identifier: AGPL-3.0-only` header.
- Shipped modules are **header-only**. No `.cpp` files for public modules.
- Public-facing types end in **`_t`** (`arena_t`, `arena_shard_t`,
  `arena_unique_ptr_t`, …). `enum class`es and `using` aliases keep their
  functional name (`concurrency_model`, `arena_byte_allocator`). Adapters and
  mixins also keep functional names (`arena_allocator<T>`,
  `arena_memory_resource`, `arena_enable_shared_from_this<T>`).
- Private class members end in `_` (`hdr_`, `region_`, `registry_mutex_`).
- Fast paths are lock-free; locks belong to setup or bounded, documented paths.
- Arena/value memory is immutable once published; never mutate in place.
- Do not introduce vendor libraries without discussing it first.
- Do not silently change public API or semantics; align docs and behavior in
  the same change.

## Before you finish a change

1. `cmake --preset debug` → build → `ctest --preset debug` (all tests pass).
2. Run `asan` and `ubsan` presets. Run `tsan` when the change touches
   concurrency.
3. Format: `clang-format` (see CI `lint` job); clean `clang-tidy` on changed
   TU's.
4. Update the human-friendly docs in `docs/` whenever public API changes.
5. Follow conventional commits: `feat(scope):`, `fix(scope):`,
   `docs(scope):`, `refactor(scope):`, `ci(scope):`, `test(scope):`.

## What agents must NOT do

- No "surprise" edits: no unrelated reformatting, no untracked behavior
  changes mixed into a commit.
- No touching CI triggers, credentials, or workflow secrets without explicit
  request.
- No generating READMEs or guides unless asked.
- No committing unless explicitly asked.
- No in-place mutation of published values in the arena/env design.
- No `new`/`delete`/`malloc` in arena fast paths or env read paths.