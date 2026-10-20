// `detective` command-line interface.
//
//   detective capabilities
//   detective record [options] -- <executable> [args...]
//   detective report <session.json> [--top N]
//   detective compare <baseline.json> <candidate.json>
//   detective overhead [--runs N] [--freq HZ] -- <executable> [args...]
//   detective serve [--port P] [--host H] [--allow-dir DIR]... [--data-dir DIR] [--frontend DIR]
//   detective demo-data --out FILE
#include "detective/api_server.hpp"
#include "detective/comparison.hpp"
#include "detective/exporter.hpp"
#include "detective/flame_graph.hpp"
#include "detective/process_runner.hpp"
#include "detective/profiler.hpp"
#include "detective/sampler.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef DETECTIVE_DEFAULT_FRONTEND
#define DETECTIVE_DEFAULT_FRONTEND ""
#endif
#ifndef DETECTIVE_DEFAULT_ALLOC_LIB
#define DETECTIVE_DEFAULT_ALLOC_LIB ""
#endif
#ifndef DETECTIVE_DEFAULT_EXAMPLES
#define DETECTIVE_DEFAULT_EXAMPLES ""
#endif

using namespace detective;

namespace {

int usage() {
    std::fprintf(stderr,
                 "Performance Detective — C++ sampling profiler and bottleneck analyzer\n\n"
                 "usage:\n"
                 "  detective capabilities\n"
                 "  detective record [--freq HZ] [--mode sampling|instrumentation|none] [--alloc]\n"
                 "                   [--runs N] [--warmup N] [--timeout SEC] [--out FILE.json]\n"
                 "                   [--svg FILE.svg] [--csv FILE.csv] [--top N] -- EXE [ARGS...]\n"
                 "  detective report SESSION.json [--top N]\n"
                 "  detective compare BASELINE.json CANDIDATE.json\n"
                 "  detective overhead [--runs N] [--freq HZ] -- EXE [ARGS...]\n"
                 "  detective serve [--port P] [--host H] [--allow-dir DIR]... [--data-dir DIR]\n"
                 "                  [--frontend DIR] [--workers N]\n"
                 "  detective demo-data --out FILE.json\n");
    return 2;
}

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

std::string pct(double f) {
    char b[32];
    std::snprintf(b, sizeof b, "%5.1f%%", f * 100);
    return b;
}

std::string fmt_us(double ns) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", ns / 1e3);
    return b;
}

std::string trunc(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + "…" : s; }

std::string dump(const Json& j, int indent = -1) { return j.dump(indent, ' ', false, Json::error_handler_t::replace); }

