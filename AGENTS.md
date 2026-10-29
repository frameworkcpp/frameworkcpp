# AGENTS.md

Contract for AI agents (and humans) working in this repository. Read it before
touching anything.

## Mission

FrameworkCpp is a **header-only C++20 framework for building real-time systems**.
It is modular: each module lives under `include/frameworkcpp/<module>/` and is
self-contained. The current modules are `arena` (memory management),
`environment` (environment management) and `reactor` (event loops on io_uring
and epoll). `frameworkcpp/concurrency.hpp` holds the concurrency vocabulary
shared by the modules. The project targets RT constraints: no dynamic
allocation at runtime on hot paths and lock-free fast paths.

## Style

The style encodes the *kind* of every identifier into its spelling, so a name
can never collide with another kind (for example a member named like its type)
and a reader can tell at a glance what a name is.

| Kind | Rule | Example |
| --- | --- | --- |
| Class / struct / union | `_t` suffix | `memory_t`, `object_spec_t` |
| Enum (the type) | `_t` suffix | `concurrency_model_t` |
| Exception class | `exception` suffix (not `_t`) | `arena_overflow_exception` |
| Enumerator | `_` suffix | `single_thread_` |
| Type alias | `_t` suffix | `arena_byte_allocator_t` |
| Function-pointer alias | `_fn` suffix | `arena_warning_fn` |
| Data member (public or private) | `_` suffix | `capacity_`, `hdr_` |
| Constant (namespace, class or local) | `_k` suffix | `max_memory_shards_k` |
| Local variable | `_` prefix | `_iterator` |
| Local constant | `_` prefix + `_k` suffix | `_limit_k` |
| Parameter | no prefix/suffix | `bytes`, `alignment` |
| Function / method | `snake_case`, action-naming | `get_capacity`, `reset` |

Rules that follow from the table:

- A constant that is also a member drops the member `_` to avoid a reserved
  `__`: `limit_k`, not `limit__k`.
- Names are complete words, never abbreviations: `_iterator`, not `_it`.
- A function's name must translate the action of calling it. Accessors that
  merely obtain state are `get_`/`set_` (`get_capacity`, `set_env`); boolean
  queries are `is_`/`has_`; mutators are verbs (`reset`, `reserve`). A `std`
  trait or contract wins over this rule (`allocate`, `deallocate`, `value_type`,
  `what`, `operator bool`).
- Template parameters: `T`/`U` are fine when generic; name them after their role
  when it is known (`Allocator`, not `T`).
- `const` correctness, `[[nodiscard]]`, `noexcept` and `explicit` are orthogonal
  to naming; use them wherever they apply.

Formatting is enforced by `.clang-format`: 2-space indentation, no tabs, access
specifiers at the class-body level, `&`/`*` bound to the name, namespace bodies
not indented. Data members are declared first, then `public:`; every class and
namespace that spans more than a few lines closes with a `// class x_t` or
`// namespace x` comment.

## Hard rules

- Every new file carries the `SPDX-License-Identifier: AGPL-3.0-only` header.
- Shipped modules are **header-only**. No `.cpp` files for public modules.
- Fast paths are lock-free; locks belong to setup or bounded, documented paths.
- Arena/value memory is immutable once published; never mutate in place.
- Do not introduce vendor libraries without discussing it first.
- Do not silently change public API or semantics; align docs and behavior in
  the same change.
- Follow the naming rules above; they exist to keep identifiers unambiguous.

## Before you finish a change

1. `cmake --preset debug` → build → `ctest --preset debug` (all tests pass).
2. Run `asan` and `ubsan` presets. Run `tsan` when the change touches
   concurrency.
3. Format: `cmake --build --preset debug --target format`; clean `clang-tidy` on
   changed TU's (naming is enforced by `readability-identifier-naming`).
4. Update the human-friendly docs in `docs/` whenever public API changes, and
   follow `docs/contribute/documentation.md` whenever you write or edit them.
5. Follow conventional commits: `feat(scope):`, `fix(scope):`,
   `docs(scope):`, `refactor(scope):`, `ci(scope):`, `test(scope):`.

## What agents must NOT do

- No "surprise" edits: no unrelated reformatting, no untracked behavior changes
  mixed into a commit.
- No touching CI triggers, credentials, or workflow secrets without explicit
  request.
- No generating READMEs or guides unless asked.
- No committing unless explicitly asked.
- No in-place mutation of published values in the arena/environment design.
- No `new`/`delete`/`malloc` in arena fast paths or environment read paths.
