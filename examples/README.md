# Example workloads

Small programs with known performance characteristics, built into `build/examples/` with
`-O2 -g -fno-omit-frame-pointer`. They are integration fixtures for the profiler: the engine has
no knowledge of them, and the tests only check that it *finds* the documented evidence.

The "Observed" lines are from the sessions run while building this project (2-vCPU VM; your
numbers will differ).

## A — Inefficient algorithm (`inefficient_loop.cpp`)

Counts unique values. `--mode slow` checks membership with a linear scan of a growing vector
(O(n²)); `--mode fast` sorts and deduplicates (O(n log n)). `--n` sets the input size (90 000).

- **Expected bottleneck:** `contains_linear` (self cost), reached via `main → count_unique_slow`.
- **Run:** `build/examples/inefficient_loop --mode slow`
- **Profile:** `build/detective record --runs 5 --warmup 1 --out slow.json -- build/examples/inefficient_loop --mode slow`, same with `--mode fast`, then `build/detective compare slow.json fast.json`.
- **Evidence:** a high-severity `cpu_hotspot` on `contains_linear` with the dominant path as a fact,
  and the "replace the linear search with an indexed lookup" recommendation.
  *Observed:* 852/854 samples (99.8 %) self in `contains_linear`; comparison slow → fast
  −98.3 % wall (5 vs 5 runs, p = 0.012), `contains_linear` −99.8 pp.
- **Verify independently:** double `--n` and time it — the slow mode takes ~4× longer
  (quadratic), the fast mode ~2×. A stripped copy, `inefficient_loop_stripped`, exercises the
  missing-symbols path (functions shown as `inefficient_loop_stripped+0x…`).

## B — Allocation-heavy program (`allocation_stress.cpp`)

Tokenizes CSV-like lines. `--mode naive` builds temporary strings per character and a fresh
vector per record; `--mode reuse` keeps buffers across iterations. `--records` (400 000).

- **Expected bottleneck:** heap allocation in `tokenize_naive`.
- **Profile:** `build/detective record --alloc -- build/examples/allocation_stress --mode naive`
  (sampling + allocation tracking), or `--mode none --alloc` for counts only.
- **Evidence:** `allocator_cpu` (CPU in malloc/free/operator new, with `tokenize_naive` as the
  driving caller) and, with `--alloc`, `allocation_pressure` with exact counts and call sites.
  *Observed:* naive 34.7 M allocations (26.8 M from `tokenize_naive`) vs. reuse 4.8 M; the
  tracker itself appears as an `observer_overhead` data-quality note, not as a hotspot.
- **Verify independently:** `valgrind --tool=massif` or `ltrace -c -e malloc` on a reduced
  `--records`, or compare wall time of the two modes (naive ≈ 1.2 s, reuse ≈ 0.2 s here).

## C — Recursive computation (`recursive_workload.cpp`)

`--mode naive` computes Fibonacci by exponential recursion; `--mode memo` memoizes. `--n` (40).
`--depth D` additionally walks a D-deep recursive chain (use > 127 to see stack truncation
warnings).

- **Expected bottleneck:** `fib_naive`, recursion depth ≈ n.
- **Profile:** `build/detective record -- build/examples/recursive_workload --mode naive`
- **Evidence:** `cpu_hotspot` on `fib_naive` and a `recursion` finding with the observed depth;
  the flame graph shows a tall tower of `fib_naive` frames.
  *Observed:* 99 % of samples in `fib_naive`, depth up to ~30 frames in one stack.
- **Verify independently:** `--mode memo` finishes in microseconds; each +1 on `--n` multiplies
  naive run time by ~1.6 (the golden ratio).

## D — Multithreaded contention (`contention_demo.cpp`)

Four threads each do 8 000 chunks of arithmetic. `--mode coarse` holds one global mutex while
computing; `--mode fine` computes outside the lock and only publishes under it.

- **Expected bottleneck:** serialization on the mutex (threads blocked, i.e. *off-CPU*).
- **Profile:** `build/detective record --runs 5 -- build/examples/contention_demo --mode coarse`, and `--mode fine`; compare.
- **Evidence:** cpu-clock sampling cannot see blocked time, so the hotspot is the computation
  itself (`compute_chunk`). The contention shows up as the process-level `off_cpu` finding:
  ~1.1 of 2 possible cores busy and ~16 000 voluntary context switches (≈ 18 000/s), with the
  "reduce lock scope" recommendation. *Observed:* coarse → fine −56.9 % wall (p = 0.012), and
  `__lll_lock_wait` / `pthread_mutex_lock` shares drop.
- **Verify independently:** `perf sched` or bcc `offcputime` to see where threads block;
  `/usr/bin/time -v` reports the same context-switch counts.

## E — Data processing (`data_processing.cpp`)

Aggregates revenue per (region, product) over 2 M generated CSV lines. `--impl naive` uses
`istringstream`/`getline`, `std::stod` and a `std::map<std::string, double>` keyed by a
concatenated string; `--impl fast` slices `string_view`s, parses integers by hand and uses an
`unordered_map<string_view, …>`.

- **Expected bottleneck:** stream parsing and string handling in `process_naive`; in both
  modes, input generation (`snprintf`) is a fixed cost.
- **Profile:** `build/detective record --runs 5 -- build/examples/data_processing --impl naive`, then `--impl fast`; compare.
- **Evidence:** `iostream_cpu` (getline, strtod, locale/printf internals) and `string_cpu`, with
  `process_naive` as the driving caller, plus a `hot_path` ending in `process_naive`.
  *Observed:* naive → fast −63.8 % wall (p = 0.012); `process_naive` −8.5 pp self share. The
  fast run's remaining `iostream_cpu` finding comes from `generate()`'s `snprintf` — a correct
  finding about the input generator, not about `process_fast`.
- **Verify independently:** time the two implementations directly; both print the same total.

## Instrumentation — `instrumented_demo.cpp`

A two-thread request pipeline linked with `detective_instr`, using `DETECTIVE_SCOPE`,
`DETECTIVE_FUNCTION` and `DETECTIVE_MARK`. `--sleepy` adds a 200 µs sleep inside `db.query` to
show that instrumented times are wall-clock.

- **Profile:** `build/detective record --mode instrumentation -- build/examples/instrumented_demo`
- **Evidence:** `db.query` dominates instrumented self time (`instr_hot_scope`), cache hit/miss
  marks (19 000 / 1 000), and `checksum_byte` triggers `excessive_calls` with *low* confidence
  because its measured time is comparable to the marker's own clock cost.
- **Verify independently:** remove the `DETECTIVE_FUNCTION()` from `checksum_byte` and compare;
  run with `--sleepy` and observe `db.query` grow by at least 1 000 × 200 µs (observed: 39 ms → 355 ms total, because `sleep_for` overshoots), while process CPU time barely changes.