void print_report(const SessionResult& r, size_t top) {
    const auto& p = r.process;
    std::printf("\n== Process ==\n");
    if (!p.started) {
        std::printf("  failed to start: %s\n", p.start_error.c_str());
        return;
    }
    std::printf("  pid %d  exit %s  wall %.3f s  cpu %.3f s (user %.3f, sys %.3f)  avg cores busy %.2f\n", p.pid,
                p.exited_normally ? std::to_string(p.exit_code).c_str()
                                  : ("signal " + std::to_string(p.term_signal)).c_str(),
                p.wall_seconds, p.user_cpu_seconds + p.sys_cpu_seconds, p.user_cpu_seconds, p.sys_cpu_seconds,
                p.wall_seconds > 0 ? (p.user_cpu_seconds + p.sys_cpu_seconds) / p.wall_seconds : 0.0);
    std::printf("  max RSS %ld KiB  context switches: %ld voluntary, %ld involuntary%s%s\n", p.max_rss_kb,
                p.voluntary_ctx_switches, p.involuntary_ctx_switches, p.timed_out ? "  [TIMED OUT]" : "",
                p.cancelled ? "  [CANCELLED]" : "");
    if (!p.stdout_text.empty()) std::printf("  stdout: %s", trunc(p.stdout_text, 300).c_str());
    if (!p.stdout_text.empty() && p.stdout_text.back() != '\n') std::printf("\n");

    if (r.benchmark) {
        const auto& b = *r.benchmark;
        std::printf("\n== Benchmark (%zu unprofiled runs, %d warm-up, %d failed) ==\n", b.wall_seconds.size(),
                    b.warmup_runs, b.failed_runs);
        std::printf("  wall median %.4f s  [95%% CI %.4f – %.4f]  MAD %.4f  min %.4f  max %.4f\n", b.wall.median,
                    b.wall.ci95_low, b.wall.ci95_high, b.wall.mad, b.wall.min, b.wall.max);
        std::printf("  cpu  median %.4f s  [95%% CI %.4f – %.4f]\n", b.cpu.median, b.cpu.ci95_low, b.cpu.ci95_high);
    }

    const auto& c = r.collection;
    if (c.sampling_requested) {
        std::printf("\n== Sampling (%s, %d Hz, period %.3f ms) ==\n", c.event.c_str(), c.frequency_hz,
                    static_cast<double>(c.period_ns) / 1e6);
        if (!c.sampling_collected) std::printf("  NOT COLLECTED: %s\n", c.sampling_error.c_str());
        for (const auto& w : c.warnings) std::printf("  warning: %s\n", w.c_str());
    }
    if (r.profile) {
        const auto& pr = *r.profile;
        uint64_t n = pr.total_samples();
        std::printf("  %llu samples (~%.3f s CPU, estimate), %zu functions, %zu threads, max depth %u, "
                    "complete stacks %.1f%%\n",
                    static_cast<unsigned long long>(n), pr.estimated_cpu_seconds(), pr.functions().size(),
                    pr.threads().size(), pr.max_depth(),
                    n ? 100.0 * static_cast<double>(pr.complete_stacks()) / static_cast<double>(n) : 0.0);
        std::printf("\n  %-7s %-7s %-9s  %s\n", "self", "total", "samples", "function  [module]  (source)");
        auto ranked = pr.ranked_by_self();
        for (size_t i = 0; i < ranked.size() && i < top; ++i) {
            const auto& f = pr.functions()[ranked[i]];
            if (f.self_samples == 0) break;
            std::string src = f.source_file.empty() ? "" : "  (" + f.source_file.substr(f.source_file.rfind('/') + 1) +
                                                              ":" + std::to_string(f.source_line) + ")";
            std::printf("  %s %s %-9llu  %s  [%s]%s\n",
                        pct(static_cast<double>(f.self_samples) / static_cast<double>(n)).c_str(),
                        pct(static_cast<double>(f.total_samples) / static_cast<double>(n)).c_str(),
                        static_cast<unsigned long long>(f.self_samples), trunc(f.name, 70).c_str(), f.module.c_str(),
                        src.c_str());
        }
        if (pr.threads().size() > 1) {
            std::printf("\n  threads:");
            for (const auto& t : pr.threads())
                std::printf(" %s(%u)=%s", t.name.c_str(), t.tid,
                            pct(static_cast<double>(t.samples) / static_cast<double>(n)).c_str());
            std::printf("\n");
        }
    }
    if (r.instrumentation) {
        std::printf("\n== Instrumentation (wall-clock, %llu threads, clock overhead ~%.0f ns/scope) ==\n",
                    static_cast<unsigned long long>(r.instrumentation->threads), r.instrumentation->clock_overhead_ns);
        std::printf("  %-28s %10s %12s %12s %10s %10s\n", "scope", "calls", "total ms", "self ms", "mean µs", "p99 µs");
        for (const auto& s : r.instrumentation->scopes)
            std::printf("  %-28s %10llu %12.2f %12.2f %10.2f %10s\n", trunc(s.name, 28).c_str(),
                        static_cast<unsigned long long>(s.count), s.total_ns / 1e6, s.self_ns / 1e6, s.mean_ns / 1e3,
                        s.p99_ns >= 0 && s.percentiles_valid ? fmt_us(s.p99_ns).c_str() : "n/a");
        for (const auto& [k, v] : r.instrumentation->marks)
            std::printf("  mark %-23s %10llu\n", k.c_str(), static_cast<unsigned long long>(v));
    } else if (!c.instrumentation_error.empty()) {
        std::printf("\n== Instrumentation ==\n  unavailable: %s\n", c.instrumentation_error.c_str());
    }
    if (r.memory) {
        const auto& m = *r.memory;
        std::printf("\n== Allocations (LD_PRELOAD tracker) ==\n");
        std::printf("  %llu allocations (malloc %llu, new %llu, calloc %llu, realloc %llu), %llu frees\n",
                    static_cast<unsigned long long>(m.total_allocations), static_cast<unsigned long long>(m.malloc_calls),
                    static_cast<unsigned long long>(m.new_calls), static_cast<unsigned long long>(m.calloc_calls),
                    static_cast<unsigned long long>(m.realloc_calls), static_cast<unsigned long long>(m.free_calls));
        std::printf("  bytes requested %.2f MiB, peak live heap %.2f MiB, max RSS %.2f MiB\n",
                    static_cast<double>(m.bytes_requested) / 1048576.0, static_cast<double>(m.peak_live_bytes) / 1048576.0,
                    static_cast<double>(p.max_rss_kb) / 1024.0);
        for (size_t i = 0; i < m.sites.size() && i < 5; ++i)
            std::printf("  site %-60s %10llu allocs %10.2f MiB\n", trunc(m.sites[i].function, 60).c_str(),
                        static_cast<unsigned long long>(m.sites[i].count),
                        static_cast<double>(m.sites[i].bytes) / 1048576.0);
    } else if (!c.allocation_error.empty()) {
        std::printf("\n== Allocations ==\n  unavailable: %s\n", c.allocation_error.c_str());
    }

    std::printf("\n== Findings (%zu) ==\n", r.findings.size());
    for (const auto& f : r.findings) {
        std::printf("\n  [%s | confidence %s | score %.1f] %s\n", to_string(f.severity), to_string(f.confidence), f.score,
                    f.title.c_str());
        for (const auto& fact : f.facts) std::printf("    fact: %s\n", fact.c_str());
        if (!f.interpretation.empty()) std::printf("    interpretation: %s\n", f.interpretation.c_str());
        for (const auto& a : f.next_actions) std::printf("    next: %s\n", a.c_str());
        for (const auto& rec : f.recommendations) std::printf("    recommendation: %s\n", rec.title.c_str());
    }
    if (r.findings.empty()) std::printf("  No credible bottleneck was identified from the collected evidence.\n");
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
    return static_cast<bool>(out);
}

