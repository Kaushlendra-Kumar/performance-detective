#include "detective/call_tree.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace detective {

std::string stable_function_id(const std::string& module, const std::string& name) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](unsigned char c) {
        h ^= c;
        h *= 1099511628211ULL;
    };
    for (unsigned char c : module) mix(c);
    mix('!');
    for (unsigned char c : name) mix(c);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool is_entry_point_symbol(const std::string& raw) {
    std::string name = raw.rfind("__GI_", 0) == 0 ? raw.substr(5) : raw;
    static const char* entries[] = {"_start", "__libc_start_main", "__libc_start_call_main",
                                    "start_thread", "clone", "clone3", "__clone", "__clone3",
                                    "__clone_internal", "_dl_start_user", "_dl_start"};
    for (const char* e : entries)
        if (name == e) return true;
    return false;
}

namespace {
struct PidIp {
    uint32_t pid;
    uint64_t ip;
    bool operator==(const PidIp& o) const { return pid == o.pid && ip == o.ip; }
};
struct PidIpHash {
    size_t operator()(const PidIp& k) const noexcept { return std::hash<uint64_t>()(k.ip * 0x9E3779B97F4A7C15ULL ^ k.pid); }
};
struct StackKeyHash {
    size_t operator()(const std::vector<uint32_t>& v) const noexcept {
        uint64_t h = 1469598103934665603ULL;
        for (uint32_t x : v) { h ^= x; h *= 1099511628211ULL; }
        return static_cast<size_t>(h);
    }
};
}  // namespace

