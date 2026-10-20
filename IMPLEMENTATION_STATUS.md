# Implementation status

Last updated: 2026-10-12. Environment: Ubuntu 24.04 (KVM guest, 2 vCPUs, no PMU), Linux 6.18,
GCC 13.3, CMake 3.28, Node 22, Chromium 141 (headless, for UI checks).

## Completed

| Area | Notes |
|---|---|
| Process manager | fork/execve with argv vector, gate pipe so collectors attach before exec, bounded stdout/stderr capture, env vars, working dir, timeout (SIGTERM → SIGKILL to the process group), cancellation, exec-failure reporting, rusage |
| Sampling profiler | `perf_event_open` software cpu-clock, per-CPU inherited events (fallback: per-task), ring-buffer reader with wrap-around, lost-record accounting, sample cap, MMAP/COMM records + `/proc/maps` scan, 8-byte user-stack capture |
| Unwinding | kernel frame-pointer callchains; frameless-leaf caller recovery verified against the `call rel32` target; stack completeness classification; max-stack truncation warnings |
| Symbols | own ELF64 parser (fuzzed), demangling, version-suffix stripping, PLT stubs, build-id debug files (`/usr/lib/debug`), vDSO exports from the profiler's own mapping, `addr2line` source lines, honest `module+0xOFF` for unknown code |
| Aggregation | address-cached interning, merged stacks, deterministic ordering, stable function ids, self/inclusive, callers/callees, recursion depth, per-thread and per-module views, prefix-sharing call-tree construction, inverted tree, thread/module filters |
| Analysis | 16 rules + 4 data-quality notices, facts/interpretation/next actions/limitations, transparent score, confidence from sampling error, base-name function classification, driver (nearest app caller) attribution, observer-overhead detection |
| Recommendations | evidence-specific advice with qualitative benefit, trade-offs, validation and falsification criteria |
| Benchmark & comparison | warm-up + unprofiled runs, median/MAD/bootstrap CI, Mann-Whitney U with min-runs and min-effect rules, Bonferroni-corrected per-function two-proportion tests |
| Instrumentation (Mode B) | `DETECTIVE_SCOPE/FUNCTION/MARK`, RAII, thread-local stats + edge cache, merge at thread exit, self vs total, recursion-safe totals, reservoir percentiles with minimum counts, clock-overhead measurement, report at exit |
| Allocation tracking | LD_PRELOAD interposer (malloc family + all operator new/delete), thread-local batching, lock-free global tables, call sites + maps snapshot, size histogram, peak live heap (±8 KiB/thread), distinct from RSS |
| HTTP API | all endpoints in docs/api-reference.md, bounded work queue, structured errors, validation, executable allow-list, Host-header + content-type checks, persistence and restore, import/export (JSON/CSV/SVG) |
| Frontend | dashboard, new-session form, session page (overview/findings/benchmark/threads/modules), canvas flame graph (zoom, breadcrumbs, search, thread/module filters, inclusive/inverted, self-threshold highlight, keyboard navigation, tooltips), function table + detail panel, instrumentation and memory views, output, export, comparison page, labelled demo mode |
| CLI | `capabilities`, `record`, `report`, `compare`, `overhead`, `serve`, `demo-data` |
| Docs | README, architecture, profiling methods (with measured overhead), optimization guide (rules + formula), API reference, Linux permissions, workloads, third-party licences |

## Tests run (final code, all passing)

| Configuration | Suite | Result |
|---|---|---|
| RelWithDebInfo | unit_tests (Catch2) | 44 cases, 3 593 assertions ✅ |
| RelWithDebInfo | integration_tests | 27 cases, 758 assertions ✅ |
| RelWithDebInfo | frontend_tests (`node --test`) | 8 tests ✅ |
| Debug + ASan + UBSan | unit, integration, frontend | all ✅ (no sanitizer reports) |
| Debug + TSan | unit + `[api],[process],[session]` integration | all ✅ (no data races) |

`scripts/run_all_tests.sh` reproduces all of the above (≈ 3 minutes on the dev VM).

Integration tests use real executables and real `perf_event_open` sampling: hot function
detection, caller recovery (Workload A), recursion (C), off-CPU contention (D), stripped
binaries, simulated perf unavailability, crashing targets, cancellation, instrumentation mode,
allocation tracking, benchmark comparison, the HTTP API end-to-end (create/start/poll/export/
import/compare/persist/delete), validation errors, DNS-rebinding defence, queue overflow,
concurrent readers and repeated create/delete.

Manual end-to-end checks: all 11 example sessions profiled through the API; every UI page
rendered in headless Chromium with zero console errors; keyboard zoom verified.

## Defects found and fixed while testing

- Frame-pointer chains skipped callers of frameless leaf functions → stack-top recovery.
- First recovery version accepted stale return addresses (bogus 0.6 % caller) → now verified
  against the call target.
- Dangling temporaries when iterating `json.value(...).items()` → instrumentation reports failed to load.
- Functions taking `std::string` parameters were classified as string functions → base-name classification.
- glibc `__GI_` aliases defeated name patterns; `@@GLIBC` version suffixes leaked into names; PLT stubs were shown as `_init`.
- Duplicate hot-path/hotspot findings ranked the less actionable one first → merged.
- Content-type guard ran before the body was read → header-based check.
- Allocation tracker cost +160 % and 54 % of samples → thread-local batching (+26 %, ~10 % of samples, reported as observer overhead).
- Call-tree build 2.9 s / aggregation 0.33 M samples/s on large profiles → prefix-sharing tree (0.42 s) and address cache (0.7 M samples/s).
- UI: tooltip clipping, misleading top-function shares from 7-sample sessions, unreadable comparison rows.
- Two use-after-free bugs in test code caught by ASan (references into temporaries inside `CHECK`).

## Known limitations

- **No hardware counters** on this VM (no PMU); only `cpu-clock`. Cycle/cache-miss events are not implemented.
- **No DWARF unwinding** (libunwind/libdw unavailable); callers inside libraries built without
  frame pointers can be lost. Self costs are unaffected; incomplete stacks are measured and reported.
- **Off-CPU time is not attributed to functions.** Blocking is detected only at process level
  (utilization, context switches). Lock contention is never asserted from samples alone.
- Instrumentation costs ~60–75 ns per scope on this VM (dominated by `clock_gettime`); do not
  instrument functions shorter than a few hundred nanoseconds.
- Allocation tracking does not see statically linked binaries or allocators that bypass malloc;
  call sites are the immediate caller only (no full allocation stacks).
- Sampling overhead could not be resolved from run-to-run noise on this VM (n = 9, |Δ| ≤ 8 %, p ≥ 0.18).
- x86-64 Linux only (callchain fix-up decodes x86 call instructions; ELF64 little-endian).
- The API has no authentication and is single-user by design.
- Profiling sessions run one at a time by default (`--workers 1`) to avoid interference between measurements.

## Next milestone

1. Off-CPU profiling (sched_switch tracepoints or `PERF_COUNT_SW_CONTEXT_SWITCHES` with
   callchains) to attribute blocked time to call stacks and make contention findings direct.
2. Optional DWARF unwinding via `PERF_SAMPLE_REGS_USER` + `PERF_SAMPLE_STACK_USER` and libunwind when available.
3. Hardware events (cycles, cache misses, branch misses) when a PMU is present.
4. Allocation call stacks (frame-pointer walk in the tracker) and per-call-site lifetimes.