std::optional<SessionResult> load_session_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return std::nullopt;
    }
    try {
        Json j = Json::parse(in);
        std::string err;
        auto r = result_from_json(j.contains("result") ? j["result"] : j, &err);
        if (!r) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return std::nullopt;
        }
        analyze_result(*r);
        return r;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
        return std::nullopt;
    }
}

int cmd_capabilities() {
    auto c = probe_sampler_capabilities();
    std::printf("%s\n", c.summary.c_str());
    std::printf("running as root: %s\n", c.running_as_root ? "yes" : "no");
    std::printf("allocation tracker: %s\n", env_or("DETECTIVE_ALLOC_LIB", DETECTIVE_DEFAULT_ALLOC_LIB).c_str());
    return c.perf_event_open_available ? 0 : 1;
}

int cmd_record(int argc, char** argv) {
    SessionConfig cfg;
    std::string out, svg, csv;
    size_t top = 15;
    int i = 0;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--") { ++i; break; }
        if (a == "--freq") cfg.frequency_hz = std::atoi(next().c_str());
        else if (a == "--mode") {
            auto m = profiling_mode_from_string(next());
            if (!m) return usage();
            cfg.mode = *m;
        } else if (a == "--alloc") cfg.track_allocations = true;
        else if (a == "--runs") cfg.benchmark_runs = std::atoi(next().c_str());
        else if (a == "--warmup") cfg.warmup_runs = std::atoi(next().c_str());
        else if (a == "--timeout") cfg.timeout_ms = std::atoi(next().c_str()) * 1000;
        else if (a == "--out") out = next();
        else if (a == "--svg") svg = next();
        else if (a == "--csv") csv = next();
        else if (a == "--top") top = static_cast<size_t>(std::atoi(next().c_str()));
        else if (a == "--label") cfg.label = next();
        else return usage();
    }
    if (i >= argc) return usage();
    cfg.executable = argv[i++];
    for (; i < argc; ++i) cfg.args.push_back(argv[i]);
    if (auto err = validate_config(cfg)) {
        std::fprintf(stderr, "invalid configuration: %s\n", err->c_str());
        return 2;
    }
    ProfilerOptions opt;
    opt.alloc_lib_path = env_or("DETECTIVE_ALLOC_LIB", DETECTIVE_DEFAULT_ALLOC_LIB);
    opt.scratch_dir = env_or("TMPDIR", "/tmp");
    RunHooks hooks;
    hooks.on_state = [](SessionState s, const std::string&) { std::fprintf(stderr, "[detective] %s\n", to_string(s)); };
    auto r = run_profile(cfg, opt, nullptr, hooks);
    print_report(*r, top);
    struct rusage self_ru{};
    ::getrusage(RUSAGE_SELF, &self_ru);
    std::printf("\n(profiler engine peak RSS: %.1f MiB; analysis took %.1f ms)\n", self_ru.ru_maxrss / 1024.0,
                r->collection.analysis_seconds * 1e3);
    if (!out.empty()) {
        Session s(generate_session_id(), cfg, 0);
        s.restore(SessionState::Completed, "recorded with CLI");
        s.set_result(r);
        if (!write_file(out, dump(session_to_json(s, true)))) std::fprintf(stderr, "failed to write %s\n", out.c_str());
        else std::printf("\nsession written to %s\n", out.c_str());
    }
    if (!svg.empty() && r->profile) {
        FlameSvgOptions o;
        o.title = "Performance Detective — " + cfg.executable.substr(cfg.executable.rfind('/') + 1);
        o.subtitle = std::to_string(r->profile->total_samples()) + " cpu-clock samples (estimates)";
        write_file(svg, render_flame_svg(*r->profile, r->profile->tree(), o));
        std::printf("flame graph written to %s\n", svg.c_str());
    }
    if (!csv.empty() && r->profile) {
        write_file(csv, functions_csv(*r->profile));
        std::printf("function table written to %s\n", csv.c_str());
    }
    if (!r->process.started) return 1;
    if (r->collection.sampling_requested && !r->collection.sampling_collected) return 3;
    return 0;
}

