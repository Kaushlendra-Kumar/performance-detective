#include "detective/exporter.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace detective {

namespace {

double share(uint64_t a, uint64_t n) { return n ? static_cast<double>(a) / static_cast<double>(n) : 0.0; }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <typename T>
bool get_opt(const Json& j, const char* key, T* out, std::string* error) {
    if (!j.contains(key) || j[key].is_null()) return true;
    try {
        *out = j[key].get<T>();
        return true;
    } catch (const std::exception&) {
        if (error) *error = std::string("invalid type for field '") + key + "'";
        return false;
    }
}

}  // namespace

Json config_to_json(const SessionConfig& c) {
    Json env = Json::object();
    for (const auto& [k, v] : c.env) env[k] = v;
    return {{"executable", c.executable},
            {"args", c.args},
            {"env", env},
            {"working_dir", c.working_dir},
            {"mode", to_string(c.mode)},
            {"frequency_hz", c.frequency_hz},
            {"timeout_ms", c.timeout_ms},
            {"track_allocations", c.track_allocations},
            {"warmup_runs", c.warmup_runs},
            {"benchmark_runs", c.benchmark_runs},
            {"label", c.label}};
}

std::optional<SessionConfig> config_from_json(const Json& j, std::string* error) {
    if (!j.is_object()) {
        if (error) *error = "request body must be a JSON object";
        return std::nullopt;
    }
    static const char* allowed[] = {"executable", "args", "env", "working_dir", "mode", "frequency_hz",
                                    "timeout_ms", "track_allocations", "warmup_runs", "benchmark_runs", "label"};
    for (const auto& [k, v] : j.items()) {
        bool ok = false;
        for (const char* a : allowed) if (k == a) ok = true;
        if (!ok) {
            if (error) *error = "unknown field '" + k + "'";
            return std::nullopt;
        }
    }
    SessionConfig c;
    std::string mode = "sampling";
    if (!get_opt(j, "executable", &c.executable, error) || !get_opt(j, "args", &c.args, error) ||
        !get_opt(j, "working_dir", &c.working_dir, error) || !get_opt(j, "mode", &mode, error) ||
        !get_opt(j, "frequency_hz", &c.frequency_hz, error) || !get_opt(j, "timeout_ms", &c.timeout_ms, error) ||
        !get_opt(j, "track_allocations", &c.track_allocations, error) ||
        !get_opt(j, "warmup_runs", &c.warmup_runs, error) || !get_opt(j, "benchmark_runs", &c.benchmark_runs, error) ||
        !get_opt(j, "label", &c.label, error))
        return std::nullopt;
    if (j.contains("env") && !j["env"].is_null()) {
        if (!j["env"].is_object()) {
            if (error) *error = "env must be an object of string values";
            return std::nullopt;
        }
        for (const auto& [k, v] : j["env"].items()) {
            if (!v.is_string()) {
                if (error) *error = "env values must be strings";
                return std::nullopt;
            }
            c.env.emplace_back(k, v.get<std::string>());
        }
    }
    auto m = profiling_mode_from_string(mode);
    if (!m) {
        if (error) *error = "mode must be one of: sampling, instrumentation, none";
        return std::nullopt;
    }
    c.mode = *m;
    if (auto err = validate_config(c)) {
        if (error) *error = *err;
        return std::nullopt;
    }
    return c;
}

Json process_to_json(const ProcessResult& p) {
    return {{"pid", p.pid},
            {"started", p.started},
            {"start_error", p.start_error},
            {"exited_normally", p.exited_normally},
            {"exit_code", p.exit_code},
            {"term_signal", p.term_signal},
            {"timed_out", p.timed_out},
            {"cancelled", p.cancelled},
            {"start_unix_ms", p.start_unix_ms},
            {"end_unix_ms", p.end_unix_ms},
            {"wall_seconds", p.wall_seconds},
            {"user_cpu_seconds", p.user_cpu_seconds},
            {"sys_cpu_seconds", p.sys_cpu_seconds},
            {"max_rss_kb", p.max_rss_kb},
            {"voluntary_ctx_switches", p.voluntary_ctx_switches},
            {"involuntary_ctx_switches", p.involuntary_ctx_switches},
            {"stdout", p.stdout_text},
            {"stderr", p.stderr_text},
            {"stdout_truncated", p.stdout_truncated},
            {"stderr_truncated", p.stderr_truncated}};
}

