# HTTP API reference

Base URL: `http://127.0.0.1:7878` (start with `detective serve`). All responses are JSON
unless noted. The API binds to loopback by default and is **not** intended for untrusted
networks (no authentication).

## Conventions

- Request bodies must be `Content-Type: application/json` (other types → `415`).
- Requests with a non-loopback `Host` header are rejected (`403 forbidden_host`).
- Errors share one shape:

```json
{"error": {"code": "executable_not_allowed",
           "message": "executable is outside the allowed target directories (start the server with --allow-dir)"}}
```

| Status | Codes |
|---|---|
| 400 | `invalid_json`, `invalid_config`, `invalid_param`, `invalid_session` |
| 403 | `executable_not_allowed`, `forbidden_host` |
| 404 | `not_found`, `no_profile`, `no_instrumentation`, `no_memory_data` |
| 409 | `invalid_state`, `no_result` |
| 415 | `unsupported_media_type` |
| 429 | `too_many_sessions` |
| 503 | `queue_full` |
| 500 | `internal_error` |

- Sampling numbers are estimates: `*_samples` are counts, `*_share` fractions of the session's
  samples, `*_seconds_est = samples × period`.

## Health and targets

### `GET /api/health`

```json
{"status": "ok", "version": "0.1.0", "sessions": 11, "queue_pending": 0, "queue_capacity": 16, "workers": 1,
 "allowed_dirs": ["/home/claude/performance-detective/build/examples"],
 "capabilities": {"perf_event_open": true, "software_cpu_clock": true, "hardware_counters": false,
                  "perf_event_paranoid": 2, "max_stack": 127, "allocation_tracking": true,
                  "summary": "perf_event_open: available; software cpu-clock: yes; hardware counters: no (no PMU / not permitted); perf_event_paranoid=2; max_stack=127"}}
```

### `GET /api/targets`

Executables found in the allowed directories: `{"allowed_dirs": [...], "targets": ["/…/examples/inefficient_loop", …]}`

## Sessions

### `POST /api/sessions[?start=1]`

Creates a session (`201`). With `start=1` it is queued immediately.

| Field | Type | Default | Constraints |
|---|---|---|---|
| `executable` | string | required | inside an allowed dir; regular, executable, not setuid/setgid |
| `args` | string[] | `[]` | ≤ 256, each ≤ 4096 bytes, no NUL; passed verbatim (no shell) |
| `env` | object | `{}` | ≤ 128 `NAME: value` pairs, names `[A-Za-z0-9_]`; `LD_PRELOAD`/`LD_LIBRARY_PATH`/`LD_AUDIT` refused |
| `working_dir` | string | inherit | must exist |
| `mode` | `sampling` \| `instrumentation` \| `none` | `sampling` | |
| `frequency_hz` | int | 999 | 1–10 000 |
| `timeout_ms` | int | 120 000 | 100–3 600 000 |
| `track_allocations` | bool | false | needs `libdetective_alloc.so` |
| `warmup_runs` | int | 0 | 0–20 (unprofiled, discarded) |
| `benchmark_runs` | int | 0 | 0–100 (unprofiled, timed) |
| `label` | string | `""` | ≤ 200 |

Unknown fields are rejected.

```bash
curl -s -X POST 'localhost:7878/api/sessions' -H 'Content-Type: application/json' \
  -d '{"executable":"/…/build/examples/recursive_workload","args":["--n","32"],"label":"docs example"}'
```
```json
{"id": "s-b536bbd23227", "state": "CREATED", "message": "", "created_ms": 1791754839752, "pid": -1,
 "demo": false, "imported": false,
 "config": {"executable": "/…/build/examples/recursive_workload", "args": ["--n", "32"], "env": {},
            "working_dir": "", "mode": "sampling", "frequency_hz": 999, "timeout_ms": 120000,
            "track_allocations": false, "warmup_runs": 0, "benchmark_runs": 0, "label": "docs example"}}
```

### `GET /api/sessions`

`{"sessions": [ … ]}` newest first. Each entry is the object above plus, once results exist,
`summary`, `process` (without stdout/stderr) and `top_findings` (id, title, severity, score).

### `GET /api/sessions/{id}`

One session in the same shape. `state` is one of `CREATED QUEUED STARTING RUNNING STOPPING
ANALYZING COMPLETED FAILED CANCELLED`; `message` explains failures
(e.g. `"target terminated by signal 11; partial results kept"`).

### `POST /api/sessions/{id}/start` → `202`

Queues a `CREATED` session. `409 invalid_state` otherwise; `503 queue_full` when the bounded
queue is full (the session is then marked FAILED).

### `POST /api/sessions/{id}/stop` → `202`

Requests cancellation. Queued sessions become `CANCELLED` immediately; running targets get
SIGTERM, then SIGKILL after 300 ms; collected data is analyzed and kept. `409` if already
finished.

### `DELETE /api/sessions/{id}` → `{"deleted": "<id>"}`

