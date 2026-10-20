#include "detective/profiler.hpp"

#include "detective/process_runner.hpp"
#include "detective/sampler.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace detective {

namespace {

ProcessSpec make_spec(const SessionConfig& cfg, const std::string& exe) {
    ProcessSpec spec;
    spec.executable = exe;
    spec.args = cfg.args;
    spec.env = cfg.env;
    spec.working_dir = cfg.working_dir;
    spec.timeout_ms = cfg.timeout_ms;
    return spec;
}

std::string scratch_file(const ProfilerOptions& opt, const char* suffix) {
    std::string dir = opt.scratch_dir.empty() ? "/tmp" : opt.scratch_dir;
    return dir + "/run-" + generate_session_id() + suffix;
}

}  // namespace

void analyze_result(SessionResult& r) {
    AnalysisInput in;
    in.profile = r.profile ? &*r.profile : nullptr;
    in.process = r.process.started ? &r.process : nullptr;
    in.instrumentation = r.instrumentation ? &*r.instrumentation : nullptr;
    in.memory = r.memory ? &*r.memory : nullptr;
    if (r.profile) {
        size_t busy = 0;
        uint64_t total = r.profile->total_samples();
        for (const auto& t : r.profile->threads())
            if (total && static_cast<double>(t.samples) / static_cast<double>(total) >= 0.05) ++busy;
        in.sampled_threads = busy;
    } else if (r.instrumentation) {
        in.sampled_threads = r.instrumentation->threads;
    }
    r.findings = detect_bottlenecks(in);
    attach_recommendations(r.findings, in);
}