Json summary_to_json(const SessionResult& r) {
    Json s;
    const auto& p = r.process;
    double cpu = p.user_cpu_seconds + p.sys_cpu_seconds;
    s["wall_seconds"] = p.wall_seconds;
    s["cpu_seconds"] = cpu;
    s["avg_cores_busy"] = p.wall_seconds > 0 ? cpu / p.wall_seconds : 0;
    s["max_rss_kb"] = p.max_rss_kb;
    s["exit_code"] = p.exit_code;
    s["findings"] = r.findings.size();
    const auto& c = r.collection;
    s["collection"] = {{"sampling_requested", c.sampling_requested},
                       {"sampling_collected", c.sampling_collected},
                       {"sampling_error", c.sampling_error},
                       {"event", c.event},
                       {"period_ns", c.period_ns},
                       {"frequency_hz", c.frequency_hz},
                       {"raw_samples", c.raw_samples},
                       {"lost_records", c.lost_records},
                       {"warnings", c.warnings},
                       {"instrumentation_error", c.instrumentation_error},
                       {"allocation_error", c.allocation_error},
                       {"analysis_seconds", c.analysis_seconds}};
    Json mods = Json::array();
    for (const auto& m : c.modules)
        mods.push_back({{"path", m.path}, {"loaded", m.loaded}, {"has_symtab", m.has_symtab},
                        {"has_debug_info", m.has_debug_info}, {"error", m.error}, {"debug_file", m.debug_file}});
    s["collection"]["modules"] = mods;
    if (r.profile) {
        const auto& pr = *r.profile;
        uint64_t n = pr.total_samples();
        s["sampling"] = {{"total_samples", n},
                         {"estimated_cpu_seconds", pr.estimated_cpu_seconds()},
                         {"functions", pr.functions().size()},
                         {"distinct_stacks", pr.stacks().size()},
                         {"threads", pr.threads().size()},
                         {"max_stack_depth", pr.max_depth()},
                         {"complete_stack_fraction", share(pr.complete_stacks(), n)},
                         {"unresolved_frame_fraction", share(pr.unresolved_frames(), pr.total_frames())},
                         {"note", "Sample shares estimate on-CPU time; they do not include time spent blocked."}};
        Json top = Json::array();
        auto ranked = pr.ranked_by_self();
        for (size_t i = 0; i < ranked.size() && i < 5; ++i) {
            const auto& f = pr.functions()[ranked[i]];
            top.push_back({{"id", f.id}, {"name", f.name}, {"module", f.module},
                           {"self_share", share(f.self_samples, n)}, {"total_share", share(f.total_samples, n)}});
        }
        s["top_functions"] = top;
    }
    if (r.benchmark) s["benchmark"] = benchmark_to_json(*r.benchmark);
    return s;
}

Json findings_to_json(const std::vector<Finding>& fs) {
    Json arr = Json::array();
    for (const auto& f : fs) {
        Json m = Json::array();
        for (const auto& x : f.measurements) m.push_back({{"name", x.name}, {"value", x.value}, {"unit", x.unit}});
        Json recs = Json::array();
        for (const auto& r : f.recommendations)
            recs.push_back({{"title", r.title},
                            {"observed", r.observed},
                            {"why_it_matters", r.why_it_matters},
                            {"change_to_investigate", r.change_to_investigate},
                            {"expected_benefit", r.expected_benefit},
                            {"tradeoffs", r.tradeoffs},
                            {"how_to_validate", r.how_to_validate},
                            {"would_disprove", r.would_disprove}});
        arr.push_back({{"id", f.id},
                       {"kind", f.kind},
                       {"title", f.title},
                       {"severity", to_string(f.severity)},
                       {"confidence", to_string(f.confidence)},
                       {"score", f.score},
                       {"impact", f.impact},
                       {"source", f.source},
                       {"facts", f.facts},
                       {"interpretation", f.interpretation},
                       {"next_actions", f.next_actions},
                       {"remediation", f.remediation},
                       {"measurements", m},
                       {"affected_functions", f.affected_functions},
                       {"affected_names", f.affected_names},
                       {"limitations", f.limitations},
                       {"recommendations", recs}});
    }
    return arr;
}

