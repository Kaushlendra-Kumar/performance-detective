#include "detective/bottleneck_detector.hpp"

#include "detective/statistics.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace detective {

const char* to_string(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Low: return "low";
        case Severity::Medium: return "medium";
        case Severity::High: return "high";
    }
    return "info";
}

const char* to_string(Confidence c) {
    switch (c) {
        case Confidence::Low: return "low";
        case Confidence::Medium: return "medium";
        case Confidence::High: return "high";
    }
    return "low";
}

double evidence_weight(Confidence c) {
    switch (c) {
        case Confidence::High: return 1.0;
        case Confidence::Medium: return 0.7;
        case Confidence::Low: return 0.4;
    }
    return 0.4;
}

const char* to_string(FuncCategory c) {
    switch (c) {
        case FuncCategory::Other: return "other";
        case FuncCategory::Allocator: return "allocator";
        case FuncCategory::Synchronization: return "synchronization";
        case FuncCategory::StringOps: return "string";
        case FuncCategory::MemCopy: return "memcopy";
        case FuncCategory::HashTable: return "hash_table";
        case FuncCategory::OrderedMap: return "ordered_map";
        case FuncCategory::IoStream: return "iostream";
        case FuncCategory::Sorting: return "sorting";
        case FuncCategory::Syscall: return "syscall";
        case FuncCategory::MathLib: return "math";
    }
    return "other";
}

namespace {

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

// Reduces a demangled signature to the function's own qualified name: drops the parameter
// list and any return type, so `f(std::string const&)` is not mistaken for a string function.
std::string function_base_name(const std::string& full) {
    int depth = 0;
    size_t end = full.size();
    for (size_t i = 0; i < full.size(); ++i) {
        char c = full[i];
        if (c == '<') ++depth;
        else if (c == '>' && depth > 0) --depth;
        else if (c == '(' && depth == 0) {
            // `operator()` keeps its own parentheses.
            if (i >= 8 && full.compare(i - 8, 8, "operator") == 0 && i + 1 < full.size() && full[i + 1] == ')') {
                ++i;
                continue;
            }
            end = i;
            break;
        }
    }
    std::string head = full.substr(0, end);
    // A space at template depth 0 separates a return type from the name.
    depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < head.size(); ++i) {
        char c = head[i];
        if (c == '<') ++depth;
        else if (c == '>' && depth > 0) --depth;
        else if (c == ' ' && depth == 0 && head.compare(i > 8 ? i - 8 : 0, 8, "operator") != 0) start = i + 1;
    }
    return head.substr(start);
}

namespace {
// glibc exposes internal aliases such as __GI___libc_malloc or __GI_memcpy; strip them.
std::string normalize_libc_name(std::string n) {
    if (n.rfind("__GI_", 0) == 0) n = n.substr(5);
    while (n.size() > 1 && n[0] == '_') n = n.substr(1);
    if (n.rfind("libc_", 0) == 0) n = n.substr(5);
    return n;
}
}  // namespace

FuncCategory classify_function(const std::string& full_name, const std::string& module) {
    const std::string n = function_base_name(full_name);
    const std::string c = normalize_libc_name(n);  // e.g. "malloc", "memcpy_avx512_unaligned_erms"
    static const char* alloc_names[] = {"malloc", "free", "calloc", "realloc", "cfree", "int_malloc", "int_free",
                                        "malloc_consolidate", "int_realloc", "tcache_get", "tcache_put",
                                        "unlink_chunk", "sysmalloc", "malloc_trim", "posix_memalign",
                                        "aligned_alloc", "memalign", "int_free_merge_chunk",
                                        "int_free_create_chunk", "int_free_maybe_consolidate", "int_memalign",
                                        "tcache_init", "tcache_thread_shutdown", "arena_get2", "malloc_printerr"};
    for (const char* a : alloc_names)
        if (c == a) return FuncCategory::Allocator;
    if (starts(n, "operator new") || starts(n, "operator delete")) return FuncCategory::Allocator;
    if (contains(c, "pthread_mutex") || contains(c, "lll_lock") || contains(c, "pthread_cond") ||
        contains(c, "pthread_spin") || contains(c, "pthread_rwlock") || contains(c, "futex") ||
        contains(n, "std::mutex::lock") || c == "sched_yield")
        return FuncCategory::Synchronization;
    if (starts(c, "memmove") || starts(c, "memcpy") || starts(c, "memset") || starts(c, "memcmp") ||
        starts(c, "mempcpy") || starts(c, "wmemset"))
        return FuncCategory::MemCopy;
    if (contains(n, "basic_string") || starts(c, "strlen") || starts(c, "strcmp") || starts(c, "strchr") ||
        starts(c, "strncmp") || starts(c, "strcpy") || starts(c, "strnlen") || starts(c, "strrchr") ||
        starts(n, "std::to_string") || starts(n, "std::__cxx11::to_string") || contains(n, "char_traits"))
        return FuncCategory::StringOps;
    if (contains(n, "_Hashtable") || contains(n, "unordered_map") || contains(n, "unordered_set") ||
        contains(n, "_Hash_bytes") || contains(n, "_Prime_rehash_policy"))
        return FuncCategory::HashTable;
    if (contains(n, "_Rb_tree") || contains(n, "std::map<") || contains(n, "std::set<"))
        return FuncCategory::OrderedMap;
    if (contains(n, "basic_istream") || contains(n, "basic_ostream") || contains(n, "basic_stringbuf") ||
        contains(n, "basic_istringstream") || contains(n, "basic_ostringstream") || contains(n, "basic_ios") ||
        contains(n, "std::getline") || contains(n, "num_get") || contains(n, "num_put") ||
        contains(n, "std::locale") || contains(n, "basic_streambuf") || contains(c, "strtod") ||
        contains(c, "strtol") || contains(n, "__stoa") || contains(c, "printf") || contains(c, "itoa_word") ||
        contains(c, "scanf") || contains(n, "std::__cxx11::stod"))
        return FuncCategory::IoStream;
    if (contains(n, "std::__introsort") || contains(n, "std::__insertion_sort") || contains(n, "std::sort") ||
        contains(n, "std::__adjust_heap") || contains(n, "std::stable_sort") || starts(c, "qsort") ||
        contains(n, "std::__final_insertion_sort") || contains(n, "std::__unguarded_partition"))
        return FuncCategory::Sorting;
    static const char* sys[] = {"read", "write", "syscall", "nanosleep", "clock_nanosleep", "poll", "epoll_wait",
                                "select", "open", "openat", "close", "mmap", "munmap", "brk", "sbrk", "fsync",
                                "readv", "writev", "sendto", "recvfrom", "ioctl", "fstat", "lseek"};
    for (const char* x : sys)
        if (c == x || c == std::string(x) + "_nocancel") return FuncCategory::Syscall;
    if (contains(module, "libm.so")) return FuncCategory::MathLib;
    return FuncCategory::Other;
}