Profile Profile::build(const RawProfile& raw, SymbolResolver& resolver, bool with_source) {
    resolver.add_mappings(raw.mappings);
    Profile p;
    p.period_ns_ = raw.sample_period_ns;

    std::unordered_map<std::string, uint32_t> index;  // "module\0name" -> function index (once per symbol)
    struct Rep { uint32_t pid; uint64_t ip; };
    std::vector<Rep> reps;
    // Address -> function index. String work happens once per distinct address, not per frame.
    std::unordered_map<PidIp, uint32_t, PidIpHash> by_addr;
    by_addr.reserve(4096);

    auto func_of = [&](uint32_t pid, uint64_t ip) -> uint32_t {
        auto it = by_addr.find({pid, ip});
        if (it != by_addr.end()) return it->second;
        ResolvedFrame f = resolver.resolve(pid, ip);
        std::string key = f.module;
        key.push_back('\0');
        key += f.function;
        uint32_t idx;
        auto ki = index.find(key);
        if (ki != index.end()) {
            idx = ki->second;
        } else {
            idx = static_cast<uint32_t>(p.functions_.size());
            FunctionStats fs;
            fs.name = f.function;
            fs.module = f.module;
            fs.resolved = f.resolved;
            fs.id = stable_function_id(fs.module, fs.name);
            p.functions_.push_back(std::move(fs));
            reps.push_back({pid, ip});
            index.emplace(std::move(key), idx);
        }
        by_addr.emplace(PidIp{pid, ip}, idx);
        return idx;
    };

    // Frameless-leaf recovery decisions, cached per (pid, stack-top word, leaf function).
    std::unordered_map<PidIp, int64_t, PidIpHash> call_target_cache;  // stack_top -> target func or -1
    auto recovered_caller_ok = [&](uint32_t pid, uint64_t ret, uint32_t leaf_fn) -> bool {
        int64_t target_fn;
        auto it = call_target_cache.find({pid, ret});
        if (it != call_target_cache.end()) {
            target_fn = it->second;
        } else {
            target_fn = -1;
            if (resolver.is_return_address(pid, ret)) {
                if (auto t = resolver.direct_call_target(pid, ret)) target_fn = func_of(pid, *t);
            }
            call_target_cache.emplace(PidIp{pid, ret}, target_fn);
        }
        if (target_fn < 0) return false;
        const auto& tf = p.functions_[static_cast<size_t>(target_fn)];
        const auto& lf = p.functions_[leaf_fn];
        // The call instruction must target the sampled function itself, or a PLT stub for a
        // call into another module. Stale return addresses in uninitialized slots fail this.
        return (static_cast<uint32_t>(target_fn) == leaf_fn && tf.resolved) ||
               (tf.name == "[plt stub]" && tf.module != lf.module);
    };

    std::unordered_map<std::vector<uint32_t>, uint64_t, StackKeyHash> merged;  // frames + tid -> count
    merged.reserve(raw.samples.size() / 4 + 16);
    std::vector<uint32_t> frames;
    for (const auto& s : raw.samples) {
        frames.clear();
        if (s.ips.empty()) continue;
        uint32_t leaf = func_of(s.pid, s.ips[0]);
        frames.push_back(leaf);
        // Frameless leaf: the frame-pointer chain skipped the leaf's caller; the word at %rsp
        // holds its return address.
        if (s.stack_top != 0 && (s.ips.size() < 2 || s.ips[1] != s.stack_top) &&
            recovered_caller_ok(s.pid, s.stack_top, leaf)) {
            uint32_t caller = func_of(s.pid, s.stack_top - 1);
            if (caller != leaf) {
                frames.push_back(caller);
                ++p.recovered_frames_;
            }
        }
        for (size_t i = 1; i < s.ips.size(); ++i) {
            // Caller frames hold return addresses; step back into the call instruction.
            uint64_t ip = s.ips[i] == 0 ? 0 : s.ips[i] - 1;
            frames.push_back(func_of(s.pid, ip));
        }
        std::reverse(frames.begin(), frames.end());  // outermost first
        frames.push_back(s.tid);                     // key = frames + tid
        merged[frames] += 1;
    }
    p.stacks_.reserve(merged.size());
    for (auto& [k, count] : merged) {
        StackEntry e;
        e.tid = k.back();
        e.count = count;
        e.frames.assign(k.begin(), k.end() - 1);
        p.stacks_.push_back(std::move(e));
    }
    // Deterministic order independent of hashing.
    std::sort(p.stacks_.begin(), p.stacks_.end(), [](const StackEntry& a, const StackEntry& b) {
        if (a.tid != b.tid) return a.tid < b.tid;
        return a.frames < b.frames;
    });

    p.thread_names_ = raw.thread_names;
    p.finalize();
    // Threads that never renamed themselves inherit their process's command name.
    std::map<uint32_t, uint32_t> tid_pid;
    for (const auto& s : raw.samples) tid_pid[s.tid] = s.pid;
    for (const auto& t : p.threads()) {
        if (p.thread_names_.count(t.tid)) continue;
        auto pid = tid_pid.find(t.tid);
        auto pname = pid != tid_pid.end() ? raw.thread_names.find(pid->second) : raw.thread_names.end();
        p.thread_names_[t.tid] = pname != raw.thread_names.end() ? pname->second : "tid " + std::to_string(t.tid);
    }

    if (with_source) {
        // Look up source locations for the most expensive functions only (bounded cost).
        auto ranked = p.ranked_by_total();
        if (ranked.size() > 400) ranked.resize(400);
        std::map<std::string, std::vector<uint64_t>> requests;
        std::map<uint32_t, std::pair<std::string, uint64_t>> where;
        for (uint32_t fi : ranked) {
            if (!p.functions_[fi].resolved) continue;
            auto start = resolver.function_start_vaddr(reps[fi].pid, reps[fi].ip);
            auto mod = resolver.module_vaddr(reps[fi].pid, reps[fi].ip);
            if (!start || !mod) continue;
            requests[mod->first].push_back(*start);
            where[fi] = {mod->first, *start};
        }
        auto locs = lookup_source_locations(requests);
        for (const auto& [fi, key] : where) {
            auto it = locs.find(key);
            if (it != locs.end()) {
                p.functions_[fi].source_file = it->second.first;
                p.functions_[fi].source_line = it->second.second;
            }
        }
    }
    return p;
}

