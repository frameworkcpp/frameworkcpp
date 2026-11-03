# Measuring Performance

How to add benchmarks to FrameworkCpp, and what makes a number trustworthy.
This page is the measurement counterpart of
[Writing Documentation](/contribute/documentation): follow it whether you are
adding one operation or a whole module.

## Start from the question, not the number

"How fast is this?" is never the real question. The real question is attached
to a decision:

- "Can my 1 kHz control loop afford one allocation per frame?" — you need
  `p99` and `max`, not the average.
- "Does the lock-free bucket still scale when four threads hammer it?" — you
  need the 1/2/4-thread scaling curve, not a single number.
- "How bad is the writer mutex under contention?" — you need `p999` and
  `stddev_ns`, not `items_per_second`.

Before you write the benchmark, write down the decision it has to answer. If
no decision changes with the answer, the benchmark does not belong in the
suite.

## The dimensions that matter

Every number this suite reports carries the same axes:

1. **Distribution.** `min`, `p50`, `p90`, `p99`, `p99.9`, `max`, `mean`,
   `stddev` — never a bare average. The histogram counts samples in 4 ns
   buckets and derives the percentiles from the buckets.
2. **Threads.** 1, 2 and 4, always. One is the baseline; two shows the
   presence or absence of scaling; four shows whether contention or the
   scheduler is the wall.
3. **Ownership.** Every benchmark runs in both ownership modes:
   **shared resource** (one bucket/table/queue touched by N threads) — how
   much contention costs — and **per-thread resources** (one bucket/table/
   queue per thread) — the ceiling when nothing is shared. They answer
   different questions; neither is a compromise of the other.
4. **Batch vs operation.** Benchmarks that operate on batches exist so both
   the per-op cost and the batch-shaped tail are visible.

## How a benchmark is built here

Three pieces in `benchmarks/detail/` make every benchmark measure the same
way:

**`cycle_count()`** reads the hardware cycle counter directly (`rdtsc` on
`x86_64`, `cntvct_el0` on `aarch64`) instead of asking the C++ runtime for
the time. Reason one: `steady_clock::now()` costs more than a small
allocation. Reason two: the framework is only interested in how many CPU
cycles the operation burns.

**`harness_overhead_cycles()`** — the two cycle-counter reads cost cycles
too. `record()` subtracts the calibrated floor from every sample; the floor
is the minimum observed cost of a read–read pair, not an estimate. Anything
at or below it is reported as `0`: that is honest, and better than inventing
precision.

**The histogram** distributes samples into buckets: 4 ns steps cover
[0, 2048); 64 ns steps cover [2048, 16512); one overflow bucket above that.
Per operation the cost is one shift, one or two compares, and one increment —
no allocation, no growth, no lock. At the end, the buckets are summed and the
percentiles are read from them. Each percentile is the *lower bound* of its
bucket: the reported value can underestimate by at most the bucket width;
`min_ns` and `max_ns` are always exact.

Each multi-thread worker owns its own histogram; at the end of the run they
are summed and the merged distribution is reported. No atomics contention at
measurement time, and nothing is hidden inside a helper thread.

## What every benchmark must report

- `count` — number of samples.
- `min_ns`, `p50_ns`, `p90_ns`, `p99_ns`, `p999_ns`, `max_ns`, `mean_ns`,
  `stddev_ns` — the distribution.
- `items_per_second` — derived throughput (`SetItemsProcessed`).
- On multi-thread runs: `aggregate_items_per_second` — the batch-wide rate.
- On saturating queues: `dropped_posts` — how many operations the queue
  refused. Non-zero does not invalidate the latency samples; it says the
  producers outran the consumer.

Environment benchmarks get a special note: **each benchmark initialises its
own instance of the module.** The module keeps one global store whose views
are claimed per thread; a reader that goes quiet pins retired cells forever.
That is correct inside one process, but incompatible with "nine benchmarks in
one binary share the store": the retired cells never get recycled and the
next writer runs dry. Every bench calls `init()` and `shutdown()` for its own
instance — setup happens once per benchmark, never inside the measured loop.

## Writing a benchmark: the checklist

- [ ] The operation being measured is a real hot path, not setup or
      teardown.
- [ ] `benchmark::DoNotOptimize` is applied to the result, or the result is
      stored to a volatile — otherwise the compiler may elide the call and
      you measure nothing.
- [ ] The operation is measured with `cycle_count()` and nothing else runs
      inside the timed window by accident: no iostream, no locking, no
      allocation of the measurement structures themselves.
- [ ] A histogram is recorded per operation and `report_histogram` is used
      to publish the counters (batch benchmarks are the documented exception:
      they record the batch, and their names say so).
- [ ] Threads: 1, 2 and 4 benchmarks are added if the operation is meant to
      be used from more than one thread.
- [ ] The concurrency model matches the deploy you would recommend
      (`single_thread_` for one-owner fast paths, `lock_free_`/`thread_safe_`
      for shared ones) — never the cheapest option.
- [ ] The benchmark cleans up after itself: no global state left behind, no
      module left initialised.
- [ ] One full JSON run is produced and inspected before commit.

## Common mistakes

**Measuring the timer instead of the op.** If your sample time shrinks to
zero as you tune the operation, you are measuring mostly `cycle_count()`.
Compare against a no-op sample; the floor subtraction lives inside
`record()`; nothing else is honest.

**Reporting a mean as if it were the truth.** A mean of 100 ns with a p99 of
8 µs is the lottery you buy every frame at 60 Hz. The tail is the answer;
the average is the brochure.

**Comparing numbers across machines.** The same runner changes frequency
inside a single run of the suite — on purpose (power saving) and by accident
(neighbours). Use same-host comparisons only; the GitHub runner is not your
production machine either.

**Reporting a symmetric benchmark you would not deploy.** A `lock_free_`
stress test on a bucket whose user will only ever touch from one thread is
noise; the profile of that call is dominated by an owner-never-contended
path, which the `single_thread_` benchmark shows instead.

**Forgetting to drain.** A benchmark that fills a queue and never consumes
it measures exhaustion, not latency. If an operation can report "full", say
so: `dropped_posts` exists for exactly that case.

**Bloating the harness.** One extra call inside the timed window is already
visible in the fastest benchmarks. Keep the body minimal; put the setup
outside the measurement.

## What the numbers mean

A benchmark from this suite means: "in this commit, on the runner that
produced this log, at this time, N samples behaved like this." It is a
snapshot with hardware attached, not a promise about your machine. It is
good enough to see regressions, to compare two variants of the same code, and
to reason about worst-case costs. It is not good enough to brag about, and
it is not good enough to decide for someone else's workload.

If a number surprises you, do not publish it: profile with `perf`, `vtune`,
or your hardware counters first. A surprising benchmark is a bug in your
understanding before it is a result.

## A worked example

Suppose someone asks you to measure the `initialize()` of a new module. First
ask: one-shot setup, or called per frame with a fill-level argument?

If it is one-shot: no benchmark. The module's documentation carries the cost
of setup phrased as "expect N µs on this runner".

If it is called per frame: measure that path with a histogram, on the same
pattern as the reactor benchmarks — `DoNotOptimize` the result, one sample
per call, merge under `module/init`. If it allocates, report the bucket pair;
if it writes to a shared structure, report the contention variant as well,
and one per-thread variant if the call can be scoped per thread. 1 and 4
threads; `single_thread_` buckets only if that is the recommended deploy —
the shared variant exists to show why not.

If any of that is ambiguous, ask before writing the benchmark, not after.
