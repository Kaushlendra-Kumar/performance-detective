// Optimization recommendation engine: maps evidence patterns to targeted advice.
// Advice is only attached to findings that carry evidence; there are no generic
// per-function tips, and no promised speedups (benefits are qualitative).
#include "detective/bottleneck_detector.hpp"

#include <algorithm>
#include <cctype>

namespace detective {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::string kValidate =
    "Re-run the same workload with the same arguments (ideally with benchmark runs) and compare the two "
    "sessions; accept the change only if the comparison reports a significant improvement.";

Recommendation rec_algorithmic(const Finding& f) {
    return {"Reduce algorithmic work in the hot function",
            f.facts.empty() ? "" : f.facts.front(),
            "When one function's own instructions dominate, the cost usually scales with input size inside a loop. "
            "An O(n²) pattern (e.g. a linear search inside a loop) becomes dominant quickly as n grows.",
            "Look for nested loops, linear searches over growing containers and recomputation of values that do not "
            "change. Replace linear membership tests with a hash set or sorted data + binary search; hoist invariant "
            "work out of loops; consider vectorization-friendly data layouts (contiguous arrays, no aliasing).",
            "Potentially large when the complexity class changes (e.g. O(n²) → O(n log n)); modest for constant-factor tweaks.",
            "Hash/sorted structures use more memory and may change iteration order; added code complexity.",
            kValidate + " Also verify outputs are identical.",
            "If the function's self share stays the same after reducing its input size, the cost is per-call "
            "overhead rather than algorithmic growth."};
}

Recommendation rec_by_category(FuncCategory c, const Finding& f) {
    std::string obs = f.facts.empty() ? "" : f.facts.front();
    switch (c) {
        case FuncCategory::Allocator:
            return {"Reduce heap allocations in hot loops", obs,
                    "Each allocation/free pair costs tens to hundreds of nanoseconds and scatters data across memory.",
                    "Reuse buffers across iterations (clear() keeps capacity), reserve() containers, avoid building "
                    "temporary std::string/std::vector per element, pass by const reference or std::string_view, "
                    "consider object pools or arena allocation for many small objects.",
                    "Moderate to large when allocator time is a big share; also improves cache behaviour.",
                    "Reused buffers retain memory longer (higher steady-state footprint); pools add lifetime complexity.",
                    kValidate + " Enable allocation tracking to confirm the allocation count dropped.",
                    "If the allocation count drops but allocator CPU share does not, the cost is elsewhere (e.g. "
                    "page faults from large allocations)."};
        case FuncCategory::MemCopy:
        case FuncCategory::StringOps:
            return {"Avoid unnecessary copies and string temporaries", obs,
                    "Copying data costs memory bandwidth; string concatenation creates temporaries and allocations.",
                    "Pass large objects by reference, use std::move for transfers, use std::string_view for "
                    "read-only slices, append in place (+=, reserve) instead of `a = a + b`, avoid repeated "
                    "conversions between string types.",
                    "Moderate; depends on string sizes and frequency.",
                    "string_view must not outlive its source buffer; moves leave sources in a valid-but-unspecified state.",
                    kValidate,
                    "If copies are of small, SSO-sized strings, the time may be call overhead rather than copying."};
        case FuncCategory::HashTable:
            return {"Make hash-table operations cheaper", obs,
                    "Hash maps cost a hash computation plus a probe per operation; rehashing during growth copies all nodes.",
                    "reserve() the expected size, use cheaper keys (integers or interned ids instead of long strings), "
                    "look up once and keep the iterator instead of find-then-insert, check hash quality (many collisions "
                    "show up as long probe chains), consider a flat/open-addressing map.",
                    "Moderate.",
                    "reserve() trades memory for speed; changing the map type adds a dependency.",
                    kValidate,
                    "If lookups are few but each is slow, the key type (hashing long strings) is the issue, not the table."};
        case FuncCategory::OrderedMap:
            return {"Reconsider the ordered associative container", obs,
                    "std::map/std::set do O(log n) pointer-chasing comparisons per lookup; with string keys each "
                    "comparison is itself a string compare.",
                    "If ordering is not required, try std::unordered_map with reserve(); otherwise use a sorted "
                    "std::vector with binary search for read-mostly data, or cheaper keys.",
                    "Moderate to large for lookup-heavy code.",
                    "Unordered containers lose sorted iteration; sorted vectors make inserts O(n).",
                    kValidate,
                    "If the map is small (a handful of entries), the cost is in key construction, not tree traversal."};
        case FuncCategory::IoStream:
            return {"Replace stream-based parsing on the hot path", obs,
                    "iostreams carry locale handling, virtual calls and per-call setup; std::stod/strtod are "
                    "locale-aware and general.",
                    "Parse with std::string_view slicing and std::from_chars (or hand-written integer parsing), "
                    "construct streams once instead of per line, or read whole buffers.",
                    "Often large for text-heavy workloads.",
                    "Hand-written parsers need careful validation of malformed input.",
                    kValidate + " Diff the outputs to ensure parsing is equivalent.",
                    "If parsing time is small relative to I/O wait (low CPU utilization), the bottleneck is I/O."};
        case FuncCategory::Synchronization:
            return {"Reduce lock scope and contention", obs,
                    "Contended locks serialize threads; time blocked on a lock does not even show up as CPU samples.",
                    "Move computation outside the critical section, shard data per thread and merge at the end, "
                    "use atomics for simple counters, or reduce how often threads must coordinate.",
                    "Can be large on multi-core machines when threads currently serialize.",
                    "Finer-grained locking increases the risk of races and deadlocks; validate with ThreadSanitizer.",
                    kValidate + " Check that average cores busy (CPU time / wall time) increases.",
                    "If CPU utilization is already close to the thread count, contention is not the limiting factor."};
        case FuncCategory::Sorting:
            return {"Avoid repeated or unnecessary sorting", obs,
                    "Sorting is O(n log n); sorting repeatedly inside a loop multiplies that cost.",
                    "Sort once and maintain order incrementally, use partial_sort/nth_element when only the top-k "
                    "is needed, or keep data in a structure that stays ordered.",
                    "Moderate.", "Incremental structures are more complex.", kValidate,
                    "If each sort is of a small input, call overhead dominates instead."};
        case FuncCategory::Syscall:
            return {"Batch system calls", obs,
                    "Each system call crosses into the kernel (hundreds of ns to µs).",
                    "Buffer I/O (larger reads/writes), avoid per-item flushes (std::endl), batch timer/clock calls.",
                    "Moderate.", "Larger buffers delay visibility of output.", kValidate,
                    "If system time is low in the process summary, syscalls are not the bottleneck."};
        default:
            return rec_algorithmic(f);
    }
}

}  // namespace