Profile Profile::from_parts(std::vector<FunctionStats> functions, std::vector<StackEntry> stacks,
                            std::map<uint32_t, std::string> thread_names, uint64_t period_ns) {
    Profile p;
    p.functions_ = std::move(functions);
    for (auto& f : p.functions_) {
        if (f.id.empty()) f.id = stable_function_id(f.module, f.name);
    }
    p.stacks_ = std::move(stacks);
    // Drop stacks that reference unknown functions (corrupt input).
    p.stacks_.erase(std::remove_if(p.stacks_.begin(), p.stacks_.end(),
                                   [&](const StackEntry& s) {
                                       for (uint32_t f : s.frames)
                                           if (f >= p.functions_.size()) return true;
                                       return s.frames.empty() || s.count == 0;
                                   }),
                    p.stacks_.end());
    p.thread_names_ = std::move(thread_names);
    p.period_ns_ = period_ns;
    p.finalize();
    return p;
}

void Profile::finalize() {
    for (auto& f : functions_) {
        f.self_samples = f.total_samples = 0;
        f.max_recursion_depth = 1;
        f.callers.clear();
        f.callees.clear();
        f.thread_samples.clear();
    }
    total_samples_ = complete_stacks_ = incomplete_stacks_ = total_frames_ = unresolved_frames_ = 0;
    max_depth_ = 0;
    std::unordered_map<uint32_t, uint32_t> seen;  // function -> occurrences in this stack
    for (const auto& s : stacks_) {
        total_samples_ += s.count;
        total_frames_ += s.count * s.frames.size();
        max_depth_ = std::max<uint32_t>(max_depth_, static_cast<uint32_t>(s.frames.size()));
        if (s.frames.empty()) continue;
        const auto& outer = functions_[s.frames.front()];
        if (is_entry_point_symbol(outer.name)) complete_stacks_ += s.count;
        else if (outer.resolved) incomplete_stacks_ += s.count;
        functions_[s.frames.back()].self_samples += s.count;
        seen.clear();
        for (size_t i = 0; i < s.frames.size(); ++i) {
            uint32_t f = s.frames[i];
            if (!functions_[f].resolved) unresolved_frames_ += s.count;
            if (++seen[f] == 1) {
                functions_[f].total_samples += s.count;
                functions_[f].thread_samples[s.tid] += s.count;
            }
            if (i > 0) {
                uint32_t parent = s.frames[i - 1];
                functions_[f].callers.insert(parent);
                functions_[parent].callees.insert(f);
            }
        }
        for (const auto& [f, n] : seen)
            functions_[f].max_recursion_depth = std::max(functions_[f].max_recursion_depth, n);
    }
}

CallTree Profile::tree(const TreeFilter& filter) const {
    CallTree t;
    t.nodes.push_back({});
    // Child order: by function name (then module), via a precomputed integer rank.
    std::vector<uint32_t> order(functions_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (functions_[a].name != functions_[b].name) return functions_[a].name < functions_[b].name;
        return functions_[a].module < functions_[b].module;
    });
    std::vector<uint32_t> rank(functions_.size());
    for (uint32_t i = 0; i < order.size(); ++i) rank[order[i]] = i;

    // Select stacks, then sort them lexicographically by the rank of their frames (in walk
    // order). Stacks sharing a prefix become contiguous, so each tree node is created exactly
    // once, without hashing, and children are created already in deterministic order.
    std::vector<uint32_t> sel;
    sel.reserve(stacks_.size());
    for (uint32_t i = 0; i < stacks_.size(); ++i) {
        const auto& s = stacks_[i];
        if (filter.tid && s.tid != *filter.tid) continue;
        if (filter.module) {
            bool hit = false;
            for (uint32_t f : s.frames) if (functions_[f].module == *filter.module) { hit = true; break; }
            if (!hit) continue;
        }
        sel.push_back(i);
    }
    const bool inv = filter.inverted;
    auto frame_at = [&](const StackEntry& s, size_t d) -> uint32_t {
        return inv ? s.frames[s.frames.size() - 1 - d] : s.frames[d];
    };
    std::sort(sel.begin(), sel.end(), [&](uint32_t a, uint32_t b) {
        const auto& sa = stacks_[a];
        const auto& sb = stacks_[b];
        size_t n = std::min(sa.frames.size(), sb.frames.size());
        for (size_t d = 0; d < n; ++d) {
            uint32_t ra = rank[frame_at(sa, d)], rb = rank[frame_at(sb, d)];
            if (ra != rb) return ra < rb;
        }
        if (sa.frames.size() != sb.frames.size()) return sa.frames.size() < sb.frames.size();
        return a < b;
    });

    std::vector<uint32_t> path;  // path[d] = node at depth d+1 for the current prefix
    const StackEntry* prev = nullptr;
    for (uint32_t si : sel) {
        const auto& s = stacks_[si];
        const size_t n = s.frames.size();
        size_t common = 0;
        if (prev) {
            size_t m = std::min(n, prev->frames.size());
            while (common < m && common < path.size() && frame_at(s, common) == frame_at(*prev, common)) ++common;
        }
        path.resize(common);
        uint32_t cur = common ? path.back() : 0;
        for (size_t d = common; d < n; ++d) {
            uint32_t node = static_cast<uint32_t>(t.nodes.size());
            CallNode cn;
            cn.func = static_cast<int32_t>(frame_at(s, d));
            t.nodes.push_back(cn);
            t.nodes[cur].children.push_back(node);
            path.push_back(node);
            cur = node;
        }
        t.total += s.count;
        t.nodes[0].total += s.count;
        for (uint32_t node : path) t.nodes[node].total += s.count;
        if (n) t.nodes[cur].self += s.count;
        else t.nodes[0].self += s.count;
        prev = &s;
    }
    return t;
}