std::shared_ptr<SessionResult> run_profile(const SessionConfig& cfg, const ProfilerOptions& opt,
                                           const std::atomic<bool>* cancel, const RunHooks& hooks) {
    auto result = std::make_shared<SessionResult>();
    auto state = [&](SessionState s, const std::string& m) { if (hooks.on_state) hooks.on_state(s, m); };

    std::string exe;
    if (auto err = validate_executable(cfg.executable, &exe)) {
        result->process.start_error = *err;
        return result;
    }

    ProcessRunner runner;
    bool announced_running = false;
    auto on_started = [&](int pid) {
        if (hooks.on_pid) hooks.on_pid(pid);
        if (!announced_running) {
            announced_running = true;
            state(SessionState::Running, "target running");
        }
    };

    // ---- Warm-up and benchmark runs (unprofiled, so timings are not perturbed).
    if (cfg.warmup_runs > 0 || cfg.benchmark_runs > 0) {
        BenchmarkResult b;
        b.warmup_runs = cfg.warmup_runs;
        ProcessSpec spec = make_spec(cfg, exe);
        for (int i = 0; i < cfg.warmup_runs + cfg.benchmark_runs; ++i) {
            if (cancel && cancel->load()) break;
            auto pr = runner.run(spec, nullptr, cancel, on_started);
            if (!pr.started) {
                result->process = pr;
                return result;  // exec failure: no point continuing
            }
            bool ok = pr.exited_normally && pr.exit_code == 0 && !pr.timed_out && !pr.cancelled;
            if (i < cfg.warmup_runs) continue;
            if (!ok) {
                ++b.failed_runs;
                continue;
            }
            b.wall_seconds.push_back(pr.wall_seconds);
            b.cpu_seconds.push_back(pr.user_cpu_seconds + pr.sys_cpu_seconds);
        }
        b.wall = stats::summarize(b.wall_seconds);
        b.cpu = stats::summarize(b.cpu_seconds);
        result->benchmark = std::move(b);
    }
    if (cancel && cancel->load()) {
        result->process.cancelled = true;
        return result;
    }

    // ---- Profiled run.
    ProcessSpec spec = make_spec(cfg, exe);
    std::string instr_path, alloc_path;
    if (cfg.mode == ProfilingMode::Instrumentation) {
        instr_path = scratch_file(opt, "-instr.json");
        spec.env.emplace_back("DETECTIVE_INSTR_OUT", instr_path);
    }
    if (cfg.track_allocations) {
        if (opt.alloc_lib_path.empty() || ::access(opt.alloc_lib_path.c_str(), R_OK) != 0) {
            result->collection.allocation_error = "allocation tracker library not found: " + opt.alloc_lib_path;
        } else {
            alloc_path = scratch_file(opt, "-alloc.json");
            spec.env.emplace_back("LD_PRELOAD", opt.alloc_lib_path);
            spec.env.emplace_back("DETECTIVE_ALLOC_OUT", alloc_path);
        }
    }

    result->collection.sampling_requested = cfg.mode == ProfilingMode::Sampling;
    std::unique_ptr<PerfSampler> sampler;
    if (cfg.mode == ProfilingMode::Sampling) {
        SamplerConfig sc;
        sc.frequency_hz = cfg.frequency_hz;
        sc.simulate_unavailable = opt.simulate_perf_unavailable;
        sampler = std::make_unique<PerfSampler>(sc);
    }
    result->process = runner.run(spec, sampler.get(), cancel, on_started);
    state(SessionState::Analyzing, "analyzing");
    auto t0 = std::chrono::steady_clock::now();

    if (sampler) {
        RawProfile raw = sampler->take_profile();
        auto& c = result->collection;
        c.sampling_collected = raw.collected;
        c.sampling_error = raw.collection_error;
        c.event = raw.event_name;
        c.period_ns = raw.sample_period_ns;
        c.frequency_hz = cfg.frequency_hz;
        c.raw_samples = raw.samples.size();
        c.lost_records = raw.lost_records;
        c.warnings = raw.warnings;
        if (raw.lost_records > 0)
            c.warnings.push_back(std::to_string(raw.lost_records) +
                                 " samples were lost by the kernel (ring buffer overflow); totals are under-counted.");
        if (raw.collected && result->process.started) {
            SymbolResolver resolver;
            result->profile = Profile::build(raw, resolver, opt.resolve_sources);
            c.modules = resolver.modules();
            if (result->profile->recovered_frames() > 0)
                c.warnings.push_back(std::to_string(result->profile->recovered_frames()) +
                                     " samples had a frameless leaf; its caller was recovered from the stack-top "
                                     "return address (verified: the preceding call instruction targets the sampled function).");
            for (const auto& m : c.modules) {
                if (m.loaded && !m.has_symtab && m.path.find(".so") == std::string::npos)
                    c.warnings.push_back("executable " + m.path + " has no symbol table (stripped); "
                                         "functions are shown as module+offset");
            }
        }
    }
    if (!instr_path.empty()) {
        std::string err;
        result->instrumentation = load_instrumentation_report(instr_path, &err);
        if (!result->instrumentation) result->collection.instrumentation_error = err;
        std::remove(instr_path.c_str());
    }
    if (!alloc_path.empty()) {
        std::string err;
        result->memory = load_memory_report(alloc_path, &err);
        if (!result->memory) result->collection.allocation_error = err;
        std::remove(alloc_path.c_str());
    }
    analyze_result(*result);
    result->collection.analysis_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

void execute_session(Session& s, const ProfilerOptions& opt) {
    if (!s.transition(SessionState::Starting, "starting")) return;  // e.g. cancelled while queued
    RunHooks hooks;
    hooks.on_pid = [&](int pid) { s.set_pid(pid); };
    hooks.on_state = [&](SessionState st, const std::string& m) {
        if (st == SessionState::Analyzing && s.cancel_flag()->load()) {
            s.transition(SessionState::Stopping, "stopping");
        }
        s.transition(st, m);
    };
    std::shared_ptr<SessionResult> r;
    try {
        r = run_profile(s.config(), opt, s.cancel_flag(), hooks);
    } catch (const std::exception& e) {
        s.transition(SessionState::Failed, std::string("internal error: ") + e.what());
        return;
    }
    s.set_result(r);
    const auto& p = r->process;
    if (s.cancel_flag()->load() || p.cancelled) {
        s.transition(SessionState::Cancelled, "cancelled by user; partial results kept");
    } else if (!p.started) {
        s.transition(SessionState::Failed, "failed to start: " + p.start_error);
    } else if (p.timed_out) {
        s.transition(SessionState::Failed, "target exceeded the timeout and was killed; partial results kept");
    } else if (!p.exited_normally) {
        s.transition(SessionState::Failed, "target terminated by signal " + std::to_string(p.term_signal) +
                                               "; partial results kept");
    } else if (r->collection.sampling_requested && !r->collection.sampling_collected) {
        s.transition(SessionState::Failed, "profiling unavailable: " + r->collection.sampling_error);
    } else {
        std::string msg = "completed";
        if (p.exit_code != 0) msg += " (target exit code " + std::to_string(p.exit_code) + ")";
        s.transition(SessionState::Completed, msg);
    }
}

}  // namespace detective