namespace {

std::string pct(double f) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f%%", f * 100.0);
    return b;
}

std::string num(double v, int prec = 3) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*g", prec, v);
    return b;
}

std::string short_name(const std::string& n, size_t max = 90) {
    return n.size() > max ? n.substr(0, max - 3) + "..." : n;
}

Confidence sampling_confidence(double p, uint64_t n, double threshold) {
    double se = stats::proportion_stderr(p, n);
    double lower = p - 2 * se;
    // High: the lower bound clears the threshold with a large sample, or clears twice the
    // threshold with a moderate one.
    if ((n >= 1000 && lower >= threshold) || (n >= 200 && lower >= 2 * threshold)) return Confidence::High;
    if (lower >= threshold * 0.5) return Confidence::Medium;
    return Confidence::Low;
}

Confidence downgrade(Confidence c) {
    return c == Confidence::High ? Confidence::Medium : Confidence::Low;
}

Severity severity_for_share(double p) {
    if (p >= 0.40) return Severity::High;
    if (p >= 0.20) return Severity::Medium;
    return Severity::Low;
}

std::string location(const FunctionStats& f) {
    if (f.source_file.empty()) return "";
    return f.source_file + ":" + std::to_string(f.source_line);
}

const std::vector<std::string> kSamplingLimits = {
    "Sampling estimate: shares are statistical; the 95% interval is roughly ±2 standard errors.",
    "cpu-clock sampling only observes on-CPU time; time spent blocked (I/O, locks, sleep) is invisible here.",
    "Inlined functions are attributed to the function they were inlined into."};

void finalize_score(Finding& f) { f.score = std::round(1000.0 * 100.0 * f.impact * evidence_weight(f.confidence)) / 1000.0; }

// For stacks whose leaf belongs to `cat`, find the nearest caller that is not in the category
// and not in a shared library: "who is driving this cost".
std::vector<std::pair<uint32_t, uint64_t>> top_drivers(const Profile& p, FuncCategory cat, size_t k) {
    const auto& funcs = p.functions();
    std::map<uint32_t, uint64_t> drivers;
    for (const auto& s : p.stacks()) {
        if (s.frames.empty()) continue;
        const auto& leaf = funcs[s.frames.back()];
        if (classify_function(leaf.name, leaf.module) != cat) continue;
        for (size_t i = s.frames.size(); i-- > 0;) {
            const auto& f = funcs[s.frames[i]];
            bool lib = f.module.find(".so") != std::string::npos;
            if (!lib && classify_function(f.name, f.module) == FuncCategory::Other && f.resolved) {
                drivers[s.frames[i]] += s.count;
                break;
            }
        }
    }
    std::vector<std::pair<uint32_t, uint64_t>> v(drivers.begin(), drivers.end());
    std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : funcs[a.first].id < funcs[b.first].id;
    });
    if (v.size() > k) v.resize(k);
    return v;
}

