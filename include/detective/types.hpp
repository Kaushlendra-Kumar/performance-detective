// Core data types shared across the Performance Detective engine.
//
// Clock semantics (see docs/profiling-methods.md):
//   * wall-clock durations use std::chrono::steady_clock (monotonic, never jumps)
//   * sample timestamps come from the kernel perf clock (CLOCK_MONOTONIC based)
//   * CPU times come from getrusage/wait4 (user + system time charged to the process)
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace detective {

// One raw call-stack sample as delivered by the collector.
// ips[0] is the leaf (the instruction executing when the sample fired);
// ips[1..] are return addresses of the callers, outermost last.
struct RawSample {
    uint64_t time_ns = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t weight_ns = 0;  // sampling period attributed to this sample (estimate)
    std::vector<uint64_t> ips;
    // Word at the user stack pointer when the sample fired (PERF_SAMPLE_STACK_USER).
    // Used to recover the caller of frameless leaf functions; 0 if unavailable.
    uint64_t stack_top = 0;
};

// A memory mapping observed in the target (PERF_RECORD_MMAP or /proc/<pid>/maps).
struct MappingRecord {
    uint32_t pid = 0;
    uint64_t start = 0;
    uint64_t length = 0;
    uint64_t pgoff = 0;
    std::string path;
    bool executable = true;
};

// Everything the sampler collected, before symbolization.
struct RawProfile {
    std::vector<RawSample> samples;
    std::vector<MappingRecord> mappings;
    std::map<uint32_t, std::string> thread_names;  // tid -> comm
    uint64_t lost_records = 0;      // samples the kernel dropped (ring buffer full)
    uint64_t sample_period_ns = 0;  // nominal period
    std::string event_name;         // e.g. "cpu-clock"
    bool collected = false;         // false => sampler could not run
    std::string collection_error;   // why not, if !collected
    std::vector<std::string> warnings;
};

// A resolved stack frame.
struct ResolvedFrame {
    std::string function;  // demangled name, or "[unknown]" / "module+0xoff"
    std::string module;    // short module name (basename) or "[unknown]"
    std::string source_file;  // optional
    uint32_t source_line = 0;
    bool resolved = false;
};

struct ProcessResult {
    int pid = -1;
    bool started = false;
    bool exited_normally = false;
    int exit_code = -1;
    int term_signal = 0;
    bool timed_out = false;
    bool cancelled = false;
    std::string start_error;  // exec failure, etc.
    int64_t start_unix_ms = 0;
    int64_t end_unix_ms = 0;
    double wall_seconds = 0;
    double user_cpu_seconds = 0;
    double sys_cpu_seconds = 0;
    long max_rss_kb = 0;
    long voluntary_ctx_switches = 0;
    long involuntary_ctx_switches = 0;
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
};

}  // namespace detective