int cmd_report(int argc, char** argv) {
    if (argc < 1) return usage();
    size_t top = 20;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--top") && i + 1 < argc) top = static_cast<size_t>(std::atoi(argv[++i]));
    auto r = load_session_file(argv[0]);
    if (!r) return 1;
    print_report(*r, top);
    return 0;
}

int cmd_compare(int argc, char** argv) {
    if (argc < 2) return usage();
    auto a = load_session_file(argv[0]);
    auto b = load_session_file(argv[1]);
    if (!a || !b) return 1;
    auto c = compare_results(*a, *b);
    std::printf("== Timing ==\n");
    for (const auto& m : c.metrics) {
        std::printf("  %-13s baseline %.4f s (n=%zu)  candidate %.4f s (n=%zu)  change %+.4f s (%+.1f%%)\n    -> %s: %s\n",
                    m.metric.c_str(), m.baseline_median, m.n_baseline, m.candidate_median, m.n_candidate,
                    m.absolute_change, m.relative_change * 100, m.verdict.c_str(), m.explanation.c_str());
    }
    std::printf("\n== Functions (self share, percentage points) ==\n");
    for (size_t i = 0; i < c.functions.size() && i < 15; ++i) {
        const auto& f = c.functions[i];
        std::printf("  %s → %s  Δ %+6.1f pp %s  %s\n", pct(f.baseline_self_share).c_str(),
                    pct(f.candidate_self_share).c_str(), f.delta_self_share * 100, f.significant ? "*" : " ",
                    trunc(f.name, 70).c_str());
    }
    for (const auto& n : c.notes) std::printf("  note: %s\n", n.c_str());
    return 0;
}