void sampling_rules(const AnalysisInput& in, const DetectorConfig& cfg, std::vector<Finding>& out) {
    const Profile& p = *in.profile;
    const auto& funcs = p.functions();
    const uint64_t n = p.total_samples();
    const double period_s = static_cast<double>(p.period_ns()) / 1e9;

    if (n < cfg.min_samples) {
        Finding f;
        f.id = "insufficient_samples";
        f.kind = "data_quality";
        f.source = "sampling";
        f.title = "Too few samples for reliable CPU analysis";
        f.facts.push_back("Collected " + std::to_string(n) + " samples (minimum for analysis: " +
                          std::to_string(cfg.min_samples) + ").");
        f.interpretation = "The program ran too briefly (or was mostly off-CPU) for sampling to locate hotspots.";
        f.next_actions = {"Increase the workload size or run time.", "Increase the sampling frequency (up to 10 kHz)."};
        f.limitations = {"No hotspot findings were generated from this session's samples."};
        out.push_back(std::move(f));
        return;
    }

    // ---- Data quality: stacks that provably stopped before an entry point.
    double complete = static_cast<double>(p.complete_stacks()) / static_cast<double>(n);
    double incomplete = static_cast<double>(p.incomplete_stacks()) / static_cast<double>(n);
    if (incomplete >= 0.3) {
        Finding f;
        f.id = "incomplete_stacks";
        f.kind = "data_quality";
        f.source = "sampling";
        f.title = "Many call stacks did not unwind to an entry point";
        f.facts.push_back(pct(incomplete) + " of samples have stacks whose outermost frame is an ordinary function "
                          "rather than _start/main/start_thread.");
        f.measurements.push_back({"complete_stack_fraction", complete, "ratio"});
        f.measurements.push_back({"incomplete_stack_fraction", incomplete, "ratio"});
        f.interpretation = "Frame-pointer unwinding broke in code compiled without frame pointers "
                           "(typical for system libraries). Self costs remain accurate; inclusive costs of "
                           "outer callers are under-counted.";
        f.next_actions = {"Rebuild the target with -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer.",
                          "Prefer inclusive numbers only for frames below the break point."};
        f.limitations = {"DWARF-based unwinding is not available in this build (no libunwind/libdw)."};
        out.push_back(std::move(f));
    }
    double unresolved = p.total_frames() ? static_cast<double>(p.unresolved_frames()) / static_cast<double>(p.total_frames()) : 0;
    if (unresolved > 0.25) {
        Finding f;
        f.id = "missing_symbols";
        f.kind = "data_quality";
        f.source = "sampling";
        f.title = "Many frames could not be resolved to function names";
        f.facts.push_back(pct(unresolved) + " of sampled frames have no symbol (shown as module+offset).");
        f.interpretation = "The binary or one of its libraries is stripped or lacks a symbol table.";
        f.next_actions = {"Build with -g and do not strip the binary being profiled.",
                          "Hot addresses are still reported as module+0xOFFSET and can be resolved manually with addr2line."};
        out.push_back(std::move(f));
    }

    // ---- Observer overhead: samples inside Performance Detective's own injected code.
    uint64_t observer = 0;
    for (const auto& fs : funcs)
        if (fs.module.rfind("libdetective", 0) == 0) observer += fs.self_samples;
    double observer_share = static_cast<double>(observer) / static_cast<double>(n);
    if (observer_share >= 0.02) {
        Finding f;
        f.id = "observer_overhead";
        f.kind = "data_quality";
        f.source = "sampling";
        f.title = "Profiler overhead is visible in this profile";
        f.facts.push_back(pct(observer_share) + " of samples were inside Performance Detective's injected "
                          "allocation tracker (libdetective_alloc.so), not in the program itself.");
        f.interpretation = "Allocation tracking slows every malloc/free. Shares of the program's own functions "
                           "are diluted, and allocation-heavy code looks more expensive than it is.";
        f.next_actions = {"Use this session for allocation counts and call sites only.",
                          "Run a second session without allocation tracking for unbiased CPU shares and timings."};
        f.measurements = {{"observer_share", observer_share, "fraction"}};
        f.limitations = {"Benchmark runs are always unprofiled and do not include this overhead."};
        out.push_back(std::move(f));
    }

    // ---- CPU hotspots (self cost).
    size_t emitted = 0;
    for (uint32_t fi : p.ranked_by_self()) {
        const auto& fs = funcs[fi];
        double share = static_cast<double>(fs.self_samples) / static_cast<double>(n);
        if (share < cfg.hotspot_self_fraction || emitted >= cfg.max_hotspots) break;
        if (fs.module.rfind("libdetective", 0) == 0) continue;  // observer overhead, reported above
        ++emitted;
        Finding f;
        f.kind = "cpu_hotspot";
        f.source = "sampling";
        f.id = "cpu_hotspot:" + fs.id;
        f.title = "CPU hotspot: " + short_name(fs.name, 70);
        f.impact = share;
        f.severity = severity_for_share(share);
        f.confidence = sampling_confidence(share, n, cfg.hotspot_self_fraction);
        double se = stats::proportion_stderr(share, n);
        double total_share = static_cast<double>(fs.total_samples) / static_cast<double>(n);
        f.facts.push_back(std::to_string(fs.self_samples) + " of " + std::to_string(n) +
                          " samples (" + pct(share) + " ± " + pct(2 * se) + ") were taken while executing `" +
                          short_name(fs.name) + "` itself (self cost).");
        f.facts.push_back("Inclusive share (function on stack): " + pct(total_share) + ".");
        f.facts.push_back("Estimated self CPU time ≈ " + num(static_cast<double>(fs.self_samples) * period_s) +
                          " s (samples × " + num(period_s * 1e3) + " ms period).");
        if (!location(fs).empty()) f.facts.push_back("Defined at " + location(fs) + ".");
        if (!fs.callers.empty()) {
            std::string callers;
            size_t c = 0;
            for (uint32_t ci : fs.callers) {
                if (c++ == 3) { callers += ", …"; break; }
                callers += (callers.empty() ? "" : ", ") + short_name(funcs[ci].name, 50);
            }
            f.facts.push_back("Observed callers: " + callers + ".");
        }
        FuncCategory cat = classify_function(fs.name, fs.module);
        if (cat == FuncCategory::Other) {
            f.interpretation = "The instructions of this function itself account for a large share of on-CPU time. "
                               "This usually indicates a hot loop, repeated work, or an algorithm whose cost grows "
                               "faster than the input.";
        } else {
            f.interpretation = std::string("This is a runtime/library function in the '") + to_string(cat) +
                               "' category; the cost is usually driven by how often callers invoke it, not by "
                               "the function's implementation.";
        }
        f.next_actions = {"Inspect the source of `" + short_name(fs.name, 60) + "`" +
                              (location(fs).empty() ? "" : " (" + location(fs) + ")") +
                              " for loops and repeated work.",
                          "Check how many times it is called and with what input sizes (instrumentation mode can count calls).",
                          "Re-profile after a change and compare sessions to confirm the share drops."};
        f.remediation = "Reduce the work done per call or the number of calls; see recommendations.";
        f.measurements = {{"self_samples", static_cast<double>(fs.self_samples), "samples"},
                          {"self_share", share, "fraction"},
                          {"inclusive_share", total_share, "fraction"},
                          {"self_cpu_estimate", static_cast<double>(fs.self_samples) * period_s, "s"}};
        f.affected_functions = {fs.id};
        f.affected_names = {fs.name};
        f.limitations = kSamplingLimits;
        if (!fs.resolved) {
            f.confidence = downgrade(f.confidence);
            f.limitations.push_back("The function name could not be resolved (stripped binary); it is identified by address only.");
        }
        out.push_back(std::move(f));
    }

    // ---- Hot path: follow the heaviest child chain from the root.
    CallTree t = p.tree();
    std::vector<uint32_t> path;
    uint32_t cur = 0;
    for (;;) {
        uint32_t best = 0;
        uint64_t best_v = 0;
        for (uint32_t c : t.nodes[cur].children)
            if (t.nodes[c].total > best_v) { best_v = t.nodes[c].total; best = c; }
        if (best_v == 0 || static_cast<double>(best_v) / static_cast<double>(n) < cfg.hot_path_fraction) break;
        path.push_back(best);
        cur = best;
    }
    // Drop trailing frames that are trivially on every stack (entry points / main).
    while (!path.empty()) {
        const auto& nm = funcs[static_cast<size_t>(t.nodes[path.back()].func)].name;
        if (is_entry_point_symbol(nm) || function_base_name(nm) == "main") path.pop_back();
        else break;
    }
    // If the path ends in a function already reported as a hotspot, add the path to that finding
    // as context instead of emitting a near-duplicate finding.
    Finding* hotspot_for_path = nullptr;
    if (path.size() >= 2) {
        const auto& lid = funcs[static_cast<size_t>(t.nodes[path.back()].func)].id;
        for (auto& f : out)
            if (f.kind == "cpu_hotspot" && f.id == "cpu_hotspot:" + lid) hotspot_for_path = &f;
    }
    if (hotspot_for_path) {
        std::string chain;
        size_t skip = path.size() > 6 ? path.size() - 6 : 0;
        if (skip) chain = "… → ";
        for (size_t i = skip; i < path.size(); ++i)
            chain += (i > skip ? " → " : "") +
                     short_name(funcs[static_cast<size_t>(t.nodes[path[i]].func)].name, 40);
        double share = static_cast<double>(t.nodes[path.back()].total) / static_cast<double>(n);
        hotspot_for_path->facts.push_back(pct(share) + " of all samples reach it through one dominant call path: " +
                                          chain + ".");
    } else if (path.size() >= 2) {
        // Report the path down to the last node holding >= hot_path_fraction.
        const auto& last = t.nodes[path.back()];
        double share = static_cast<double>(last.total) / static_cast<double>(n);
        Finding f;
        f.kind = "hot_path";
        f.source = "sampling";
        const auto& lf = funcs[static_cast<size_t>(last.func)];
        f.id = "hot_path:" + lf.id;
        f.title = "Dominant call path ends in " + short_name(lf.name, 60);
        f.impact = share;
        f.severity = severity_for_share(share);
        f.confidence = sampling_confidence(share, n, cfg.hot_path_fraction);
        std::string chain;
        size_t skip = path.size() > 8 ? path.size() - 8 : 0;
        if (skip) chain = "… → ";
        for (size_t i = skip; i < path.size(); ++i) {
            const auto& pf = funcs[static_cast<size_t>(t.nodes[path[i]].func)];
            chain += (i > skip ? " → " : "") + short_name(pf.name, 40);
            f.affected_functions.push_back(pf.id);
            f.affected_names.push_back(pf.name);
        }
        f.facts.push_back(pct(share) + " of all samples pass through this exact call path: " + chain + ".");
        f.facts.push_back("Below this point no single child holds ≥" + pct(cfg.hot_path_fraction) + " of samples.");
        f.interpretation = "Execution time is concentrated in one call chain rather than spread out; optimizing "
                           "anything off this path cannot save more than " + pct(1 - share) + " of CPU time.";
        f.next_actions = {"Focus investigation on the last frames of this path.",
                          "Use the flame graph: click the path to zoom in and see how its cost splits."};
        f.measurements = {{"path_share", share, "fraction"}, {"path_depth", static_cast<double>(path.size()), "frames"}};
        f.limitations = kSamplingLimits;
        if (incomplete >= 0.3) f.limitations.push_back("Many stacks were incomplete; the path may be truncated at the top.");
        out.push_back(std::move(f));
    }

    // ---- Deep recursion.
    for (uint32_t fi : p.ranked_by_total()) {
        const auto& fs = funcs[fi];
        double share = static_cast<double>(fs.total_samples) / static_cast<double>(n);
        if (share < cfg.recursion_fraction) break;
        if (fs.max_recursion_depth < cfg.recursion_depth) continue;
        Finding f;
        f.kind = "recursion";
        f.source = "sampling";
        f.id = "recursion:" + fs.id;
        f.title = "Deep recursion in " + short_name(fs.name, 60);
        f.impact = share;
        f.severity = severity_for_share(share);
        f.confidence = sampling_confidence(share, n, cfg.recursion_fraction);
        f.facts.push_back("`" + short_name(fs.name) + "` appeared up to " + std::to_string(fs.max_recursion_depth) +
                          " times in a single sampled stack.");
        f.facts.push_back("It is on the stack in " + pct(share) + " of samples.");
        f.interpretation = "Recursive calls dominate this workload. Exponential recursion (re-solving the same "
                           "sub-problems) or very deep recursion both show this signature.";
        f.next_actions = {"Check whether identical sub-problems are recomputed (memoize / dynamic programming).",
                          "Consider an iterative formulation if depth is large (stack overflow risk)."};
        f.measurements = {{"max_recursion_depth", static_cast<double>(fs.max_recursion_depth), "frames"},
                          {"inclusive_share", share, "fraction"}};
        f.affected_functions = {fs.id};
        f.affected_names = {fs.name};
        f.limitations = kSamplingLimits;
        f.limitations.push_back("Observed depth is capped by the kernel callchain limit (perf_event_max_stack).");
        out.push_back(std::move(f));
    }

    // ---- Category rules.
    std::map<FuncCategory, uint64_t> cat_self;
    std::map<FuncCategory, std::vector<uint32_t>> cat_funcs;
    for (uint32_t i = 0; i < funcs.size(); ++i) {
        auto c = classify_function(funcs[i].name, funcs[i].module);
        if (c == FuncCategory::Other || funcs[i].self_samples == 0) continue;
        cat_self[c] += funcs[i].self_samples;
        cat_funcs[c].push_back(i);
    }
    struct CatRule { FuncCategory cat; const char* kind; const char* title; double threshold; const char* interp; };
    const CatRule rules[] = {
        {FuncCategory::Allocator, "allocator_cpu", "Significant CPU time inside the memory allocator",
         cfg.category_fraction,
         "Frequent heap allocation and deallocation is costing CPU time. Typical causes: temporary objects in "
         "loops, containers that grow without reserve(), strings that exceed the small-string buffer."},
        {FuncCategory::MemCopy, "memcopy_cpu", "Significant CPU time copying memory", cfg.category_fraction,
         "Large or frequent memcpy/memmove calls suggest data being copied rather than moved or referenced."},
        {FuncCategory::StringOps, "string_cpu", "Significant CPU time in string operations", cfg.category_fraction,
         "std::string construction, concatenation or comparison is a large share of CPU time."},
        {FuncCategory::HashTable, "hash_cpu", "Significant CPU time in hash-table operations", cfg.category_fraction,
         "Hash-table lookups/inserts are expensive: costly hashing of keys, rehashing, or many lookups."},
        {FuncCategory::OrderedMap, "ordered_map_cpu", "Significant CPU time in ordered map/set operations",
         cfg.category_fraction,
         "Red-black-tree operations (std::map/std::set) are a large cost: O(log n) comparisons per lookup with "
         "pointer chasing and, for string keys, string comparisons."},
        {FuncCategory::IoStream, "iostream_cpu", "Significant CPU time in iostream / text parsing", cfg.category_fraction,
         "Stream-based parsing (istringstream, getline, stod) carries locale and virtual-call overhead per field."},
        {FuncCategory::Synchronization, "sync_cpu", "CPU time spent in locking primitives", cfg.sync_fraction,
         "Threads spend CPU acquiring/releasing locks. Time spent *blocked* on a lock is not visible to "
         "cpu-clock sampling, so this is a lower bound on the synchronization cost."},
        {FuncCategory::Syscall, "syscall_cpu", "CPU time in system-call wrappers", cfg.category_fraction,
         "Frequent system calls are costing user-space time; the kernel side is excluded from these samples."},
    };
    for (const auto& r : rules) {
        uint64_t s = cat_self[r.cat];
        double share = static_cast<double>(s) / static_cast<double>(n);
        if (share < r.threshold) continue;
        Finding f;
        f.kind = r.kind;
        f.source = "sampling";
        f.id = std::string(r.kind) + ":process";
        f.title = r.title;
        f.impact = share;
        f.severity = severity_for_share(share);
        f.confidence = sampling_confidence(share, n, r.threshold);
        f.facts.push_back(pct(share) + " of samples have a leaf frame in " + to_string(r.cat) + " functions.");
        auto& members = cat_funcs[r.cat];
        std::sort(members.begin(), members.end(), [&](uint32_t a, uint32_t b) {
            return funcs[a].self_samples != funcs[b].self_samples ? funcs[a].self_samples > funcs[b].self_samples
                                                                  : funcs[a].id < funcs[b].id;
        });
        std::string top;
        for (size_t i = 0; i < members.size() && i < 4; ++i)
            top += (i ? ", " : "") + short_name(funcs[members[i]].name, 45) + " (" +
                   pct(static_cast<double>(funcs[members[i]].self_samples) / static_cast<double>(n)) + ")";
        f.facts.push_back("Top functions: " + top + ".");
        auto drivers = top_drivers(p, r.cat, 3);
        if (!drivers.empty()) {
            std::string d;
            for (size_t i = 0; i < drivers.size(); ++i) {
                d += (i ? ", " : "") + short_name(funcs[drivers[i].first].name, 50) + " (" +
                     pct(static_cast<double>(drivers[i].second) / static_cast<double>(n)) + ")";
                f.affected_functions.push_back(funcs[drivers[i].first].id);
                f.affected_names.push_back(funcs[drivers[i].first].name);
            }
            f.facts.push_back("Nearest application callers driving this cost: " + d + ".");
        }
        f.interpretation = r.interp;
        f.next_actions = {"Look at the application callers listed above; they decide how often these functions run."};
        if (r.cat == FuncCategory::Allocator)
            f.next_actions.push_back("Enable allocation tracking for exact allocation counts and call sites.");
        f.measurements = {{"category_self_share", share, "fraction"},
                          {"category_self_samples", static_cast<double>(s), "samples"}};
        f.limitations = kSamplingLimits;
        f.limitations.push_back("Category membership is decided by function-name patterns; inlined library code "
                                "is attributed to its caller and is not counted.");
        out.push_back(std::move(f));
    }
}