Json instrumentation_to_json(const InstrumentationReport& r) {
    Json scopes = Json::array();
    for (const auto& s : r.scopes) {
        Json th = Json::object();
        for (const auto& [tid, c] : s.thread_counts) th[std::to_string(tid)] = c;
        scopes.push_back({{"name", s.name}, {"count", s.count}, {"total_ns", s.total_ns}, {"self_ns", s.self_ns},
                          {"min_ns", s.min_ns}, {"max_ns", s.max_ns}, {"mean_ns", s.mean_ns},
                          {"p50_ns", s.p50_ns}, {"p90_ns", s.p90_ns}, {"p99_ns", s.p99_ns},
                          {"percentiles_valid", s.percentiles_valid}, {"retained_durations", s.retained_durations},
                          {"threads", th}});
    }
    Json edges = Json::array();
    for (const auto& e : r.edges)
        edges.push_back({{"parent", e.parent}, {"child", e.child}, {"count", e.count}, {"total_ns", e.total_ns}});
    return {{"clock", "steady_clock (wall time)"},
            {"threads", r.threads},
            {"clock_overhead_ns", r.clock_overhead_ns},
            {"scopes", scopes},
            {"edges", edges},
            {"marks", r.marks}};
}

Json memory_to_json(const MemoryReport& m) {
    Json sites = Json::array();
    for (const auto& s : m.sites)
        sites.push_back({{"function", s.function}, {"module", s.module}, {"address", s.address},
                         {"count", s.count}, {"bytes", s.bytes}});
    return {{"malloc_calls", m.malloc_calls}, {"calloc_calls", m.calloc_calls}, {"realloc_calls", m.realloc_calls},
            {"memalign_calls", m.memalign_calls}, {"new_calls", m.new_calls}, {"free_calls", m.free_calls},
            {"total_allocations", m.total_allocations}, {"bytes_requested", m.bytes_requested},
            {"peak_live_bytes", m.peak_live_bytes}, {"live_bytes_at_exit", m.live_bytes_at_exit},
            {"peak_error_bytes_per_thread", m.peak_error_bytes_per_thread},
            {"size_histogram", m.size_histogram}, {"sites", sites}, {"sites_overflowed", m.sites_overflowed},
            {"scope_note", m.scope_note},
            {"definitions",
             {{"bytes_requested", "total allocation volume: sum of requested sizes over the run"},
              {"peak_live_bytes", "maximum heap bytes allocated at once (malloc_usable_size accounting)"},
              {"max_rss_kb", "peak resident set size from getrusage (includes code, stacks, allocator overhead)"}}}};
}

namespace {
Json summary_stats(const stats::Summary& s) {
    return {{"n", s.n}, {"mean", s.mean}, {"median", s.median}, {"stddev", s.stddev}, {"mad", s.mad},
            {"min", s.min}, {"max", s.max}, {"p5", s.p5}, {"p95", s.p95},
            {"ci95_low", s.ci95_low}, {"ci95_high", s.ci95_high}};
}
}  // namespace

Json benchmark_to_json(const BenchmarkResult& b) {
    return {{"warmup_runs", b.warmup_runs}, {"failed_runs", b.failed_runs},
            {"wall_seconds", b.wall_seconds}, {"cpu_seconds", b.cpu_seconds},
            {"wall", summary_stats(b.wall)}, {"cpu", summary_stats(b.cpu)},
            {"note", "Unprofiled runs; CI is a bootstrap 95% interval of the median."}};
}

