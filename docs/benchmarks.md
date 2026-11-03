# Benchmarks

The repository ships a benchmark suite for the hot paths of every module. It is
not a bragging tool: it exists so that, in this commit, on the runners GitHub
gives you, you can read what each operation costs and how its tail behaves. The
numbers below are one snapshot of that world, not a scoreboard.

## What is measured

Every public hot path the framework exposes has a number. Every benchmark
reports a distribution, never a bare average.

| Module | Benchmark | Operation |
| --- | --- | --- |
| memory | `memory/allocate_deallocate` | one `allocate` + `deallocate` pair on a 64-byte block |
| memory | `memory/allocate_batch/16\|256\|4096` | distribution per op inside a burst of allocations |
| memory | `memory/allocate_tagged` | allocation by tag, not by size walk |
| memory | `memory/bucket_acquire_release` | same bucket, single thread |
| memory | `memory/bucket_lock_free_mt/1\|2\|4` | same `lock_free_` bucket, N threads |
| memory | `memory/bucket_thread_safe_mt/1\|2\|4` | same `thread_safe_` bucket, N threads |
| memory | `memory/bucket_private_mt/1\|2\|4` | one owned bucket per thread, `single_thread_` |
| environment | `environment/get_env` | read a pinned key (lock-free path) |
| environment | `environment/set_env` | write the same key (writer mutex path) |
| environment | `environment/get_env_mt/1\|2\|4` | same table, N readers |
| environment | `environment/set_env_mt/1\|2\|4` | same store, N writers |
| reactor | `reactor/post_empty/64\|512\|4096` | post N empty jobs to one queue |
| reactor | `reactor/process_empty/1\|10\|100\|1k\|10k` | post + drain, wall time per batch |
| reactor | `reactor/burst_latency/1\|10\|100\|1k\|10k` | end-to-end latency per op in a burst |
| reactor | `reactor/timer_schedule_cancel` | schedule + cancel with the heap |
| reactor | `reactor/socket_send` / `socket_recv` | round trip through one socketpair |
| reactor | `reactor/echo_round_trip` | accept, recv, send, read |
| reactor | `reactor/accept` | TCP accept per connection |
| reactor | `reactor/post_mp/1\|2\|4` | N producers, one queue, `lock_free_` producers |
| reactor | `reactor/post_mp_thread_safe/1\|2\|4` | N producers, one queue, `thread_safe_` producers |
| reactor | `reactor/post_private_mt/1\|2\|4` | one owned queue per thread, `single_thread_` producers |

Concurrency models come from `frameworkcpp::concurrency_model_t`:
`single_thread_` (one thread owns the resource), `lock_free_` (CAS), and
`thread_safe_` (mutex). Each resource documents its own model; the benchmarks
only measure the combinations a user would actually deploy.

## Running the suite

```bash
cmake --preset bench
cmake --build --preset bench
./build/bench/benchmarks/frameworkcpp_memory_bench
./build/bench/benchmarks/frameworkcpp_environment_bench
./build/bench/benchmarks/frameworkcpp_reactor_bench
```

Every binary takes the usual Google Benchmark flags, so you can narrow the run
or write the results to a file:

```bash
./build/bench/benchmarks/frameworkcpp_reactor_bench \
  --benchmark_filter='reactor/burst_latency.*' \
  --benchmark_out=reactor.json --benchmark_out_format=json
```

CI runs the same binaries natively on `amd64` and `arm64` and uploads the JSON
artifacts, so every commit carries the numbers of the runner it was built on.
Nothing gates on them: benchmarks are a lens, not a check.

## Reading the output

```text
Benchmark               Time        CPU   Iterations   UserCounters...
memory/bucket_private_mt/4   710 us   612 us     1200 ... p50_ns=42 p99_ns=140 max_ns=3.4k count=2.4k
```

- `Time` / `CPU` — Google Benchmark's measurement. On the reactor benchmarks
  they are nearly equal: the caller spins while the queue does its work, so
  the wall clock and the burned CPU time are the same thing.
- `count` — number of samples that produced the histogram.
- `min_ns` … `p50_ns` … `p999_ns` — the distribution of per-operation cost.
  `min_ns = 0` means the operation is at or below the harness floor (it
  cannot measure faster than the calibrated read of the cycle counter).
- `max_ns` — the worst sample; the one you actually care about on a real-time
  path.
- `mean_ns` / `stddev_ns` — classic summary, useful next to the percentiles.
- `items_per_second` — Google Benchmark's own derived rate (see
  `SetItemsProcessed`).
- `aggregate_items_per_second` — on multi-thread benchmarks, the aggregate
  rate of the whole batch.
- `dropped_posts` — how many ops the queue rejected with a full pool. A
  non-zero count means the producer saturated the consumer; the latency
  numbers still hold for the ops that did go through.

## What the numbers mean, and what they don't

This suite measures on shared CI hardware, on a single commit, at a single
moment. Treat it like a thermometer, not a trophy:

- **Frequency changes during a run.** The same binary shows memory ops at
  3860 MHz in one second and 2700 MHz in the next. Percentiles move with it.
- **CPU ≈ Time is harness design.** The reactor caller busy-spins until the
  queue replies; the scheduler is deliberately not in the measurement.
- **Cross-kernel comparisons are meaningless.** Different runner, different
  frequency, different cache sizes, different ASLR — don't read them.
- **ASLR adds noise.** `*** WARNING *** ASLR is enabled …` is printed on every
  run and the noise is real; the shape of the distribution is not affected.
- **The harness has a floor.** Anything at or below the cost of two
  `cycle_count()` calls measures `0`. That is the price of honest per-operation
  timing, and it is paid for with the `count` histogram rather than with a
  lowered resolution.
- **Multi-thread numbers measure both contention and topology.** The
  `*_mp_*` and `one bucket, N threads` benchmarks show how much the
  contended resource costs; the `*_private_mt` family shows the ceiling when
  nothing is shared. Both are true numbers; they answer different questions.

A number from this suite means: *in this commit, on this runner, on this
day, N samples of this operation behaved like this.* Anything more is
storytelling.

## Reading the table like a real-time engineer

If you are sizing a deadline budget, use `p99_ns` and `max_ns`, not
`items_per_second`:

"controller loop every 1 ms; one `allocate` per frame; p99 28 ns, max 4 µs
outlier once in a million — budget it" is a legitimate engineering decision.
"102 million allocations per second" is not a decision, it is a party trick.

## Where to go next

If you are about to add a new operation to the framework, read
[Measuring Performance](/contribute/measuring) first: it explains why a number
is only worth something when its distribution is reported and the machine it
was measured on is named.
