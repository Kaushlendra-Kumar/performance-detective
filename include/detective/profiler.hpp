// End-to-end profiling pipeline: (warm-up + benchmark runs) -> profiled run -> analysis.
#pragma once

#include "detective/session.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace detective {

struct ProfilerOptions {
    std::string alloc_lib_path;  // libdetective_alloc.so for allocation tracking
    std::string scratch_dir;     // where per-run report files are written
    bool resolve_sources = true;
    bool simulate_perf_unavailable = false;  // testing only
};

struct RunHooks {
    std::function<void(SessionState, const std::string&)> on_state;
    std::function<void(int)> on_pid;
};

// Runs a complete session. Never throws for target failures; failures are reported
// in the returned result (process.start_error, exit status, collection errors).
std::shared_ptr<SessionResult> run_profile(const SessionConfig& cfg, const ProfilerOptions& opt,
                                           const std::atomic<bool>* cancel, const RunHooks& hooks);

// Re-runs the analysis engine (findings + recommendations) on a result.
void analyze_result(SessionResult& r);

// Drives a Session object through its state machine using run_profile.
void execute_session(Session& s, const ProfilerOptions& opt);

}  // namespace detective