Json comparison_to_json(const ComparisonResult& c) {
    Json metrics = Json::array();
    for (const auto& m : c.metrics)
        metrics.push_back({{"metric", m.metric}, {"n_baseline", m.n_baseline}, {"n_candidate", m.n_candidate},
                           {"baseline_median", m.baseline_median}, {"candidate_median", m.candidate_median},
                           {"baseline_ci", {m.baseline_ci_low, m.baseline_ci_high}},
                           {"candidate_ci", {m.candidate_ci_low, m.candidate_ci_high}},
                           {"absolute_change", m.absolute_change}, {"relative_change", m.relative_change},
                           {"p_value", m.p_value}, {"test_valid", m.test_valid}, {"verdict", m.verdict},
                           {"explanation", m.explanation}});
    Json funcs = Json::array();
    for (const auto& f : c.functions)
        funcs.push_back({{"id", f.id}, {"name", f.name}, {"module", f.module},
                         {"baseline_self_share", f.baseline_self_share}, {"candidate_self_share", f.candidate_self_share},
                         {"baseline_total_share", f.baseline_total_share}, {"candidate_total_share", f.candidate_total_share},
                         {"delta_self_share", f.delta_self_share}, {"p_value", f.p_value}, {"significant", f.significant}});
    return {{"metrics", metrics}, {"functions", funcs}, {"baseline_samples", c.baseline_samples},
            {"candidate_samples", c.candidate_samples}, {"notes", c.notes}};
}

Json functions_to_json(const Profile& p, const FunctionQuery& q) {
    const auto& funcs = p.functions();
    // Optional per-thread view: recompute self/total for one thread.
    std::vector<uint64_t> self(funcs.size(), 0), total(funcs.size(), 0);
    uint64_t n = 0;
    if (q.tid) {
        std::vector<uint32_t> seen_gen(funcs.size(), 0);
        uint32_t gen = 0;
        for (const auto& s : p.stacks()) {
            if (s.tid != *q.tid || s.frames.empty()) continue;
            n += s.count;
            ++gen;
            self[s.frames.back()] += s.count;
            for (uint32_t f : s.frames)
                if (seen_gen[f] != gen) { seen_gen[f] = gen; total[f] += s.count; }
        }
    } else {
        n = p.total_samples();
        for (size_t i = 0; i < funcs.size(); ++i) { self[i] = funcs[i].self_samples; total[i] = funcs[i].total_samples; }
    }
    std::vector<uint32_t> idx;
    std::string needle = lower(q.search);
    for (uint32_t i = 0; i < funcs.size(); ++i) {
        if (total[i] == 0) continue;
        if (!needle.empty() && lower(funcs[i].name).find(needle) == std::string::npos &&
            lower(funcs[i].module).find(needle) == std::string::npos)
            continue;
        idx.push_back(i);
    }
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        if (q.sort == "total" && total[a] != total[b]) return total[a] > total[b];
        if (q.sort == "name" && funcs[a].name != funcs[b].name) return funcs[a].name < funcs[b].name;
        if (self[a] != self[b]) return self[a] > self[b];
        if (total[a] != total[b]) return total[a] > total[b];
        return funcs[a].id < funcs[b].id;
    });
    size_t matched = idx.size();
    if (idx.size() > q.limit) idx.resize(q.limit);
    double period_s = static_cast<double>(p.period_ns()) / 1e9;
    Json rows = Json::array();
    for (uint32_t i : idx) {
        const auto& f = funcs[i];
        rows.push_back({{"id", f.id}, {"name", f.name}, {"module", f.module},
                        {"source_file", f.source_file}, {"source_line", f.source_line}, {"resolved", f.resolved},
                        {"self_samples", self[i]}, {"total_samples", total[i]},
                        {"self_share", share(self[i], n)}, {"total_share", share(total[i], n)},
                        {"self_seconds_est", static_cast<double>(self[i]) * period_s},
                        {"total_seconds_est", static_cast<double>(total[i]) * period_s},
                        {"callers", f.callers.size()}, {"callees", f.callees.size()},
                        {"threads", f.thread_samples.size()}, {"max_recursion_depth", f.max_recursion_depth}});
    }
    return {{"total_samples", n}, {"matched", matched}, {"functions", rows},
            {"units", "samples; *_share = fraction of samples; *_seconds_est = samples x period (estimate)"}};
}

