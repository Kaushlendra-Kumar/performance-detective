// Safe target-process launcher: fork + execve with an argv vector (never a shell),
// bounded stdout/stderr capture, timeouts, cancellation and process-group cleanup.
#pragma once

#include "detective/types.hpp"

#include <sys/types.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace detective {

struct ProcessSpec {
    std::string executable;  // absolute path; executed directly via execve
    std::vector<std::string> args;  // argv[1..]
    std::vector<std::pair<std::string, std::string>> env;  // extra / overriding variables
    bool inherit_env = true;
    std::string working_dir;  // empty = inherit
    int timeout_ms = 60'000;
    size_t max_output_bytes = 1 << 20;  // per stream
};

// Hooks that let a collector attach to the child between fork() and execve().
class ProcessObserver {
public:
    virtual ~ProcessObserver() = default;
    // Called in the parent after fork, before the child is allowed to exec.
    // Returning false aborts the launch.
    virtual bool on_child_created(pid_t pid, std::string* error) = 0;
    // File descriptors to wake the poll loop (e.g. perf ring-buffer fds).
    virtual std::vector<int> poll_fds() { return {}; }
    // Called on every loop iteration while the child runs.
    virtual void on_poll() {}
    // Called once after the child has been reaped.
    virtual void on_child_exited() {}
};

class ProcessRunner {
public:
    // Runs the process to completion (or timeout / cancellation). Blocking.
    // `on_started` is invoked with the pid once exec has succeeded.
    ProcessResult run(const ProcessSpec& spec, ProcessObserver* observer = nullptr,
                      const std::atomic<bool>* cancel = nullptr,
                      const std::function<void(int)>& on_started = {});
};

// Checks that `path` is an existing regular, executable, non-setuid/setgid file.
// Returns an error message, or nullopt if acceptable. Resolves to a canonical path.
std::optional<std::string> validate_executable(const std::string& path, std::string* canonical);

// Small helper: run a tool and capture stdout (used for addr2line). No shell.
std::optional<std::string> run_and_capture(const std::vector<std::string>& argv, size_t max_bytes,
                                           int timeout_ms);

}  // namespace detective