int cmd_overhead(int argc, char** argv) {
    int runs = 7, freq = 999;
    int i = 0;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { ++i; break; }
        if (a == "--runs" && i + 1 < argc) runs = std::atoi(argv[++i]);
        else if (a == "--freq" && i + 1 < argc) freq = std::atoi(argv[++i]);
        else return usage();
    }
    if (i >= argc) return usage();
    std::string exe;
    if (auto err = validate_executable(argv[i], &exe)) {
        std::fprintf(stderr, "%s\n", err->c_str());
        return 1;
    }
    ProcessSpec spec;
    spec.executable = exe;
    for (int k = i + 1; k < argc; ++k) spec.args.push_back(argv[k]);
    ProcessRunner runner;
    std::vector<double> base, prof;
    runner.run(spec);  // warm-up (page cache)
    for (int k = 0; k < runs; ++k) {
        // Interleave to spread drift over both arms.
        auto r0 = runner.run(spec);
        SamplerConfig sc;
        sc.frequency_hz = freq;
        PerfSampler sampler(sc);
        auto r1 = runner.run(spec, &sampler);
        auto raw = sampler.take_profile();
        if (r0.exited_normally) base.push_back(r0.wall_seconds);
        if (r1.exited_normally && raw.collected) prof.push_back(r1.wall_seconds);
        std::fprintf(stderr, "run %d: unprofiled %.4f s, sampled %.4f s (%zu samples)\n", k + 1, r0.wall_seconds,
                     r1.wall_seconds, raw.samples.size());
    }
    auto sa = stats::summarize(base), sb = stats::summarize(prof);
    auto mw = stats::mann_whitney(base, prof);
    std::printf("unprofiled median %.4f s [%.4f, %.4f]; sampled@%dHz median %.4f s [%.4f, %.4f]\n", sa.median,
                sa.ci95_low, sa.ci95_high, freq, sb.median, sb.ci95_low, sb.ci95_high);
    std::printf("overhead %+.2f%% (Mann-Whitney p=%.3g, n=%zu/%zu)\n",
                sa.median > 0 ? (sb.median / sa.median - 1) * 100 : 0.0, mw.p_value, base.size(), prof.size());
    return 0;
}

ApiServer* g_server = nullptr;
void on_signal(int) {
    if (g_server) g_server->stop();
}