void process_rules(const AnalysisInput& in, std::vector<Finding>& out) {
    const ProcessResult& pr = *in.process;
    if (!pr.started || pr.wall_seconds < 0.2) return;
    const double cpu = pr.user_cpu_seconds + pr.sys_cpu_seconds;
    const double util = cpu / pr.wall_seconds;  // average cores busy
    const long ncpu = std::max(1L, ::sysconf(_SC_NPROCESSORS_ONLN));
    const double vcs_rate = static_cast<double>(pr.voluntary_ctx_switches) / pr.wall_seconds;

    size_t threads = std::max<size_t>(1, in.sampled_threads);
    double expected = std::min<double>(static_cast<double>(threads), static_cast<double>(ncpu));
    double idle = std::clamp(1.0 - util / expected, 0.0, 1.0);
    if (idle >= 0.35) {
        Finding f;
        f.kind = "off_cpu";
        f.source = "process";
        f.id = "off_cpu:process";
        f.title = threads > 1 ? "Threads spend much of their time waiting (off-CPU)"
                              : "Process spends much of its time waiting (off-CPU)";
        f.impact = idle;
        f.severity = idle >= 0.6 ? Severity::High : Severity::Medium;
        f.confidence = (vcs_rate > 200 || pr.wall_seconds > 1.0) ? Confidence::Medium : Confidence::Low;
        f.facts.push_back("CPU time " + num(cpu) + " s over " + num(pr.wall_seconds) + " s wall = " + num(util, 2) +
                          " cores busy on average.");
        f.facts.push_back(std::to_string(threads) + " thread(s) were observed on-CPU; the machine has " +
                          std::to_string(ncpu) + " CPU(s), so up to " + num(expected, 2) + " cores could be busy.");
        f.facts.push_back(std::to_string(pr.voluntary_ctx_switches) + " voluntary context switches (" +
                          num(vcs_rate) + "/s), " + std::to_string(pr.involuntary_ctx_switches) + " involuntary.");
        f.interpretation = threads > 1
                               ? "Threads are frequently blocked rather than computing: lock contention, waiting on "
                                 "each other (join/condition variables) or I/O. A high voluntary context-switch rate "
                                 "is consistent with blocking on a contended mutex or futex."
                               : "The process is blocked for much of its run time: I/O, sleeping or waiting on another "
                                 "process.";
        f.next_actions = {"Check whether work inside locked regions can be moved outside the lock.",
                          "Look for sync_cpu findings and pthread/futex frames in the flame graph.",
                          "Use an off-CPU tool (e.g. `perf sched`, `offcputime` from bcc) to see where threads block."};
        f.measurements = {{"wall_seconds", pr.wall_seconds, "s"},
                          {"cpu_seconds", cpu, "s"},
                          {"avg_cores_busy", util, "cores"},
                          {"voluntary_ctx_switches_per_s", vcs_rate, "1/s"}};
        f.limitations = {"Derived from whole-process getrusage totals; it does not say *where* threads wait.",
                         "On a busy machine, involuntary preemption also lowers utilization."};
        out.push_back(std::move(f));
    }
    if (cpu >= 0.1 && pr.sys_cpu_seconds / cpu >= 0.3) {
        double share = pr.sys_cpu_seconds / cpu;
        Finding f;
        f.kind = "kernel_time";
        f.source = "process";
        f.id = "kernel_time:process";
        f.title = "High share of CPU time spent in the kernel";
        f.impact = share;
        f.severity = severity_for_share(share);
        f.confidence = Confidence::Medium;
        f.facts.push_back("System CPU time " + num(pr.sys_cpu_seconds) + " s of " + num(cpu) + " s total (" + pct(share) + ").");
        f.interpretation = "Frequent system calls, page faults (large allocations / first-touch memory) or "
                           "contended futexes are likely.";
        f.next_actions = {"Look for many small read/write calls (batch them) or repeated mmap/munmap from large allocations.",
                          "Run `strace -c` on the target to count system calls."};
        f.measurements = {{"sys_cpu_seconds", pr.sys_cpu_seconds, "s"}, {"sys_share", share, "fraction"}};
        f.limitations = {"Kernel frames are excluded from samples (exclude_kernel), so the specific syscalls are not identified."};
        out.push_back(std::move(f));
    }
}

