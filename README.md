# Performance Detective 🔍

A locally running C++20 profiler and bottleneck analyzer. It launches a program, samples
where its CPU time goes with Linux `perf_event_open`, aggregates real call stacks into a call
tree, ranks evidence-backed findings, and shows everything in a web dashboard with an
interactive flame graph, function drill-down and run-to-run comparison.

Every number shown for a real session comes from a measurement. Sampling-derived numbers are
labelled as estimates, unavailable data is reported as unavailable, and the only synthetic
data (demo mode) is explicitly labelled and kept separate.

```
┌──────────── detective (C++20) ─────────────┐        ┌────── browser ──────┐
│ ProcessRunner ── fork/execve, pipes, rusage│        │ dashboard           │
│ PerfSampler ──── cpu-clock + callchains    │  HTTP  │ session / findings  │
│ SymbolResolver ─ ELF + build-id debug files│◄──────►│ canvas flame graph  │
│ Profile ──────── stacks → call tree        │  JSON  │ function panel      │
│ Detector + Optimizer ─ findings, advice    │        │ comparison          │
│ ApiServer ────── loopback, bounded queue   │        └─────────────────────┘
└────────────────────────────────────────────┘
  + libdetective_instr.a  (RAII scopes, Mode B)
  + libdetective_alloc.so (LD_PRELOAD allocation tracker)
```

## What works (verified on this machine)

| Capability | Status |
|---|---|
| Launch any executable (argv vector, no shell), capture stdout/stderr, exit status, timeouts, cancellation | ✅ |
| Sampling profiler: `perf_event_open` software `cpu-clock`, per-CPU inherited events, frame-pointer callchains | ✅ |
| Caller recovery for frameless leaf functions (stack-top word, verified against the call instruction) | ✅ |
| Symbols: own ELF parser, demangling, PLT stubs, vDSO, separate debug files via build-id, source lines via `addr2line` | ✅ |
| Call tree with self/inclusive cost, recursion depth, per-thread / per-module aggregation, stable function ids | ✅ |
| Rule-based findings (facts / interpretation / next actions / limitations) with a transparent score | ✅ |
| Optimization recommendations tied to evidence (no promised speedups) | ✅ |
| Benchmark runs (warm-up + repeated unprofiled runs), bootstrap CIs, Mann-Whitney comparison | ✅ |
| Instrumentation mode (`DETECTIVE_SCOPE`, `DETECTIVE_FUNCTION`, `DETECTIVE_MARK`) | ✅ |
| Allocation tracking (`LD_PRELOAD`): counts, volume, peak live heap, size histogram, call sites | ✅ |
| Local HTTP API, persistence, import/export (JSON, CSV, SVG) | ✅ |
| Web dashboard: sessions, findings, flame graph (inclusive + inverted), functions, memory, instrumentation, compare, demo mode | ✅ |
| Hardware counters (cycles, cache misses) | ❌ not available in this VM (no PMU) — the tool uses `cpu-clock` |
| Off-CPU (blocked-time) attribution per function | ❌ not implemented — reported indirectly via utilization/context switches |
| DWARF unwinding | ❌ no libunwind/libdw here — frame pointers + leaf recovery instead |

Details and limitations: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md).

## Requirements

- Linux x86-64 with `perf_event_open` (kernel ≥ 4.x). See [docs/linux-permissions.md](docs/linux-permissions.md).
- GCC ≥ 11 or Clang ≥ 14 (C++20), CMake ≥ 3.20.
- Optional: `addr2line` (binutils) for source lines; `libc6-dbg` for symbols inside glibc; Node ≥ 18 to run frontend tests.
- No network access is needed at build or run time — dependencies are vendored in `third_party/` (see [THIRD_PARTY.md](THIRD_PARTY.md)).

## Build and test

```bash
cmake --preset default            # or: cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure      # unit + integration + frontend tests

# Sanitizer builds
cmake --preset asan && cmake --build build-asan -j && ctest --test-dir build-asan
cmake --preset tsan && cmake --build build-tsan -j && ctest --test-dir build-tsan -R unit
```

Or run everything at once: `scripts/run_all_tests.sh`.

## Quick start (CLI)

```bash
./build/detective capabilities
./build/detective record -- build/examples/inefficient_loop --mode slow
./build/detective record --runs 5 --warmup 1 --out slow.json -- build/examples/inefficient_loop --mode slow
./build/detective record --runs 5 --warmup 1 --out fast.json -- build/examples/inefficient_loop --mode fast
./build/detective compare slow.json fast.json
./build/detective record --svg flame.svg --csv functions.csv -- build/examples/data_processing
./build/detective record --alloc --mode none -- build/examples/allocation_stress
./build/detective record --mode instrumentation -- build/examples/instrumented_demo
./build/detective overhead --runs 9 -- build/examples/inefficient_loop   # measure profiler overhead
```

## Quick start (dashboard)

```bash
./build/detective serve --port 7878            # targets allowed from build/examples by default
# open http://127.0.0.1:7878
```

`scripts/run_demo.sh` builds the project, starts the server, and profiles every example
workload through the API so the dashboard is populated. To explore the UI without profiling
anything, open **Demo** — it loads `frontend/demo/demo-session.json`, a reproducible synthetic
session generated by `detective demo-data` and labelled as such on every screen.

Server options: `--allow-dir DIR` (repeatable; executables must live under one of these),
`--data-dir DIR` (session persistence, default `./detective-data`), `--no-persist`,
`--host` (default `127.0.0.1`), `--workers N`.

> The API launches programs. It binds to loopback, rejects non-localhost `Host` headers and
> non-JSON bodies, and only runs executables from allowed directories — but it is **not**
> designed to be exposed to an untrusted network.

## Example workloads

Five programs with known bottlenecks live in `examples/` (inefficient algorithm, allocation
pressure, recursion, mutex contention, data processing) plus an instrumented pipeline.
[examples/README.md](examples/README.md) documents the expected evidence for each and how to
verify it independently. They are integration-test fixtures; nothing in the engine
special-cases them.

## Documentation

- [docs/architecture.md](docs/architecture.md) — modules, data flow, threading, security
- [docs/profiling-methods.md](docs/profiling-methods.md) — sampling, unwinding, clocks, instrumentation, allocation tracking, **measured overhead**
- [docs/optimization-guide.md](docs/optimization-guide.md) — finding rules and the ranking formula
- [docs/api-reference.md](docs/api-reference.md) — every endpoint with examples
- [docs/linux-permissions.md](docs/linux-permissions.md) — `perf_event_paranoid`, capabilities, containers

## License

MIT — see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY.md](THIRD_PARTY.md).