std::vector<ThreadStats> Profile::threads() const {
    std::map<uint32_t, uint64_t> counts;
    for (const auto& s : stacks_) counts[s.tid] += s.count;
    std::vector<ThreadStats> out;
    for (const auto& [tid, n] : counts) {
        auto it = thread_names_.find(tid);
        out.push_back({tid, it != thread_names_.end() ? it->second : "tid " + std::to_string(tid), n});
    }
    std::sort(out.begin(), out.end(), [](const ThreadStats& a, const ThreadStats& b) {
        return a.samples != b.samples ? a.samples > b.samples : a.tid < b.tid;
    });
    return out;
}

std::vector<ModuleStats> Profile::modules() const {
    std::map<std::string, ModuleStats> m;
    for (const auto& s : stacks_) {
        if (s.frames.empty()) continue;
        m[functions_[s.frames.back()].module].self_samples += s.count;
        std::set<std::string> seen;
        for (uint32_t f : s.frames) seen.insert(functions_[f].module);
        for (const auto& mod : seen) m[mod].total_samples += s.count;
    }
    std::vector<ModuleStats> out;
    for (auto& [name, st] : m) {
        st.name = name;
        out.push_back(st);
    }
    std::sort(out.begin(), out.end(), [](const ModuleStats& a, const ModuleStats& b) {
        return a.self_samples != b.self_samples ? a.self_samples > b.self_samples : a.name < b.name;
    });
    return out;
}

std::vector<uint32_t> Profile::ranked_by_self() const {
    std::vector<uint32_t> idx(functions_.size());
    for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        const auto& fa = functions_[a];
        const auto& fb = functions_[b];
        if (fa.self_samples != fb.self_samples) return fa.self_samples > fb.self_samples;
        if (fa.total_samples != fb.total_samples) return fa.total_samples > fb.total_samples;
        return fa.id < fb.id;
    });
    return idx;
}

std::vector<uint32_t> Profile::ranked_by_total() const {
    std::vector<uint32_t> idx(functions_.size());
    for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        const auto& fa = functions_[a];
        const auto& fb = functions_[b];
        if (fa.total_samples != fb.total_samples) return fa.total_samples > fb.total_samples;
        if (fa.self_samples != fb.self_samples) return fa.self_samples > fb.self_samples;
        return fa.id < fb.id;
    });
    return idx;
}

std::optional<uint32_t> Profile::find_function(const std::string& id) const {
    for (uint32_t i = 0; i < functions_.size(); ++i)
        if (functions_[i].id == id) return i;
    return std::nullopt;
}

}  // namespace detective
