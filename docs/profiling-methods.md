# Profiling methods

Performance Detective has two collection modes plus an optional allocation tracker. Their
results are stored and displayed separately and are never merged.

| | Sampling (Mode A, default) | Instrumentation (Mode B) | Allocation tracking |
|---|---|---|---|
| Needs rebuild | no (frame pointers recommended) | yes, link `detective_instr` | no (dynamic binaries) |
| Measures | on-CPU time, statistically | wall time of marked scopes, exactly | heap calls, bytes, sites |
| Clock | kernel `cpu-clock` event | `std::chrono::steady_clock` | — |
| Sees waiting time | no | yes (included in wall time) | — |
| Output | call tree, flame graph, function table | scope table, edges, marks | counts, histogram, sites |

## Mode A — sampling

### Event

`perf_event_open` with `PERF_TYPE_SOFTWARE / PERF_COUNT_SW_CPU_CLOCK`, period = 10⁹ / f ns
(default f = 999 Hz, allowed 1–10 000 Hz). The software clock works in virtual machines that
expose no hardware PMU (the case on the development machine: hardware `cycles` returns
`ENOENT`). One event per online CPU is opened for the target with `inherit = 1`, so every
thread and child process is sampled; if per-CPU events cannot be opened, a single per-task
event is used and a warning is recorded.

The child is created with `fork()` and blocks on a pipe until the events are attached; the
events use `enable_on_exec`, so sampling starts exactly at `execve` and the profiler's own
setup is never sampled. Kernel samples are excluded (`exclude_kernel`, `exclude_callchain_kernel`).

Each sample records `IP, PID/TID, TIME, PERIOD, CALLCHAIN` plus 8 bytes of the user stack
(`PERF_SAMPLE_STACK_USER`). Samples are read from 128-page ring buffers (per CPU) in the
launcher's poll loop and copied out record by record, handling wrap-around. Lost records
(`PERF_RECORD_LOST`) are counted and reported; a hard cap (2 M samples) bounds memory.
`PERF_RECORD_MMAP` and `PERF_RECORD_COMM` give mappings and thread names; `/proc/<pid>/maps`
is scanned periodically as a supplement.

### Stacks

The kernel walks user frame pointers. This is accurate for code built with
`-fno-omit-frame-pointer` and breaks in code without them. Two specific problems and how they
are handled:

1. **Frameless leaf functions.** GCC omits the frame in small leaf functions even with
   `-mno-omit-leaf-frame-pointer`; the frame-pointer chain then skips the leaf's caller
   (e.g. `main → contains_linear` instead of `main → count_unique_slow → contains_linear`).
   The word at `%rsp` is captured with each sample; if it (a) points into mapped code, (b) is
   preceded by a `call rel32` instruction, and (c) that call **targets the sampled function**
   (or a PLT stub for a cross-module call), it is inserted as the caller. Condition (c) was
   added after the first version accepted a stale return address left in uninitialized stack
   padding (seen as a bogus 0.6 % caller); unverifiable cases are left unchanged. The number
   of recovered frames is reported with every session.
2. **Libraries without frame pointers** (glibc, libstdc++). Self costs stay correct; outer
   frames may be lost. Each stack is classified as *complete* (outermost frame is `_start`,
   `__libc_start_main`, `start_thread`, `clone3`, …), *verifiably incomplete* (outermost frame
   is an ordinary resolved function) or *unknown*. A data-quality finding appears when ≥ 30 %
   of samples are verifiably incomplete.

DWARF unwinding is not implemented (no libunwind/libdw on the build machine). Stacks deeper
than `perf_event_max_stack` (127 here) are truncated and counted.

### Symbols

A self-contained, bounds-checked ELF64 reader loads `.symtab` (or `.dynsym` when stripped)
and `PT_LOAD` segments; addresses are mapped `ip − map_start + pgoff → file offset → vaddr`.
Names are demangled with `abi::__cxa_demangle` and symbol-version suffixes are removed.

- **Stripped modules:** a separate debug file is looked up by GNU build-id under
  `/usr/lib/debug/.build-id/xx/yyyy.debug` (override with `DETECTIVE_DEBUG_DIR`). On Ubuntu,
  `apt install libc6-dbg` makes glibc internals such as `start_thread` and
  `__memcpy_avx512_unaligned_erms` resolvable. Without it they appear as `libc.so.6+0x…`.
