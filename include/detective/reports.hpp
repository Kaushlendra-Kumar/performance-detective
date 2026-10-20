// Results of the optional collection modules: instrumentation (Mode B) and allocation tracking.
// These are kept separate from sampling estimates and are never merged into the call tree.
#pragma once

#include "detective/symbol_resolver.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace detective {

// ---------------------------------------------------------------- instrumentation
struct ScopeStats {
    std::string name;
    uint64_t count = 0;
    double total_ns = 0;  // wall-clock (steady_clock) time inside the scope, inclusive
    double self_ns = 0;   // total minus time in instrumented child scopes
    double min_ns = 0, max_ns = 0, mean_ns = 0;
    double p50_ns = 0, p90_ns = 0, p99_ns = 0;
    bool percentiles_valid = false;  // only when enough samples were retained
    uint64_t retained_durations = 0;
    std::map<uint32_t, uint64_t> thread_counts;
};

struct ScopeEdge {
    std::string parent;
    std::string child;
    uint64_t count = 0;
    double total_ns = 0;
};

struct InstrumentationReport {
    std::vector<ScopeStats> scopes;  // sorted by total_ns desc
    std::vector<ScopeEdge> edges;
    std::map<std::string, uint64_t> marks;
    uint64_t threads = 0;
    double clock_overhead_ns = 0;  // measured cost of one steady_clock::now() pair in the target
};

std::optional<InstrumentationReport> load_instrumentation_report(const std::string& path, std::string* error);

// ---------------------------------------------------------------- allocation tracking
struct AllocSite {
    uint64_t address = 0;  // return address in the caller of malloc / operator new
    std::string function;
    std::string module;
    uint64_t count = 0;
    uint64_t bytes = 0;
};

struct MemoryReport {
    uint64_t malloc_calls = 0, calloc_calls = 0, realloc_calls = 0, memalign_calls = 0;
    uint64_t new_calls = 0;  // operator new / new[]
    uint64_t free_calls = 0;
    uint64_t total_allocations = 0;
    uint64_t bytes_requested = 0;   // total allocation volume (sum of requested sizes)
    uint64_t peak_live_bytes = 0;   // peak of live heap (malloc_usable_size accounting)
    uint64_t live_bytes_at_exit = 0;
    uint64_t peak_error_bytes_per_thread = 0;  // batching bound on peak_live_bytes accuracy
    std::vector<uint64_t> size_histogram;  // bucket i: sizes in [2^i, 2^(i+1))
    std::vector<AllocSite> sites;          // sorted by count desc
    uint64_t sites_overflowed = 0;         // allocations whose call site did not fit the table
    std::string scope_note;
};

// Loads the shim's output and resolves call-site addresses using its /proc/self/maps snapshot.
std::optional<MemoryReport> load_memory_report(const std::string& path, std::string* error);

}  // namespace detective
