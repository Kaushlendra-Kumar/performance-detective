# Architecture

## Modules

```
include/detective/          src/
  types.hpp                   (shared data types: RawSample, MappingRecord, ProcessResult)
  process_runner.hpp          platform/process_runner.cpp   fork/execve, pipes, timeouts, rusage
  sampler.hpp                 sampling/perf_sampler.cpp     perf_event_open, ring buffers
  symbol_resolver.hpp         symbols/symbol_resolver.cpp   ELF, build-id debug files, vDSO, addr2line
  call_tree.hpp               analysis/call_tree.cpp        Profile: stacks, functions, call trees
  statistics.hpp              analysis/statistics.cpp       summaries, bootstrap, Mann-Whitney, z-test
  flame_graph.hpp             analysis/flame_graph.cpp      layout + standalone SVG export
  bottleneck_detector.hpp     analysis/bottleneck_detector.cpp  rules, classification, scoring
                              analysis/optimizer.cpp        recommendations
  comparison.hpp              analysis/comparison.cpp       baseline vs candidate
  reports.hpp                 analysis/instrumentation_report.cpp, memory_report.cpp
  session.hpp                 core/session.cpp              config validation, state machine
  profiler.hpp                core/profiler.cpp             end-to-end pipeline
  exporter.hpp                export/serialize.cpp          JSON/CSV views, import/export
  api_server.hpp              server/api_server.cpp         work queue, session manager, HTTP
  instrument.hpp              instrument/instrument.cpp     Mode B runtime (libdetective_instr.a)
                              alloc/alloc_shim.cpp          LD_PRELOAD tracker (libdetective_alloc.so)
                              cli/main.cpp                  `detective` CLI
frontend/                     static ES modules, no build step
  src/api.js                  fetch client + DemoApi (static demo file)
  src/flamegraph.js           pure layout + canvas renderer
  src/format.js               formatting helpers
  src/app.js                  router and views
```

`detective_core` (everything except the server, CLI and the two injected libraries) has no
knowledge of HTTP or the frontend. The CLI and the API are two front ends over
`run_profile()`. The injected libraries do not link against the engine.

## Data flow

```
SessionConfig ─validate─► run_profile()
   │  warm-up + benchmark runs (ProcessRunner, no collector) ─► BenchmarkResult
   │  profiled run: ProcessRunner + PerfSampler (ProcessObserver hooks)
   │      fork ─► child blocks on gate pipe ─► sampler opens per-CPU events (enable_on_exec)
   │      ─► gate released ─► execve ─► poll loop drains stdout/stderr + ring buffers
   │      ─► wait4 (rusage) ─► final drain
   ▼
RawProfile (samples, mappings, comm names, lost counts)
   │  Profile::build: ip → function (cached per address), frameless-leaf recovery,
   │  identical stacks merged, sorted deterministically
   ▼
Profile (functions with self/inclusive/callers/callees/threads, StackEntry list)
   │  + InstrumentationReport (from $DETECTIVE_INSTR_OUT)   ─ kept separate
   │  + MemoryReport (from $DETECTIVE_ALLOC_OUT)            ─ kept separate
   ▼
detect_bottlenecks() → attach_recommendations() → SessionResult (immutable, shared_ptr)
   ▼
views on demand: call tree (filters, inverted), flame JSON, function table/detail, CSV, SVG
```

Call trees are not stored; they are rebuilt from the merged stacks per request (fast enough —
see the measured numbers in profiling-methods.md), which keeps thread/module filters and the
inverted view exact.

## Session state machine

```
CREATED ─► QUEUED ─► STARTING ─► RUNNING ─► ANALYZING ─► COMPLETED
                                   │            ▲
                                   └► STOPPING ─┘──► CANCELLED
any non-terminal state ──► FAILED;  CREATED/QUEUED/STARTING ──► CANCELLED
```

`valid_transition()` enforces the table; COMPLETED, FAILED and CANCELLED are terminal.
A crash, timeout or missing sampling capability ends in FAILED with the partial result kept
and the reason in `message`. Stopping a running session ends in CANCELLED with partial data.

## Threading and concurrency

- **HTTP threads** (cpp-httplib's pool) only read immutable `SessionResult` snapshots
  (`shared_ptr<const …>`) or touch `Session` state under its mutex.
- **Profiling jobs** run on a bounded `WorkQueue` (default 1 worker, 16 queued). A full queue
  returns HTTP 503; nothing blocks a request handler for the duration of a run.
- `SessionManager` holds sessions in a map guarded by a mutex; persistence writes are
  atomic (`tmp` + `rename`).
- Cancellation is an `atomic<bool>` checked by the launcher's poll loop (SIGTERM, then SIGKILL
  after 300 ms, to the whole process group).
- The instrumentation runtime uses thread-local buffers merged once per thread; the allocation
  tracker uses thread-local counters plus lock-free global tables.

ThreadSanitizer runs of the unit tests and the API/process/session integration tests report no
races.

## Security model

- Targets are executed with `execve` and an argv vector; no shell is involved anywhere
  (including `addr2line`).
- The server only launches executables whose canonical path (after `realpath`) is inside an
  `--allow-dir`; setuid/setgid files are refused; `LD_PRELOAD`, `LD_LIBRARY_PATH` and `LD_AUDIT`
  cannot be set through the API.
- The API binds to `127.0.0.1` by default, rejects requests whose `Host` header is not a
  loopback name (DNS-rebinding defence) and rejects non-JSON request bodies (blocks
  cross-site form posts). There is no authentication: do not expose it to untrusted networks.
- Session ids are validated (`[a-z0-9-]{1,64}`); exports are generated in memory from session
  data, never from client-supplied paths. CSV export neutralises spreadsheet formulas.
- Output capture is bounded (1 MiB per stream), sample counts are capped, request bodies are
  limited to 64 MiB, and all ELF / JSON parsing is bounds-checked and fuzzed in tests with
  truncated and corrupted inputs.
- Temporary report files are created in the scratch directory and removed after loading; the
  whole child process group is killed when a run ends.
- When the server runs as root it prints a warning; profiled programs inherit its privileges.