Json function_detail_to_json(const Profile& p, uint32_t index, const std::vector<Finding>& findings) {
    const auto& funcs = p.functions();
    const auto& f = funcs[index];
    uint64_t n = p.total_samples();
    // Caller/callee weights from stacks (adjacent pairs, counted once per stack entry).
    std::map<uint32_t, uint64_t> callers, callees;
    for (const auto& s : p.stacks()) {
        std::set<uint32_t> seen_c, seen_e;
        for (size_t i = 0; i < s.frames.size(); ++i) {
            if (s.frames[i] != index) continue;
            if (i > 0 && seen_c.insert(s.frames[i - 1]).second) callers[s.frames[i - 1]] += s.count;
            if (i + 1 < s.frames.size() && seen_e.insert(s.frames[i + 1]).second) callees[s.frames[i + 1]] += s.count;
        }
    }
    auto list = [&](const std::map<uint32_t, uint64_t>& m) {
        std::vector<std::pair<uint32_t, uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : funcs[a.first].id < funcs[b.first].id;
        });
        Json arr = Json::array();
        for (size_t i = 0; i < v.size() && i < 50; ++i)
            arr.push_back({{"id", funcs[v[i].first].id}, {"name", funcs[v[i].first].name},
                           {"module", funcs[v[i].first].module}, {"samples", v[i].second},
                           {"share", share(v[i].second, n)}});
        return arr;
    };
    Json threads = Json::array();
    auto names = p.thread_names();
    for (const auto& [tid, c] : f.thread_samples)
        threads.push_back({{"tid", tid}, {"name", names.count(tid) ? names[tid] : ""}, {"samples", c}});
    std::vector<Finding> related;
    for (const auto& fd : findings)
        if (std::find(fd.affected_functions.begin(), fd.affected_functions.end(), f.id) != fd.affected_functions.end())
            related.push_back(fd);
    double period_s = static_cast<double>(p.period_ns()) / 1e9;
    return {{"id", f.id}, {"name", f.name}, {"module", f.module}, {"source_file", f.source_file},
            {"source_line", f.source_line}, {"resolved", f.resolved},
            {"category", to_string(classify_function(f.name, f.module))},
            {"self_samples", f.self_samples}, {"total_samples", f.total_samples},
            {"self_share", share(f.self_samples, n)}, {"total_share", share(f.total_samples, n)},
            {"self_seconds_est", static_cast<double>(f.self_samples) * period_s},
            {"total_seconds_est", static_cast<double>(f.total_samples) * period_s},
            {"max_recursion_depth", f.max_recursion_depth},
            {"callers", list(callers)}, {"callees", list(callees)}, {"threads", threads},
            {"related_findings", findings_to_json(related)}};
}

Json threads_to_json(const Profile& p) {
    Json arr = Json::array();
    uint64_t n = p.total_samples();
    double period_s = static_cast<double>(p.period_ns()) / 1e9;
    for (const auto& t : p.threads())
        arr.push_back({{"tid", t.tid}, {"name", t.name}, {"samples", t.samples}, {"share", share(t.samples, n)},
                       {"cpu_seconds_est", static_cast<double>(t.samples) * period_s}});
    return arr;
}

Json modules_to_json(const Profile& p) {
    Json arr = Json::array();
    uint64_t n = p.total_samples();
    for (const auto& m : p.modules())
        arr.push_back({{"name", m.name}, {"self_samples", m.self_samples}, {"total_samples", m.total_samples},
                       {"self_share", share(m.self_samples, n)}, {"total_share", share(m.total_samples, n)}});
    return arr;
}