Only for terminal (or never-started) sessions; also removes the persisted file.

## Results

All of these return `409 no_result` before a session has results.

### `GET /api/sessions/{id}/summary`

```json
{"wall_seconds": 0.889, "cpu_seconds": 0.872, "avg_cores_busy": 0.98, "max_rss_kb": 4004, "exit_code": 0, "findings": 1,
 "collection": {"sampling_requested": true, "sampling_collected": true, "sampling_error": "", "event": "cpu-clock",
                "period_ns": 1001001, "frequency_hz": 999, "raw_samples": 854, "lost_records": 0,
                "warnings": ["852 samples had a frameless leaf; its caller was recovered …"],
                "instrumentation_error": "", "allocation_error": "", "analysis_seconds": 0.12,
                "modules": [{"path": "/usr/lib/x86_64-linux-gnu/libc.so.6", "loaded": true, "has_symtab": true,
                             "has_debug_info": true, "error": "", "debug_file": "/usr/lib/debug/.build-id/a4/…debug"}]},
 "sampling": {"total_samples": 854, "estimated_cpu_seconds": 0.855, "functions": 9, "distinct_stacks": 6,
              "threads": 1, "max_stack_depth": 7, "complete_stack_fraction": 1.0, "unresolved_frame_fraction": 0.0,
              "note": "Sample shares estimate on-CPU time; they do not include time spent blocked."},
 "top_functions": [{"id": "194215c211748b94", "name": "contains_linear(…)", "module": "inefficient_loop",
                    "self_share": 0.998, "total_share": 0.998}],
 "benchmark": {"warmup_runs": 1, "failed_runs": 0, "wall_seconds": [0.82, …], "cpu_seconds": [...],
               "wall": {"n": 5, "median": 0.822, "mad": 0.02, "ci95_low": 0.74, "ci95_high": 0.85, …}, "cpu": {…}},
 "process": {"pid": 3671, "started": true, "exited_normally": true, "exit_code": 0, "term_signal": 0,
             "timed_out": false, "cancelled": false, "start_unix_ms": …, "end_unix_ms": …, "wall_seconds": 0.889,
             "user_cpu_seconds": 0.872, "sys_cpu_seconds": 0.0, "max_rss_kb": 4004,
             "voluntary_ctx_switches": 2, "involuntary_ctx_switches": 15, "stdout_truncated": false, "stderr_truncated": false},
 "threads": [{"tid": 3671, "name": "inefficient_loo", "samples": 854, "share": 1.0, "cpu_seconds_est": 0.855}],
 "modules": [{"name": "inefficient_loop", "self_samples": 853, "total_samples": 854, "self_share": 0.999, "total_share": 1.0}]}
```

### `GET /api/sessions/{id}/output`

`{"stdout": "...", "stderr": "...", "stdout_truncated": false, "stderr_truncated": false}` (1 MiB cap per stream).

### `GET /api/sessions/{id}/functions?sort=self|total|name&q=&limit=&tid=`