void attach_recommendations(std::vector<Finding>& findings, const AnalysisInput& in) {
    for (auto& f : findings) {
        f.recommendations.clear();
        const std::string& k = f.kind;
        if (k == "cpu_hotspot" && in.profile) {
            auto idx = f.affected_functions.empty() ? std::nullopt : in.profile->find_function(f.affected_functions[0]);
            if (!idx) continue;
            const auto& fs = in.profile->functions()[*idx];
            FuncCategory c = classify_function(fs.name, fs.module);
            if (c == FuncCategory::Other) {
                f.recommendations.push_back(rec_algorithmic(f));
                std::string n = lower(fs.name);
                if (n.find("contains") != std::string::npos || n.find("find") != std::string::npos ||
                    n.find("search") != std::string::npos || n.find("lookup") != std::string::npos ||
                    n.find("linear") != std::string::npos) {
                    f.recommendations.push_back(
                        {"Replace the linear search with an indexed lookup", f.facts.front(),
                         "The function name suggests a search; a search over a growing container inside a loop is the "
                         "classic O(n²) pattern.",
                         "Maintain an std::unordered_set / sorted vector alongside (or instead of) the container being "
                         "searched, or restructure to sort once and deduplicate.",
                         "Large if the searched container grows with the input.",
                         "Extra memory for the index; must be kept consistent with the container.",
                         kValidate,
                         "If the searched container is small and bounded, an index will not help."});
                }
            } else {
                f.recommendations.push_back(rec_by_category(c, f));
            }
        } else if (k == "hot_path") {
            Recommendation r = rec_algorithmic(f);
            r.title = "Concentrate optimization effort on the dominant path";
            r.why_it_matters = "Amdahl's law: the maximum saving from optimizing code off this path is bounded by "
                               "its small share of samples.";
            f.recommendations.push_back(r);
        } else if (k == "recursion") {
            f.recommendations.push_back(
                {"Eliminate redundant recursive work", f.facts.front(),
                 "Naive recursion that re-solves overlapping sub-problems has exponential cost; deep recursion also "
                 "risks stack overflow.",
                 "Memoize results (cache by argument), convert to bottom-up dynamic programming, or use an explicit "
                 "stack/iteration.",
                 "Potentially very large when sub-problems overlap (exponential → linear).",
                 "Memoization needs memory proportional to the number of distinct sub-problems.",
                 kValidate + " Verify identical results.",
                 "If each recursive call solves a distinct sub-problem (e.g. tree traversal), memoization will not help."});
        } else if (k == "allocator_cpu" || k == "allocation_pressure") {
            f.recommendations.push_back(rec_by_category(FuncCategory::Allocator, f));
        } else if (k == "memcopy_cpu") {
            f.recommendations.push_back(rec_by_category(FuncCategory::MemCopy, f));
        } else if (k == "string_cpu") {
            f.recommendations.push_back(rec_by_category(FuncCategory::StringOps, f));
        } else if (k == "hash_cpu") {
            f.recommendations.push_back(rec_by_category(FuncCategory::HashTable, f));
        } else if (k == "ordered_map_cpu") {
            f.recommendations.push_back(rec_by_category(FuncCategory::OrderedMap, f));
        } else if (k == "iostream_cpu") {
            f.recommendations.push_back(rec_by_category(FuncCategory::IoStream, f));
        } else if (k == "sync_cpu" || (k == "off_cpu" && in.sampled_threads > 1)) {
            f.recommendations.push_back(rec_by_category(FuncCategory::Synchronization, f));
        } else if (k == "syscall_cpu" || k == "kernel_time") {
            f.recommendations.push_back(rec_by_category(FuncCategory::Syscall, f));
        } else if (k == "excessive_calls") {
            f.recommendations.push_back(
                {"Batch work to reduce per-call overhead", f.facts.front(),
                 "When a function is called millions of times with tiny bodies, call/setup overhead is a meaningful fraction.",
                 "Process arrays of inputs per call, make the function inlinable (header, no virtual dispatch), or "
                 "hoist checks to the caller.",
                 "Small to moderate.", "Batch APIs change call sites.", kValidate,
                 "If removing the instrumentation marker alone removes most of the cost, the overhead was the marker."});
        }
        // data_quality and off_cpu (single-threaded) carry next actions only.
    }
}

}  // namespace detective