- **PLT stubs** are named `[plt stub]` instead of being mis-attributed to `_init`.
- **vDSO:** the image has no file, but it is identical for every process, so its exported
  symbols are read from the profiler's own mapping. Internal vDSO helpers stay as `[vdso]+0x…`.
- **Unresolvable addresses** are always shown as `module+0xOFFSET`, never given a guessed name.
- **Source locations** for the 400 most expensive functions come from `addr2line` (run
  without a shell) when available.

Return addresses of caller frames are moved back one byte before lookup so a call at the very
end of a function is attributed to that function.

### What a sample means

With `cpu-clock` at period *T*, each sample represents ≈ *T* of CPU time spent by the thread
that was running. A function's **self** share is the fraction of samples whose leaf frame is
that function; its **inclusive** share counts samples where it appears anywhere on the stack
(once per sample, so recursion is not double counted). These are statistical estimates; the
UI and findings show a ±2 standard-error band, `p ± 2·sqrt(p(1−p)/n)`. On the development
machine, `samples × period` agreed with `getrusage` CPU time within a few percent (asserted to
within 25 % in the integration tests).

Sampling **does not see time spent blocked** (I/O, locks, sleep). For that, the process-level
`off_cpu` finding uses `getrusage` (CPU time vs. wall time, voluntary context switches).

## Mode B — instrumentation

```cpp
#include "detective/instrument.hpp"
void handle() {
    DETECTIVE_FUNCTION();                 // scope named after the function
    { DETECTIVE_SCOPE("db.query"); ... }  // named scope
    DETECTIVE_MARK("cache.hit");          // counter only
}
```

Each scope is an RAII object; entry and exit read `steady_clock`. Per thread, a scope stack
computes **self** time (total minus instrumented children) and parent→child edges; inclusive
time is added only for the outermost activation, so recursion is not double counted.
Statistics live in thread-local buffers (no lock per event) and are merged into a global
registry once, when the thread exits. At process exit the report is written to
`$DETECTIVE_INSTR_OUT`. Durations for percentiles are kept in a reservoir (4 096 per thread,
65 536 merged); p50/p90 are shown only with ≥ 100 retained durations, p99 with ≥ 1 000. A
re-entrancy flag prevents the runtime from instrumenting itself. Totals are summed across
threads and are wall-clock time, so they include waiting; they can exceed the process wall time
when several threads run concurrently.

## Allocation tracking

`libdetective_alloc.so` is injected with `LD_PRELOAD` and interposes `malloc`, `calloc`,
`realloc`, `free`, `posix_memalign`, `aligned_alloc`, `memalign` and all `operator new/delete`
forms. The real allocator is found with `dlsym(RTLD_NEXT)`; a static bootstrap arena serves
allocations made during that lookup. Counters and a 256-entry call-site cache are
thread-local and flushed to global lock-free tables on eviction and thread exit. The call site
is the immediate caller of the allocation function (`__builtin_return_address(0)`), resolved
with a `/proc/self/maps` snapshot written alongside the counters.

Metrics are deliberately distinct:

| Metric | Meaning |
|---|---|
| `total_allocations` | number of allocation calls |
| `bytes_requested` | total allocation volume: sum of requested sizes over the run |
| `peak_live_bytes` | maximum heap bytes allocated at once, using `malloc_usable_size`; batched per thread, accurate to ±8 KiB per thread |
| `max_rss_kb` | peak resident set size from `getrusage` (code, stacks, allocator overhead included) |

Not covered: statically linked binaries, custom allocators that bypass malloc, and
`mmap`-based allocation outside malloc.

## Clocks

| Quantity | Source |
|---|---|
| Wall-clock duration | `std::chrono::steady_clock` around fork→reap |
| Start/end timestamps | `std::chrono::system_clock` (display only) |
| CPU time, RSS, context switches | `wait4` rusage of the target (includes its threads) |
| Sample timestamps | kernel perf clock |
| Instrumented scope times | `std::chrono::steady_clock` (wall time) |

## Benchmark runs