void instrumentation_rules(const AnalysisInput& in, std::vector<Finding>& out) {
    const auto& r = *in.instrumentation;
    double total_self = 0;
    for (const auto& s : r.scopes) total_self += s.self_ns;
    if (total_self <= 0) return;
    for (const auto& s : r.scopes) {
        double share = s.self_ns / total_self;
        if (share >= 0.25) {
            Finding f;
            f.kind = "instr_hot_scope";
            f.source = "instrumentation";
            f.id = "instr_hot_scope:" + s.name;
            f.title = "Instrumented scope dominates: " + s.name;
            f.impact = share;
            f.severity = severity_for_share(share);
            f.confidence = s.count >= 10 ? Confidence::High : Confidence::Medium;
            f.facts.push_back("`" + s.name + "` accounts for " + pct(share) + " of instrumented self time (wall clock).");
            f.facts.push_back(std::to_string(s.count) + " calls, mean " + num(s.mean_ns / 1e3) + " µs, max " +
                              num(s.max_ns / 1e3) + " µs" +
                              (s.percentiles_valid ? ", p99 " + num(s.p99_ns / 1e3) + " µs" : "") + ".");
            f.interpretation = "Wall-clock time includes waiting; confirm with sampling whether this scope is "
                               "computing (on-CPU) or blocked.";
            f.next_actions = {"Add finer DETECTIVE_SCOPE markers inside this scope to split its cost.",
                              "Profile the same build in sampling mode to see which functions run inside it."};
            f.measurements = {{"self_ns", s.self_ns, "ns"}, {"calls", static_cast<double>(s.count), "calls"},
                              {"mean_ns", s.mean_ns, "ns"}};
            f.affected_names = {s.name};
            f.affected_functions = {s.name};
            f.limitations = {"Measured with steady_clock (wall time), not CPU time.",
                             "Only code wrapped in DETECTIVE_SCOPE/DETECTIVE_FUNCTION is visible."};
            out.push_back(std::move(f));
        }
        double overhead = r.clock_overhead_ns * static_cast<double>(s.count);
        if (s.count >= 100'000 && s.total_ns / total_self >= 0.05 && s.mean_ns < 2'000) {
            Finding f;
            f.kind = "excessive_calls";
            f.source = "instrumentation";
            f.id = "excessive_calls:" + s.name;
            f.title = "Very frequent short calls: " + s.name;
            f.impact = std::min(1.0, s.total_ns / total_self);
            f.severity = Severity::Medium;
            f.confidence = overhead > 0.5 * s.total_ns ? Confidence::Low : Confidence::Medium;
            f.facts.push_back(std::to_string(s.count) + " calls with a mean of " + num(s.mean_ns) + " ns each.");
            f.facts.push_back("Estimated instrumentation clock overhead for these calls: " + num(overhead / 1e6) +
                              " ms (" + num(r.clock_overhead_ns) + " ns per scope).");
            f.interpretation = "Per-call overhead (function call, setup, bookkeeping) may be significant relative to the "
                               "useful work; batching or hoisting work out of the call can help.";
            f.next_actions = {"Check whether callers can batch inputs into fewer calls.",
                              "Remove the scope marker from this tiny function and re-measure (the marker itself costs time)."};
            f.measurements = {{"calls", static_cast<double>(s.count), "calls"}, {"mean_ns", s.mean_ns, "ns"}};
            f.affected_names = {s.name};
            f.affected_functions = {s.name};
            f.limitations = {"Instrumentation overhead inflates very short scopes."};
            out.push_back(std::move(f));
        }
    }
}