Json flame_tree_to_json(const Profile& p, const TreeFilter& filter, size_t max_nodes, double min_fraction) {
    CallTree t = p.tree(filter);
    const auto& funcs = p.functions();
    uint64_t min_value = static_cast<uint64_t>(std::ceil(min_fraction * static_cast<double>(t.total)));
    size_t emitted = 0, pruned = 0;
    // Breadth-first so that, under the node cap, wide (important) nodes are kept first.
    Json root = {{"name", "all"}, {"id", ""}, {"module", ""}, {"value", t.total}, {"self", t.nodes[0].self},
                 {"children", Json::array()}};
    struct Item { uint32_t node; Json* out; };
    std::vector<Item> queue{{0, &root}};
    // Store children in a stable container: Json arrays may reallocate, so build per level.
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        Item it = queue[qi];
        auto& kids = (*it.out)["children"];
        std::vector<uint32_t> keep;
        for (uint32_t c : t.nodes[it.node].children) {
            if (t.nodes[c].total < std::max<uint64_t>(1, min_value) || emitted >= max_nodes) { ++pruned; continue; }
            keep.push_back(c);
            ++emitted;
        }
        for (uint32_t c : keep) {
            const auto& f = funcs[static_cast<size_t>(t.nodes[c].func)];
            kids.push_back({{"name", f.name}, {"id", f.id}, {"module", f.module}, {"value", t.nodes[c].total},
                            {"self", t.nodes[c].self}, {"children", Json::array()}});
        }
        for (size_t k = 0; k < keep.size(); ++k) queue.push_back({keep[k], &kids[k]});
    }
    return {{"total", t.total}, {"nodes", emitted + 1}, {"pruned", pruned}, {"inverted", filter.inverted},
            {"min_fraction", min_fraction}, {"root", root}};
}

Json result_to_json(const SessionResult& r) {
    Json j;
    j["process"] = process_to_json(r.process);
    j["summary"] = summary_to_json(r);
    j["findings"] = findings_to_json(r.findings);
    if (r.profile) {
        const auto& p = *r.profile;
        Json funcs = Json::array();
        for (const auto& f : p.functions())
            funcs.push_back({{"id", f.id}, {"name", f.name}, {"module", f.module}, {"source_file", f.source_file},
                             {"source_line", f.source_line}, {"resolved", f.resolved}});
        Json stacks = Json::array();
        for (const auto& s : p.stacks()) stacks.push_back({s.tid, s.count, s.frames});
        Json names = Json::object();
        for (const auto& [tid, n] : p.thread_names()) names[std::to_string(tid)] = n;
        j["profile"] = {{"period_ns", p.period_ns()}, {"functions", funcs}, {"stacks", stacks}, {"thread_names", names}};
    }
    if (r.instrumentation) j["instrumentation"] = instrumentation_to_json(*r.instrumentation);
    if (r.memory) j["memory"] = memory_to_json(*r.memory);
    if (r.benchmark) j["benchmark"] = benchmark_to_json(*r.benchmark);
    return j;
}