`warmup_runs` (discarded) and `benchmark_runs` are executed **without** any collector before
the profiled run, so their timings are not perturbed. Summaries report median, MAD, min/max,
p5/p95 and a percentile-bootstrap 95 % CI of the median (2 000 resamples, fixed seed).

## Measured overhead

Development machine: Intel Xeon @ 2.10 GHz, 2 vCPUs (KVM guest, no PMU), 7.8 GiB RAM,
Linux 6.18, GCC 13.3, `-O2 -g -fno-omit-frame-pointer` workloads, RelWithDebInfo engine.
Numbers vary between runs on this shared VM; they are reported with their uncertainty.

**Sampling overhead** — `detective overhead --runs 9`, unprofiled and sampled runs interleaved:

| Workload | Freq | Unprofiled median [95 % CI] | Sampled median [95 % CI] | Change | Mann-Whitney p |
|---|---|---|---|---|---|
| inefficient_loop | 999 Hz | 0.728 s [0.647, 0.740] | 0.756 s [0.662, 0.789] | +3.9 % | 0.22 |
| inefficient_loop | 4999 Hz | 0.795 s [0.759, 0.913] | 0.835 s [0.728, 0.961] | +5.1 % | 0.72 |
| data_processing | 999 Hz | 1.187 s [1.101, 1.292] | 1.097 s [1.045, 1.216] | −7.6 % | 0.19 |
| contention_demo (fine, 4 threads) | 999 Hz | 0.411 s [0.383, 0.422] | 0.426 s [0.394, 0.442] | +3.6 % | 0.19 |

None of these differences is statistically significant: at these rates the sampling overhead
is below the run-to-run noise of this VM (roughly ±10 %). This is not a claim of zero overhead —
each sample costs a kernel interrupt plus a callchain walk; it simply could not be resolved here.

**Allocation tracking overhead** (`allocation_stress`, single runs): naive mode 1.23 s →
1.55 s (≈ +26 %, 34.7 M allocations), reuse mode 0.20 s → 0.27 s. The first implementation used
global atomics per call and cost +160 %; it was reworked to thread-local batching after this
measurement. When sampling and allocation tracking run together, ~10 % of samples land in the
tracker; the analyzer reports this as an *observer overhead* data-quality finding and never as
a hotspot of the user's program.

**Instrumentation overhead** — `build/engine_bench`: 58–75 ns per `DETECTIVE_SCOPE` (varies
between runs). Profiling the benchmark with Performance Detective itself showed ~65 % of that
is the two `clock_gettime` vDSO calls; the reservoir's 64-bit modulo (~10 %) was replaced by a
multiply-shift. Each instrumentation report includes the measured clock cost, and the
`excessive_calls` finding lowers its confidence when marker overhead rivals the measured time.

**Engine throughput and memory** — `build/engine_bench 1000000`:

| Step | Result |
|---|---|
| Aggregation with real symbolization (1 M samples, 756 k distinct stacks, worst case) | 1.3–1.6 s (≈ 0.7 M samples/s) |
| Call tree, 2.8 M nodes (3 000-function program, 350 k distinct stacks) | 0.42 s |
| Flame-graph JSON for the same profile (pruned to ≥ 0.01 %) | 0.51 s |
| Peak RSS of the benchmark process holding 1 M raw samples | ≈ 600 MiB |

Typical sessions (≈ 1 000 samples, hundreds of distinct stacks) are analyzed in 0.1–0.2 s
including `addr2line`, and `detective record` prints the engine's own peak RSS.
Tree construction originally used a hash map per edge (2.9 s for the same profile); it now
sorts stacks by frame rank and builds each node once from shared prefixes.

**API responsiveness** while a 4 999 Hz session was collected and analyzed: `GET /api/sessions`
median 0.61 ms, p95 1.17 ms, max 5.3 ms (idle: median 0.50 ms, p95 0.72 ms).

**Flame-graph rendering** (headless Chromium 141, 1 440 px): a profile with 82 k distinct
stacks served as a 20 001-node tree; first draw 9 ms (only 1 213 frames are wider than half a
pixel), search re-highlight 2 ms, hit test 0.04 ms, fetch + server-side tree 270 ms.