void allocation_rules(const AnalysisInput& in, std::vector<Finding>& out) {
    const auto& m = *in.memory;
    double wall = (in.process && in.process->wall_seconds > 0) ? in.process->wall_seconds : 0;
    double rate = wall > 0 ? static_cast<double>(m.total_allocations) / wall : 0;
    if (m.total_allocations < 100'000 && rate < 100'000) return;
    Finding f;
    f.kind = "allocation_pressure";
    f.source = "allocation";
    f.id = "allocation_pressure:process";
    f.title = "High heap allocation rate";
    // Impact: allocator CPU share when sampling measured it; otherwise a rate-based proxy.
    double alloc_share = -1;
    if (in.profile && in.profile->total_samples() > 0) {
        uint64_t s = 0;
        for (const auto& fs : in.profile->functions())
            if (classify_function(fs.name, fs.module) == FuncCategory::Allocator) s += fs.self_samples;
        alloc_share = static_cast<double>(s) / static_cast<double>(in.profile->total_samples());
    }
    f.impact = alloc_share >= 0 ? alloc_share : std::min(1.0, rate / 1e7);
    f.severity = rate >= 5e6 ? Severity::High : (rate >= 1e6 ? Severity::Medium : Severity::Low);
    f.confidence = alloc_share >= 0 ? Confidence::High : Confidence::Medium;
    f.facts.push_back(std::to_string(m.total_allocations) + " heap allocations (" + num(rate) + "/s), " +
                      num(static_cast<double>(m.bytes_requested) / 1048576.0) + " MiB requested in total.");
    f.facts.push_back("Peak live heap " + num(static_cast<double>(m.peak_live_bytes) / 1048576.0) +
                      " MiB (allocator usable-size accounting).");
    if (alloc_share >= 0) f.facts.push_back("Sampling attributes " + pct(alloc_share) + " of CPU samples to allocator functions.");
    std::string sites;
    for (size_t i = 0; i < m.sites.size() && i < 3; ++i) {
        sites += (i ? "; " : "") + short_name(m.sites[i].function, 60) + " — " + std::to_string(m.sites[i].count) + " allocs";
        f.affected_names.push_back(m.sites[i].function);
        f.affected_functions.push_back(m.sites[i].function);
    }
    if (!sites.empty()) f.facts.push_back("Top allocation call sites: " + sites + ".");
    f.interpretation = "Many short-lived allocations usually come from temporaries created in loops; each costs an "
                       "allocator round-trip and hurts cache locality.";
    f.next_actions = {"Inspect the top call sites; look for objects created per iteration that could be reused.",
                      "Use reserve() for containers whose final size is known."};
    f.measurements = {{"allocations", static_cast<double>(m.total_allocations), "count"},
                      {"allocations_per_s", rate, "1/s"},
                      {"bytes_requested", static_cast<double>(m.bytes_requested), "bytes"},
                      {"peak_live_bytes", static_cast<double>(m.peak_live_bytes), "bytes"}};
    f.limitations = {"Counts come from an LD_PRELOAD malloc/operator-new interposer; statically linked or custom allocators are not seen.",
                     "Call sites are the immediate caller of malloc/operator new, which may be a library function."};
    if (alloc_share < 0) f.limitations.push_back("Impact is a rate-based proxy (allocations/s ÷ 10^7) because no sampling data was collected.");
    out.push_back(std::move(f));
}

}  // namespace

std::vector<Finding> detect_bottlenecks(const AnalysisInput& in, const DetectorConfig& cfg) {
    std::vector<Finding> out;
    if (in.profile) sampling_rules(in, cfg, out);
    if (in.process) process_rules(in, out);
    if (in.instrumentation) instrumentation_rules(in, out);
    if (in.memory) allocation_rules(in, out);
    for (auto& f : out) finalize_score(f);
    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });
    return out;
}

}  // namespace detective
