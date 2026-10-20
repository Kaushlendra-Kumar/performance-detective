# Findings, ranking and recommendations

The analysis engine is deterministic and rule-based (`src/analysis/bottleneck_detector.cpp`,
`src/analysis/optimizer.cpp`). It does not use an LLM and does not claim to detect every
performance problem. Every finding has three separate parts:

1. **Observed facts** — measurements, with units and uncertainty.
2. **Interpretation** — a labelled heuristic explanation of what the facts may mean.
3. **Next actions** — what to look at or measure next.

plus `measurements`, `affected_functions`, `limitations` and evidence-specific
`recommendations`.

## Ranking formula

```
score = 100 × impact × evidence_weight
impact          = fraction of measured activity involved (0..1), defined per rule below
evidence_weight = 1.0 (high confidence) | 0.7 (medium) | 0.4 (low)
```

Findings are sorted by score (ties by id). Data-quality findings have impact 0 and sort last,
but are always shown because they qualify every other number.

### Confidence for sampling findings

For a share *p* over *n* samples with standard error `se = sqrt(p(1−p)/n)` and rule threshold *t*:

- **high** if `p − 2·se ≥ t` and `n ≥ 1000`, or `p − 2·se ≥ 2t` and `n ≥ 200`
- **medium** if `p − 2·se ≥ t/2`
- **low** otherwise

Confidence drops one level when the affected function has no symbol. No sampling finding is
produced at all below 50 samples (an `insufficient_samples` notice is emitted instead).

## Rules

| Kind | Source | Fires when | Impact |
|---|---|---|---|
| `cpu_hotspot` | sampling | a function's **self** share ≥ 10 % (max 5) | self share |
| `hot_path` | sampling | following the heaviest child from the root, ≥ 2 frames each holding ≥ 50 % of samples, ending in a frame that is not an entry point/`main` and not already a hotspot (otherwise the path is added to that hotspot as a fact) | share of the last frame |
| `recursion` | sampling | inclusive share ≥ 20 % and the function appears ≥ 16 times in one stack | inclusive share |
| `allocator_cpu` | sampling | leaf frames in malloc/free/operator new… ≥ 10 % | category share |
| `memcopy_cpu` | sampling | memcpy/memmove/memset… ≥ 10 % | category share |
| `string_cpu` | sampling | std::string / str* functions ≥ 10 % | category share |
| `hash_cpu` | sampling | `_Hashtable` / unordered containers ≥ 10 % | category share |
| `ordered_map_cpu` | sampling | `_Rb_tree` (std::map/set) ≥ 10 % | category share |
| `iostream_cpu` | sampling | iostreams, getline, strtod, printf internals ≥ 10 % | category share |
| `sync_cpu` | sampling | pthread mutex/cond/futex functions ≥ 5 % | category share |
| `syscall_cpu` | sampling | read/write/poll/… wrappers ≥ 10 % | category share |
| `off_cpu` | process (rusage) | wall ≥ 0.2 s and `1 − cores_busy / min(threads, CPUs) ≥ 0.35` | that idle fraction |
| `kernel_time` | process (rusage) | CPU ≥ 0.1 s and system time ≥ 30 % of CPU | system share |
| `instr_hot_scope` | instrumentation | a scope's self time ≥ 25 % of all instrumented self time | that share |
| `excessive_calls` | instrumentation | ≥ 100 000 calls, mean < 2 µs, ≥ 5 % of instrumented time | time share |
| `allocation_pressure` | allocation tracker | ≥ 100 000 allocations or ≥ 100 000 / s | allocator CPU share if sampled, else `min(1, rate / 10⁷)` (stated in the finding) |
| `insufficient_samples`, `incomplete_stacks`, `missing_symbols`, `observer_overhead` | data quality | < 50 samples; ≥ 30 % verifiably incomplete stacks; ≥ 25 % unresolved frames; ≥ 2 % of samples inside the injected tracker | 0 |

Category rules classify functions by their **base name** — the demangled signature with the
parameter list and return type removed — so `tokenize(const std::string&)` is not mistaken for
a string function. glibc's internal aliases (`__GI___libc_malloc`, `__memcpy_avx512_…`) are
normalised first. For category findings the engine also reports the **nearest application
callers** (the first resolved, non-library, non-category frame above each such sample), which
is usually where a change has to be made.

### What the rules deliberately do not do

- No function is called a bottleneck because of a large *inclusive* or *wall-clock* time
  alone — the hotspot rule uses self samples, which are on-CPU by construction.
- Lock contention is never asserted from sampling alone. `sync_cpu` only states CPU time
  inside locking functions; blocking is inferred at process level (`off_cpu`) with medium or
  low confidence and the limitation spelled out.
- Allocation findings appear only when the allocation tracker ran, or as a CPU-share
  statement (`allocator_cpu`) from samples that actually landed in allocator code.
- Busy loops and I/O bottlenecks are not separate rules: cpu-clock samples cannot distinguish
  a spin loop from useful work, and per-function I/O wait is not collected.
- Profiler-injected code (`libdetective_*`) is never reported as a hotspot of the program.

## Recommendations

`attach_recommendations()` maps each finding to advice using the finding kind and, for
hotspots, the function's category and name:

| Evidence | Recommendation |
|---|---|
| hotspot in application code | reduce algorithmic work (nested loops, linear searches, invariant work, vectorisation-friendly layout) |
| …and the name suggests a search (`find`, `contains`, `lookup`, `search`) | replace the linear search with an indexed lookup |
| allocator CPU / allocation pressure | reuse buffers, `reserve()`, avoid per-element temporaries, pools/arenas |
| memcpy / string | avoid copies, `std::move`, `std::string_view`, in-place appends |
| hash table | `reserve()`, cheaper keys, single lookup, check hash quality |
| ordered map | `unordered_map` if order is not needed, sorted vector for read-mostly data |
| iostream / parsing | `string_view` slicing + `from_chars`, construct streams once |
| sync / multi-threaded off-CPU | shrink critical sections, shard and merge, atomics |
| syscalls / kernel time | batch I/O, avoid per-item flushes |
| recursion | memoisation / dynamic programming / iteration |
| excessive calls | batch inputs, make the function inlinable |

Each recommendation states: what was observed, why it may matter, the change to investigate,
the expected benefit **qualitatively** (no percentages without a measured comparison),
trade-offs, how to validate (re-run and compare sessions with ≥ 5 benchmark runs), and what
evidence would disprove the hypothesis. If no rule fires, the UI and CLI say that no credible
bottleneck was identified rather than producing generic tips.

## Comparing runs

`compare_results()` (UI: *Compare*, CLI: `detective compare a.json b.json`):

- **Timing** uses benchmark runs (unprofiled). A verdict of `regression` / `improvement`
  requires ≥ 5 runs per side, a two-sided Mann-Whitney U test with p < 0.05, *and* a relative
  change of at least 2 %. Otherwise the result is `no_significant_change` or `inconclusive`
  (e.g. a single run per side — even an 86 % difference is reported as inconclusive).
- **Functions** are matched by stable id (FNV-1a of `module!name`). Self-share changes are
  tested with a two-proportion z-test, Bonferroni-corrected across functions, and must exceed
  1 percentage point to be marked significant. Shares are relative to each run's own samples.
