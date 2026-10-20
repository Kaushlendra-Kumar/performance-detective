// Profiling sessions: configuration, explicit state machine, results.
#pragma once

#include "detective/bottleneck_detector.hpp"
#include "detective/call_tree.hpp"
#include "detective/reports.hpp"
#include "detective/statistics.hpp"
#include "detective/types.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace detective {

// CREATED -> QUEUED -> STARTING -> RUNNING -> STOPPING? -> ANALYZING -> COMPLETED
// Failure and cancellation are separate terminal states.
enum class SessionState { Created, Queued, Starting, Running, Stopping, Analyzing, Completed, Failed, Cancelled };
const char* to_string(SessionState s);
std::optional<SessionState> session_state_from_string(const std::string& s);
bool is_terminal(SessionState s);
bool valid_transition(SessionState from, SessionState to);

enum class ProfilingMode { Sampling, Instrumentation, None };
const char* to_string(ProfilingMode m);
std::optional<ProfilingMode> profiling_mode_from_string(const std::string& s);

struct SessionConfig {
    std::string executable;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    std::string working_dir;
    ProfilingMode mode = ProfilingMode::Sampling;
    int frequency_hz = 999;
    int timeout_ms = 120'000;
    bool track_allocations = false;
    int warmup_runs = 0;     // unprofiled, discarded
    int benchmark_runs = 0;  // unprofiled, timed
    std::string label;
};

// Validates ranges and string sizes. Returns an error message or nullopt.
std::optional<std::string> validate_config(const SessionConfig& c);

struct BenchmarkResult {
    int warmup_runs = 0;
    std::vector<double> wall_seconds;
    std::vector<double> cpu_seconds;
    stats::Summary wall;
    stats::Summary cpu;
    int failed_runs = 0;
};

struct CollectionInfo {
    bool sampling_requested = false;
    bool sampling_collected = false;
    std::string sampling_error;
    std::string event;
    uint64_t period_ns = 0;
    int frequency_hz = 0;
    uint64_t raw_samples = 0;
    uint64_t lost_records = 0;
    std::vector<std::string> warnings;
    std::vector<ModuleInfo> modules;
    std::string instrumentation_error;
    std::string allocation_error;
    double analysis_seconds = 0;
};

struct SessionResult {
    ProcessResult process;
    CollectionInfo collection;
    std::optional<Profile> profile;
    std::optional<InstrumentationReport> instrumentation;
    std::optional<MemoryReport> memory;
    std::optional<BenchmarkResult> benchmark;
    std::vector<Finding> findings;
};

class Session {
public:
    Session(std::string id, SessionConfig cfg, int64_t created_ms);

    const std::string& id() const { return id_; }
    const SessionConfig& config() const { return config_; }
    int64_t created_ms() const { return created_ms_; }

    SessionState state() const;
    // Applies the transition if valid; returns false otherwise.
    bool transition(SessionState to, const std::string& message = "");
    std::string message() const;
    int pid() const { return pid_.load(); }
    void set_pid(int pid) { pid_.store(pid); }

    void request_cancel() { cancel_.store(true); }
    const std::atomic<bool>* cancel_flag() const { return &cancel_; }

    // Used only when restoring persisted/imported sessions (bypasses the state machine).
    void restore(SessionState s, std::string message);

    void set_result(std::shared_ptr<const SessionResult> r);
    std::shared_ptr<const SessionResult> result() const;

    bool demo = false;
    bool imported = false;

private:
    std::string id_;
    SessionConfig config_;
    int64_t created_ms_;
    mutable std::mutex mu_;
    SessionState state_ = SessionState::Created;
    std::string message_;
    std::atomic<int> pid_{-1};
    std::atomic<bool> cancel_{false};
    std::shared_ptr<const SessionResult> result_;
};

bool valid_session_id(const std::string& id);
std::string generate_session_id();

}  // namespace detective