int cmd_serve(int argc, char** argv) {
    ServerConfig cfg;
    cfg.frontend_dir = DETECTIVE_DEFAULT_FRONTEND;
    cfg.profiler.alloc_lib_path = env_or("DETECTIVE_ALLOC_LIB", DETECTIVE_DEFAULT_ALLOC_LIB);
    cfg.profiler.scratch_dir = env_or("TMPDIR", "/tmp");
    cfg.data_dir = "detective-data";
    bool default_dirs = true;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") cfg.port = std::atoi(next().c_str());
        else if (a == "--host") cfg.host = next();
        else if (a == "--allow-dir") { cfg.allowed_dirs.push_back(next()); default_dirs = false; }
        else if (a == "--data-dir") cfg.data_dir = next();
        else if (a == "--no-persist") cfg.data_dir.clear();
        else if (a == "--frontend") cfg.frontend_dir = next();
        else if (a == "--workers") cfg.workers = static_cast<size_t>(std::max(1, std::atoi(next().c_str())));
        else return usage();
    }
    if (default_dirs && std::strlen(DETECTIVE_DEFAULT_EXAMPLES) > 0) cfg.allowed_dirs.push_back(DETECTIVE_DEFAULT_EXAMPLES);
    if (cfg.host != "127.0.0.1" && cfg.host != "localhost" && cfg.host != "::1")
        std::fprintf(stderr, "WARNING: binding to %s. The API launches programs and must not be exposed to "
                             "untrusted networks.\n", cfg.host.c_str());
    if (::geteuid() == 0)
        std::fprintf(stderr, "WARNING: running as root; profiled targets inherit root privileges. Prefer an "
                             "unprivileged user (see docs/linux-permissions.md).\n");
    ApiServer server(cfg);
    server.sessions().load_persisted();
    if (!server.bind()) {
        std::fprintf(stderr, "failed to bind %s:%d\n", cfg.host.c_str(), cfg.port);
        return 1;
    }
    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("Performance Detective listening on http://%s:%d\n", cfg.host.c_str(), server.port());
    for (const auto& d : server.sessions().config().allowed_dirs) std::printf("  allowed target dir: %s\n", d.c_str());
    std::fflush(stdout);
    server.listen();
    g_server = nullptr;
    return 0;
}