`limit` 1–5000 (default 200); `q` matches function or module (case-insensitive); `tid`
restricts to one thread (shares then relative to that thread's samples).

```json
{"total_samples": 854, "matched": 6,
 "units": "samples; *_share = fraction of samples; *_seconds_est = samples x period (estimate)",
 "functions": [{"id": "194215c211748b94",
                "name": "contains_linear(std::vector<unsigned int, std::allocator<unsigned int> > const&, unsigned int)",
                "module": "inefficient_loop", "source_file": "/…/examples/inefficient_loop.cpp", "source_line": 28,
                "resolved": true, "self_samples": 852, "total_samples": 852, "self_share": 0.9977, "total_share": 0.9977,
                "self_seconds_est": 0.853, "total_seconds_est": 0.853, "callers": 1, "callees": 0, "threads": 1,
                "max_recursion_depth": 1}]}
```

### `GET /api/sessions/{id}/functions/{functionId}`

Function detail: shares, `category`, `callers` / `callees` (each with `samples` and `share`),
per-thread samples and `related_findings` (full finding objects that reference it).

### `GET /api/sessions/{id}/flamegraph?view=inverted&tid=&module=&min_fraction=`

Nested tree for the flame graph. `view=inverted` roots the tree at leaf functions (self-cost
view: hot functions → their callers). `min_fraction` (default 0.0001, max 0.5) drops subtrees
below that share of samples; at most 20 000 nodes are emitted, widest first.

```json
{"total": 12, "nodes": 31, "pruned": 0, "inverted": false, "min_fraction": 0.0001,
 "root": {"name": "all", "id": "", "module": "", "value": 12, "self": 0,
          "children": [{"name": "_start", "id": "…", "module": "recursive_workload", "value": 12, "self": 0, "children": [ … ]}]}}
```

### `GET /api/sessions/{id}/findings`

```json
{"scoring": "score = 100 × impact × evidence_weight (high 1.0, medium 0.7, low 0.4)",
 "findings": [{"id": "cpu_hotspot:194215c211748b94", "kind": "cpu_hotspot",
               "title": "CPU hotspot: contains_linear(…)", "severity": "high", "confidence": "high",
               "score": 99.8, "impact": 0.998, "source": "sampling",
               "facts": ["852 of 854 samples (99.8% ± 0.3%) were taken while executing `contains_linear(…)` itself (self cost).", "…"],
               "interpretation": "The instructions of this function itself account for …",
               "next_actions": ["Inspect the source of …", "…"], "remediation": "…",
               "measurements": [{"name": "self_share", "value": 0.998, "unit": "fraction"}, …],
               "affected_functions": ["194215c211748b94"], "affected_names": ["contains_linear(…)"],
               "limitations": ["Sampling estimate: …", "cpu-clock sampling only observes on-CPU time; …"],
               "recommendations": [{"title": "Replace the linear search with an indexed lookup", "observed": "…",
                                    "why_it_matters": "…", "change_to_investigate": "…", "expected_benefit": "…",
                                    "tradeoffs": "…", "how_to_validate": "…", "would_disprove": "…"}]}]}
```

### `GET /api/sessions/{id}/instrumentation`

Only for `mode: instrumentation` sessions whose target is linked with `detective_instr`
(`404 no_instrumentation` otherwise, with the reason):
`{"clock": "steady_clock (wall time)", "threads": 2, "clock_overhead_ns": 43.0,
"scopes": [{"name": "db.query", "count": 1000, "total_ns": …, "self_ns": …, "min_ns": …, "max_ns": …,
"mean_ns": …, "p50_ns": …, "p90_ns": …, "p99_ns": …, "percentiles_valid": true, "retained_durations": 1000,
"threads": {"4710": 500, …}}], "edges": [{"parent": "request.handle", "child": "db.query", "count": 1000, "total_ns": …}],
"marks": {"cache.hit": 19000, "cache.miss": 1000}}`. `p99_ns` is `-1` when fewer than 1 000 durations were retained.

### `GET /api/sessions/{id}/memory`

Only when `track_allocations` was enabled:
`{"total_allocations": 34756002, "malloc_calls": 2, "new_calls": 34756000, "calloc_calls": 0, "realloc_calls": 0,
"memalign_calls": 0, "free_calls": 34756000, "bytes_requested": 1329070000, "peak_live_bytes": 77824,
"peak_error_bytes_per_thread": 8192, "live_bytes_at_exit": …, "size_histogram": [0,0,0,0,31956000,…],
"sites": [{"function": "tokenize_naive(…)", "module": "allocation_stress", "address": …, "count": 26756000, "bytes": …}],
"sites_overflowed": 0, "max_rss_kb": 8960, "scope_note": "…", "definitions": {…}}`.
`size_histogram[i]` counts requests with size in `[2^i, 2^(i+1))`.

### `GET /api/sessions/{id}/export?format=json|csv|svg`

- `json` — the full session including aggregated stacks; re-importable with `/api/import` and
  usable with `detective report` / `detective compare`.
- `csv` — the function table (formula-like cells are prefixed with `'`).
- `svg` — standalone flame graph with tooltips.

## Comparison and import

### `GET /api/compare?baseline={id}&candidate={id}`

```json
{"metrics": [{"metric": "wall_seconds", "n_baseline": 5, "n_candidate": 5,
              "baseline_median": 0.952, "candidate_median": 0.410, "baseline_ci": [0.92, 1.06], "candidate_ci": [0.37, 0.42],
              "absolute_change": -0.542, "relative_change": -0.569, "p_value": 0.0122, "test_valid": true,
              "verdict": "improvement", "explanation": "Median changed by -56.9% (Mann-Whitney p=0.0122 < 0.05)."},
             {"metric": "cpu_seconds", …}],
 "functions": [{"id": "…", "name": "compute_chunk(unsigned long)", "module": "contention_demo",
                "baseline_self_share": 0.975, "candidate_self_share": 0.996, "baseline_total_share": 0.985,
                "candidate_total_share": 0.997, "delta_self_share": 0.021, "p_value": 0.0004, "significant": true}],
 "baseline_samples": 845, "candidate_samples": 808,
 "notes": ["Benchmark timings come from unprofiled runs; …"], "baseline": {…}, "candidate": {…}}
```

`verdict` ∈ `regression | improvement | no_significant_change | inconclusive` (see
[optimization-guide.md](optimization-guide.md#comparing-runs)).

### `POST /api/import[?label=…]`

Body: a JSON export (`{"config": …, "result": …}` or just the result). Creates a `COMPLETED`
session marked `imported`; findings are recomputed by this engine version. Stack frames that
reference unknown functions are dropped. Max body 64 MiB.

## Static files

`GET /` serves the dashboard from `frontend/` (`--frontend DIR` to change).