std::optional<SessionResult> result_from_json(const Json& j, std::string* error) {
    try {
        SessionResult r;
        const auto& p = j.at("process");
        auto& pr = r.process;
        pr.pid = p.value("pid", -1);
        pr.started = p.value("started", false);
        pr.start_error = p.value("start_error", "");
        pr.exited_normally = p.value("exited_normally", false);
        pr.exit_code = p.value("exit_code", -1);
        pr.term_signal = p.value("term_signal", 0);
        pr.timed_out = p.value("timed_out", false);
        pr.cancelled = p.value("cancelled", false);
        pr.start_unix_ms = p.value("start_unix_ms", 0LL);
        pr.end_unix_ms = p.value("end_unix_ms", 0LL);
        pr.wall_seconds = p.value("wall_seconds", 0.0);
        pr.user_cpu_seconds = p.value("user_cpu_seconds", 0.0);
        pr.sys_cpu_seconds = p.value("sys_cpu_seconds", 0.0);
        pr.max_rss_kb = p.value("max_rss_kb", 0L);
        pr.voluntary_ctx_switches = p.value("voluntary_ctx_switches", 0L);
        pr.involuntary_ctx_switches = p.value("involuntary_ctx_switches", 0L);
        pr.stdout_text = p.value("stdout", "");
        pr.stderr_text = p.value("stderr", "");
        pr.stdout_truncated = p.value("stdout_truncated", false);
        pr.stderr_truncated = p.value("stderr_truncated", false);

        if (j.contains("summary") && j["summary"].contains("collection")) {
            const auto& c = j["summary"]["collection"];
            auto& rc = r.collection;
            rc.sampling_requested = c.value("sampling_requested", false);
            rc.sampling_collected = c.value("sampling_collected", false);
            rc.sampling_error = c.value("sampling_error", "");
            rc.event = c.value("event", "");
            rc.period_ns = c.value("period_ns", 0ULL);
            rc.frequency_hz = c.value("frequency_hz", 0);
            rc.raw_samples = c.value("raw_samples", 0ULL);
            rc.lost_records = c.value("lost_records", 0ULL);
            rc.warnings = c.value("warnings", std::vector<std::string>{});
            rc.instrumentation_error = c.value("instrumentation_error", "");
            rc.allocation_error = c.value("allocation_error", "");
            rc.analysis_seconds = c.value("analysis_seconds", 0.0);
            for (const auto& m : c.value("modules", Json::array()))
                rc.modules.push_back({m.value("path", ""), m.value("loaded", false), m.value("has_symtab", false),
                                      m.value("has_debug_info", false), m.value("error", ""),
                                      m.value("debug_file", "")});
        }
        if (j.contains("profile") && !j["profile"].is_null()) {
            const auto& jp = j["profile"];
            std::vector<FunctionStats> funcs;
            for (const auto& f : jp.at("functions")) {
                FunctionStats fs;
                fs.id = f.value("id", "");
                fs.name = f.at("name").get<std::string>();
                fs.module = f.value("module", "");
                fs.source_file = f.value("source_file", "");
                fs.source_line = f.value("source_line", 0U);
                fs.resolved = f.value("resolved", false);
                funcs.push_back(std::move(fs));
            }
            if (funcs.size() > 5'000'000) throw std::runtime_error("too many functions");
            std::vector<StackEntry> stacks;
            for (const auto& s : jp.at("stacks")) {
                StackEntry e;
                e.tid = s.at(0).get<uint32_t>();
                e.count = s.at(1).get<uint64_t>();
                e.frames = s.at(2).get<std::vector<uint32_t>>();
                stacks.push_back(std::move(e));
            }
            std::map<uint32_t, std::string> names;
            const auto thread_names_obj = jp.value("thread_names", Json::object());
            for (const auto& [k, v] : thread_names_obj.items())
                names[static_cast<uint32_t>(std::stoul(k))] = v.get<std::string>();
            r.profile = Profile::from_parts(std::move(funcs), std::move(stacks), std::move(names),
                                            jp.value("period_ns", 0ULL));
        }
        if (j.contains("instrumentation")) {
            const auto& ji = j["instrumentation"];
            InstrumentationReport ir;
            ir.threads = ji.value("threads", 0ULL);
            ir.clock_overhead_ns = ji.value("clock_overhead_ns", 0.0);
            for (const auto& s : ji.at("scopes")) {
                ScopeStats st;
                st.name = s.at("name").get<std::string>();
                st.count = s.value("count", 0ULL);
                st.total_ns = s.value("total_ns", 0.0);
                st.self_ns = s.value("self_ns", 0.0);
                st.min_ns = s.value("min_ns", 0.0);
                st.max_ns = s.value("max_ns", 0.0);
                st.mean_ns = s.value("mean_ns", 0.0);
                st.p50_ns = s.value("p50_ns", 0.0);
                st.p90_ns = s.value("p90_ns", 0.0);
                st.p99_ns = s.value("p99_ns", 0.0);
                st.percentiles_valid = s.value("percentiles_valid", false);
                st.retained_durations = s.value("retained_durations", 0ULL);
                const auto threads_obj = s.value("threads", Json::object());
                for (const auto& [tid, c] : threads_obj.items())
                    st.thread_counts[static_cast<uint32_t>(std::stoul(tid))] = c.get<uint64_t>();
                ir.scopes.push_back(std::move(st));
            }
            for (const auto& e : ji.value("edges", Json::array()))
                ir.edges.push_back({e.at("parent").get<std::string>(), e.at("child").get<std::string>(),
                                    e.value("count", 0ULL), e.value("total_ns", 0.0)});
            const auto marks_obj = ji.value("marks", Json::object());
            for (const auto& [k, v] : marks_obj.items()) ir.marks[k] = v.get<uint64_t>();
            r.instrumentation = std::move(ir);
        }
        if (j.contains("memory")) {
            const auto& jm = j["memory"];
            MemoryReport m;
            m.malloc_calls = jm.value("malloc_calls", 0ULL);
            m.calloc_calls = jm.value("calloc_calls", 0ULL);
            m.realloc_calls = jm.value("realloc_calls", 0ULL);
            m.memalign_calls = jm.value("memalign_calls", 0ULL);
            m.new_calls = jm.value("new_calls", 0ULL);
            m.free_calls = jm.value("free_calls", 0ULL);
            m.total_allocations = jm.value("total_allocations", 0ULL);
            m.bytes_requested = jm.value("bytes_requested", 0ULL);
            m.peak_live_bytes = jm.value("peak_live_bytes", 0ULL);
            m.live_bytes_at_exit = jm.value("live_bytes_at_exit", 0ULL);
            m.peak_error_bytes_per_thread = jm.value("peak_error_bytes_per_thread", 0ULL);
            m.size_histogram = jm.value("size_histogram", std::vector<uint64_t>{});
            m.sites_overflowed = jm.value("sites_overflowed", 0ULL);
            m.scope_note = jm.value("scope_note", "");
            for (const auto& s : jm.value("sites", Json::array()))
                m.sites.push_back({s.value("address", 0ULL), s.value("function", ""), s.value("module", ""),
                                   s.value("count", 0ULL), s.value("bytes", 0ULL)});
            r.memory = std::move(m);
        }
        if (j.contains("benchmark")) {
            const auto& jb = j["benchmark"];
            BenchmarkResult b;
            b.warmup_runs = jb.value("warmup_runs", 0);
            b.failed_runs = jb.value("failed_runs", 0);
            b.wall_seconds = jb.value("wall_seconds", std::vector<double>{});
            b.cpu_seconds = jb.value("cpu_seconds", std::vector<double>{});
            b.wall = stats::summarize(b.wall_seconds);
            b.cpu = stats::summarize(b.cpu_seconds);
            r.benchmark = std::move(b);
        }
        return r;
    } catch (const std::exception& e) {
        if (error) *error = std::string("invalid session data: ") + e.what();
        return std::nullopt;
    }
}

