#include "detective/session.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>

namespace detective {

const char* to_string(SessionState s) {
    switch (s) {
        case SessionState::Created: return "CREATED";
        case SessionState::Queued: return "QUEUED";
        case SessionState::Starting: return "STARTING";
        case SessionState::Running: return "RUNNING";
        case SessionState::Stopping: return "STOPPING";
        case SessionState::Analyzing: return "ANALYZING";
        case SessionState::Completed: return "COMPLETED";
        case SessionState::Failed: return "FAILED";
        case SessionState::Cancelled: return "CANCELLED";
    }
    return "FAILED";
}

std::optional<SessionState> session_state_from_string(const std::string& s) {
    for (auto st : {SessionState::Created, SessionState::Queued, SessionState::Starting, SessionState::Running,
                    SessionState::Stopping, SessionState::Analyzing, SessionState::Completed, SessionState::Failed,
                    SessionState::Cancelled})
        if (s == to_string(st)) return st;
    return std::nullopt;
}

bool is_terminal(SessionState s) {
    return s == SessionState::Completed || s == SessionState::Failed || s == SessionState::Cancelled;
}

bool valid_transition(SessionState from, SessionState to) {
    using S = SessionState;
    switch (from) {
        case S::Created: return to == S::Queued || to == S::Starting || to == S::Cancelled || to == S::Failed;
        case S::Queued: return to == S::Starting || to == S::Cancelled || to == S::Failed;
        case S::Starting: return to == S::Running || to == S::Failed || to == S::Cancelled;
        case S::Running: return to == S::Stopping || to == S::Analyzing || to == S::Failed || to == S::Cancelled;
        case S::Stopping: return to == S::Analyzing || to == S::Failed || to == S::Cancelled;
        case S::Analyzing: return to == S::Completed || to == S::Failed || to == S::Cancelled;
        default: return false;  // terminal
    }
}

const char* to_string(ProfilingMode m) {
    switch (m) {
        case ProfilingMode::Sampling: return "sampling";
        case ProfilingMode::Instrumentation: return "instrumentation";
        case ProfilingMode::None: return "none";
    }
    return "none";
}

std::optional<ProfilingMode> profiling_mode_from_string(const std::string& s) {
    if (s == "sampling") return ProfilingMode::Sampling;
    if (s == "instrumentation") return ProfilingMode::Instrumentation;
    if (s == "none") return ProfilingMode::None;
    return std::nullopt;
}

std::optional<std::string> validate_config(const SessionConfig& c) {
    if (c.executable.empty()) return "executable is required";
    if (c.executable.size() > 4096) return "executable path too long";
    if (c.args.size() > 256) return "too many arguments (max 256)";
    for (const auto& a : c.args) {
        if (a.size() > 4096) return "argument too long";
        if (a.find('\0') != std::string::npos) return "arguments must not contain NUL";
    }
    if (c.env.size() > 128) return "too many environment variables (max 128)";
    for (const auto& [k, v] : c.env) {
        if (k.empty() || k.size() > 256 || v.size() > 4096) return "invalid environment variable size";
        for (char ch : k)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return "invalid environment variable name: " + k;
        if (k == "LD_PRELOAD" || k == "LD_LIBRARY_PATH" || k == "LD_AUDIT")
            return "setting " + k + " is not allowed";
    }
    if (c.frequency_hz < 1 || c.frequency_hz > 10'000) return "frequency_hz must be in [1, 10000]";
    if (c.timeout_ms < 100 || c.timeout_ms > 3'600'000) return "timeout_ms must be in [100, 3600000]";
    if (c.warmup_runs < 0 || c.warmup_runs > 20) return "warmup_runs must be in [0, 20]";
    if (c.benchmark_runs < 0 || c.benchmark_runs > 100) return "benchmark_runs must be in [0, 100]";
    if (c.label.size() > 200) return "label too long";
    return std::nullopt;
}

Session::Session(std::string id, SessionConfig cfg, int64_t created_ms)
    : id_(std::move(id)), config_(std::move(cfg)), created_ms_(created_ms) {}

SessionState Session::state() const {
    std::lock_guard lk(mu_);
    return state_;
}

bool Session::transition(SessionState to, const std::string& message) {
    std::lock_guard lk(mu_);
    if (!valid_transition(state_, to)) return false;
    state_ = to;
    if (!message.empty()) message_ = message;
    return true;
}

std::string Session::message() const {
    std::lock_guard lk(mu_);
    return message_;
}

void Session::restore(SessionState s, std::string message) {
    std::lock_guard lk(mu_);
    state_ = s;
    message_ = std::move(message);
}

void Session::set_result(std::shared_ptr<const SessionResult> r) {
    std::lock_guard lk(mu_);
    result_ = std::move(r);
}

std::shared_ptr<const SessionResult> Session::result() const {
    std::lock_guard lk(mu_);
    return result_;
}

bool valid_session_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

std::string generate_session_id() {
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}() ^
                               static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::lock_guard lk(mu);
    char buf[24];
    std::snprintf(buf, sizeof buf, "s-%012llx", static_cast<unsigned long long>(rng() & 0xffffffffffffULL));
    return buf;
}

}  // namespace detective
