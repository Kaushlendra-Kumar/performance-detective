// Aggregated profile: function table, deduplicated stacks and call trees.
//
// Every number here derived from sampling is an ESTIMATE: a function's share
// of samples estimates its share of on-CPU time. Waiting time (I/O, sleeping,
// blocked on a lock) does not produce cpu-clock samples.
#pragma once

#include "detective/symbol_resolver.hpp"
#include "detective/types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace detective {

struct FunctionStats {
    std::string id;  // stable: hex FNV-1a of "module!name"
    std::string name;
    std::string module;
    std::string source_file;
    uint32_t source_line = 0;
    bool resolved = false;
    uint64_t self_samples = 0;
    uint64_t total_samples = 0;  // inclusive, counted once per sample even if recursive
    uint32_t max_recursion_depth = 1;
    std::set<uint32_t> callers;  // function indices
    std::set<uint32_t> callees;
    std::map<uint32_t, uint64_t> thread_samples;  // tid -> inclusive samples
};

// Identical stacks are merged; frames are function indices, outermost first.
struct StackEntry {
    uint32_t tid = 0;
    uint64_t count = 0;
    std::vector<uint32_t> frames;
};

struct ThreadStats {
    uint32_t tid = 0;
    std::string name;
    uint64_t samples = 0;
};

struct ModuleStats {
    std::string name;
    uint64_t self_samples = 0;
    uint64_t total_samples = 0;
};

struct CallNode {
    int32_t func = -1;  // -1 for the synthetic root
    uint64_t total = 0;
    uint64_t self = 0;
    std::vector<uint32_t> children;  // node indices, sorted by function name for determinism
};

struct TreeFilter {
    std::optional<uint32_t> tid;
    std::optional<std::string> module;  // keep stacks that contain this module
    bool inverted = false;  // leaf-first ("callers of hot functions") tree
};

struct CallTree {
    std::vector<CallNode> nodes;  // nodes[0] = root
    uint64_t total = 0;
};

class Profile {
public:
    // Builds the aggregated profile from raw samples. Resolves symbols and,
    // if `with_source` is set, source locations for sampled functions via addr2line.
    static Profile build(const RawProfile& raw, SymbolResolver& resolver, bool with_source = true);

    // Construct directly from aggregated data (deserialization, tests, demo data).
    static Profile from_parts(std::vector<FunctionStats> functions, std::vector<StackEntry> stacks,
                              std::map<uint32_t, std::string> thread_names, uint64_t period_ns);

    CallTree tree(const TreeFilter& filter = {}) const;

    const std::vector<FunctionStats>& functions() const { return functions_; }
    const std::vector<StackEntry>& stacks() const { return stacks_; }
    std::vector<ThreadStats> threads() const;
    std::vector<ModuleStats> modules() const;
    std::vector<uint32_t> ranked_by_self() const;
    std::vector<uint32_t> ranked_by_total() const;
    std::optional<uint32_t> find_function(const std::string& id) const;

    uint64_t total_samples() const { return total_samples_; }
    uint64_t period_ns() const { return period_ns_; }
    double estimated_cpu_seconds() const { return double(total_samples_) * double(period_ns_) / 1e9; }

    // Stack-quality diagnostics.
    uint64_t complete_stacks() const { return complete_stacks_; }  // reached a known entry point
    // Outermost frame is a resolved, non-entry function: unwinding provably stopped early.
    uint64_t incomplete_stacks() const { return incomplete_stacks_; }
    uint64_t total_frames() const { return total_frames_; }
    uint64_t unresolved_frames() const { return unresolved_frames_; }
    uint32_t max_depth() const { return max_depth_; }
    // Samples whose frameless-leaf caller was recovered from the stack-top word.
    uint64_t recovered_frames() const { return recovered_frames_; }
    const std::map<uint32_t, std::string>& thread_names() const { return thread_names_; }

private:
    void finalize();  // derive function stats from stacks

    std::vector<FunctionStats> functions_;
    std::vector<StackEntry> stacks_;
    std::map<uint32_t, std::string> thread_names_;
    uint64_t total_samples_ = 0;
    uint64_t period_ns_ = 0;
    uint64_t complete_stacks_ = 0;
    uint64_t incomplete_stacks_ = 0;
    uint64_t total_frames_ = 0;
    uint64_t unresolved_frames_ = 0;
    uint32_t max_depth_ = 0;
    uint64_t recovered_frames_ = 0;
};

std::string stable_function_id(const std::string& module, const std::string& name);
bool is_entry_point_symbol(const std::string& name);

}  // namespace detective