Json session_to_json(const Session& s, bool include_full_result) {
    Json j = {{"id", s.id()},
              {"state", to_string(s.state())},
              {"message", s.message()},
              {"created_ms", s.created_ms()},
              {"pid", s.pid()},
              {"demo", s.demo},
              {"imported", s.imported},
              {"config", config_to_json(s.config())}};
    if (auto r = s.result()) {
        if (include_full_result) {
            j["result"] = result_to_json(*r);
        } else {
            j["summary"] = summary_to_json(*r);
            j["process"] = process_to_json(r->process);
            j["process"].erase("stdout");
            j["process"].erase("stderr");
            Json top = Json::array();
            for (size_t i = 0; i < r->findings.size() && i < 3; ++i)
                top.push_back({{"id", r->findings[i].id}, {"title", r->findings[i].title},
                               {"severity", to_string(r->findings[i].severity)}, {"score", r->findings[i].score}});
            j["top_findings"] = top;
        }
    }
    return j;
}

std::string functions_csv(const Profile& p) {
    auto q = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"') o += "\"\"";
            else o += c;
        }
        // Neutralize spreadsheet formula injection.
        if (o.size() > 1 && (o[1] == '=' || o[1] == '+' || o[1] == '-' || o[1] == '@')) o.insert(1, "'");
        return o + "\"";
    };
    std::ostringstream s;
    s << "id,function,module,source,self_samples,total_samples,self_share,total_share,self_seconds_est,"
         "total_seconds_est,max_recursion_depth\n";
    uint64_t n = p.total_samples();
    double period = static_cast<double>(p.period_ns()) / 1e9;
    for (uint32_t i : p.ranked_by_self()) {
        const auto& f = p.functions()[i];
        if (f.total_samples == 0) continue;
        s << f.id << ',' << q(f.name) << ',' << q(f.module) << ','
          << q(f.source_file.empty() ? "" : f.source_file + ":" + std::to_string(f.source_line)) << ','
          << f.self_samples << ',' << f.total_samples << ',' << share(f.self_samples, n) << ','
          << share(f.total_samples, n) << ',' << static_cast<double>(f.self_samples) * period << ','
          << static_cast<double>(f.total_samples) * period << ',' << f.max_recursion_depth << '\n';
    }
    return s.str();
}

}  // namespace detective