// Deterministic, explicitly labeled synthetic data for the UI demo mode.
int cmd_demo_data(int argc, char** argv) {
    std::string out;
    for (int i = 0; i < argc; ++i)
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    if (out.empty()) return usage();
    const char* names[] = {"_start", "__libc_start_call_main", "main", "ImagePipeline::run", "load_tiles",
                           "decode_png", "inflate_block", "apply_filters", "gaussian_blur", "convolve_row",
                           "sharpen", "encode_jpeg", "dct_8x8", "huffman_encode", "write_output", "malloc",
                           "free", "memcpy", "std::vector<float>::_M_realloc_insert", "start_thread",
                           "worker_loop", "pthread_mutex_lock"};
    std::vector<FunctionStats> funcs;
    for (const char* n : names) {
        FunctionStats f;
        f.name = n;
        bool lib = !std::strcmp(n, "malloc") || !std::strcmp(n, "free") || !std::strcmp(n, "memcpy") ||
                   !std::strcmp(n, "pthread_mutex_lock") || !std::strcmp(n, "start_thread") ||
                   !std::strcmp(n, "__libc_start_call_main");
        f.module = lib ? "libc.so.6" : "demo_pipeline";
        f.resolved = true;
        f.source_file = lib ? "" : "demo/pipeline.cpp";
        f.source_line = lib ? 0 : static_cast<uint32_t>(10 + funcs.size() * 17);
        funcs.push_back(f);
    }
    auto idx = [&](const char* n) {
        for (uint32_t i = 0; i < funcs.size(); ++i) if (funcs[i].name == n) return i;
        return 0u;
    };
    std::vector<StackEntry> stacks;
    auto add = [&](uint32_t tid, uint64_t count, std::initializer_list<const char*> path) {
        StackEntry e;
        e.tid = tid;
        e.count = count;
        for (const char* p : path) e.frames.push_back(idx(p));
        stacks.push_back(e);
    };
    const char* base[] = {"_start", "__libc_start_call_main", "main", "ImagePipeline::run"};
    auto main_path = [&](std::initializer_list<const char*> rest) {
        std::vector<const char*> v(std::begin(base), std::end(base));
        v.insert(v.end(), rest.begin(), rest.end());
        return v;
    };
    auto addv = [&](uint32_t tid, uint64_t count, const std::vector<const char*>& v) {
        StackEntry e;
        e.tid = tid;
        e.count = count;
        for (const char* p : v) e.frames.push_back(idx(p));
        stacks.push_back(e);
    };
    addv(1000, 4100, main_path({"apply_filters", "gaussian_blur", "convolve_row"}));
    addv(1000, 600, main_path({"apply_filters", "gaussian_blur"}));
    addv(1000, 900, main_path({"apply_filters", "sharpen", "convolve_row"}));
    addv(1000, 700, main_path({"load_tiles", "decode_png", "inflate_block"}));
    addv(1000, 350, main_path({"load_tiles", "decode_png", "std::vector<float>::_M_realloc_insert", "malloc"}));
    addv(1000, 200, main_path({"load_tiles", "decode_png", "std::vector<float>::_M_realloc_insert", "memcpy"}));
    addv(1000, 480, main_path({"encode_jpeg", "dct_8x8"}));
    addv(1000, 260, main_path({"encode_jpeg", "huffman_encode"}));
    addv(1000, 90, main_path({"write_output"}));
    addv(1000, 120, main_path({"apply_filters", "free"}));
    add(1001, 820, {"start_thread", "worker_loop", "convolve_row"});
    add(1001, 140, {"start_thread", "worker_loop", "pthread_mutex_lock"});
    std::map<uint32_t, std::string> tn = {{1000, "demo_pipeline"}, {1001, "demo-worker"}};
    SessionResult r;
    r.profile = Profile::from_parts(funcs, stacks, tn, 1'001'001);
    r.process.started = true;
    r.process.exited_normally = true;
    r.process.exit_code = 0;
    r.process.pid = 0;
    r.process.wall_seconds = 7.9;
    r.process.user_cpu_seconds = 8.6;
    r.process.sys_cpu_seconds = 0.2;
    r.process.max_rss_kb = 182000;
    r.process.stdout_text = "DEMO DATA — synthetic, not a real measurement\n";
    r.collection.sampling_requested = r.collection.sampling_collected = true;
    r.collection.event = "synthetic (demo)";
    r.collection.period_ns = 1'001'001;
    r.collection.frequency_hz = 999;
    r.collection.warnings.push_back("DEMO: this session is synthetic sample data generated by `detective demo-data`.");
    r.benchmark = BenchmarkResult{};
    analyze_result(r);
    SessionConfig cfg;
    cfg.executable = "/demo/demo_pipeline";
    cfg.args = {"--input", "tiles/"};
    cfg.label = "DEMO — synthetic data";
    r.benchmark.reset();
    Session s("demo-session", cfg, 1'700'000'000'000);
    s.demo = true;
    s.restore(SessionState::Completed, "synthetic demo data");
    s.set_result(std::make_shared<SessionResult>(r));
    Json j = session_to_json(s, true);
    j["demo"] = true;
    // Precomputed views so the static demo works without the API.
    j["views"] = {{"functions", functions_to_json(*r.profile)},
                  {"flamegraph", flame_tree_to_json(*r.profile, {})},
                  {"flamegraph_inverted", flame_tree_to_json(*r.profile, TreeFilter{std::nullopt, std::nullopt, true})},
                  {"threads", threads_to_json(*r.profile)},
                  {"modules", modules_to_json(*r.profile)}};
    Json details = Json::object();
    for (uint32_t i = 0; i < r.profile->functions().size(); ++i)
        details[r.profile->functions()[i].id] = function_detail_to_json(*r.profile, i, r.findings);
    j["views"]["function_details"] = details;
    if (!write_file(out, dump(j, 1))) return 1;
    std::printf("demo data written to %s\n", out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    int rest_c = argc - 2;
    char** rest_v = argv + 2;
    try {
        if (cmd == "capabilities") return cmd_capabilities();
        if (cmd == "record") return cmd_record(rest_c, rest_v);
        if (cmd == "report") return cmd_report(rest_c, rest_v);
        if (cmd == "compare") return cmd_compare(rest_c, rest_v);
        if (cmd == "overhead") return cmd_overhead(rest_c, rest_v);
        if (cmd == "serve") return cmd_serve(rest_c, rest_v);
        if (cmd == "demo-data") return cmd_demo_data(rest_c, rest_v);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
