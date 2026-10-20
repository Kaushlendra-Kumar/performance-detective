#include "detective/process_runner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>

extern char** environ;

namespace detective {

namespace {

using Clock = std::chrono::steady_clock;

int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// RAII file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) { reset(); fd_ = o.release(); }
        return *this;
    }
    ~Fd() { reset(); }
    int get() const { return fd_; }
    int release() { int f = fd_; fd_ = -1; return f; }
    void reset() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

bool make_pipe(Fd& r, Fd& w) {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) return false;
    r = Fd(p[0]);
    w = Fd(p[1]);
    return true;
}

struct OutputSink {
    std::string* text;
    bool* truncated;
    size_t limit;
    bool eof = false;

    void drain(int fd) {
        char buf[16384];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0) {
                size_t room = text->size() < limit ? limit - text->size() : 0;
                size_t take = std::min<size_t>(room, static_cast<size_t>(n));
                text->append(buf, take);
                if (take < static_cast<size_t>(n)) *truncated = true;
                continue;
            }
            if (n == 0) eof = true;
            else if (errno == EINTR) continue;
            return;  // EAGAIN or error
        }
    }
};

}  // namespace

std::optional<std::string> validate_executable(const std::string& path, std::string* canonical) {
    if (path.empty()) return "executable path is empty";
    if (path.size() > 4096) return "executable path too long";
    char resolved[PATH_MAX];
    if (!::realpath(path.c_str(), resolved)) return "executable not found: " + path;
    struct stat st{};
    if (::stat(resolved, &st) != 0) return "cannot stat executable";
    if (!S_ISREG(st.st_mode)) return "not a regular file";
    if (st.st_mode & (S_ISUID | S_ISGID)) return "refusing to run setuid/setgid executables";
    if (::access(resolved, X_OK) != 0) return "file is not executable";
    if (canonical) *canonical = resolved;
    return std::nullopt;
}

ProcessResult ProcessRunner::run(const ProcessSpec& spec, ProcessObserver* observer,
                                 const std::atomic<bool>* cancel,
                                 const std::function<void(int)>& on_started) {
    ProcessResult res;

    // ---- Prepare everything that needs allocation before fork(). ----
    std::vector<std::string> argv_store;
    argv_store.push_back(spec.executable);
    for (const auto& a : spec.args) argv_store.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argv_store) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::map<std::string, std::string> envmap;
    if (spec.inherit_env) {
        for (char** e = environ; e && *e; ++e) {
            std::string kv(*e);
            auto eq = kv.find('=');
            if (eq != std::string::npos) envmap[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    for (const auto& [k, v] : spec.env) envmap[k] = v;
    std::vector<std::string> env_store;
    for (const auto& [k, v] : envmap) env_store.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);

    Fd out_r, out_w, err_r, err_w, gate_r, gate_w, exec_r, exec_w;
    if (!make_pipe(out_r, out_w) || !make_pipe(err_r, err_w) || !make_pipe(gate_r, gate_w) ||
        !make_pipe(exec_r, exec_w)) {
        res.start_error = std::string("pipe() failed: ") + std::strerror(errno);
        return res;
    }

    const char* wd = spec.working_dir.empty() ? nullptr : spec.working_dir.c_str();
    auto t_start = Clock::now();
    res.start_unix_ms = unix_ms_now();

    pid_t pid = ::fork();
    if (pid < 0) {
        res.start_error = std::string("fork() failed: ") + std::strerror(errno);
        return res;
    }
    if (pid == 0) {
        // ---- Child: async-signal-safe calls only. ----
        ::setpgid(0, 0);
        ::signal(SIGPIPE, SIG_DFL);
        ::dup2(out_w.get(), STDOUT_FILENO);
        ::dup2(err_w.get(), STDERR_FILENO);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        char b;
        // Wait until the parent has attached collectors.
        while (::read(gate_r.get(), &b, 1) < 0 && errno == EINTR) {}
        if (wd && ::chdir(wd) != 0) {
            int e = errno;
            (void)!::write(exec_w.get(), &e, sizeof e);
            ::_exit(127);
        }
        ::execve(argv[0], argv.data(), envp.data());
        int e = errno;
        (void)!::write(exec_w.get(), &e, sizeof e);
        ::_exit(127);
    }

    // ---- Parent ----
    ::setpgid(pid, pid);  // race-free with the child's own setpgid
    res.pid = pid;
    out_w.reset();
    err_w.reset();
    gate_r.reset();
    exec_w.reset();

    auto kill_group = [&](int sig) {
        ::kill(-pid, sig);
        ::kill(pid, sig);
    };

    std::string attach_error;
    if (observer && !observer->on_child_created(pid, &attach_error)) {
        kill_group(SIGKILL);
        ::waitpid(pid, nullptr, 0);
        res.start_error = "collector attach failed: " + attach_error;
        return res;
    }
    // Release the child.
    char go = 1;
    (void)!::write(gate_w.get(), &go, 1);
    gate_w.reset();

    // Exec status: EOF means exec succeeded (CLOEXEC closed the pipe).
    int exec_errno = 0;
    ssize_t n;
    do { n = ::read(exec_r.get(), &exec_errno, sizeof exec_errno); } while (n < 0 && errno == EINTR);
    exec_r.reset();
    if (n == static_cast<ssize_t>(sizeof exec_errno)) {
        ::waitpid(pid, nullptr, 0);
        res.start_error = std::string("exec failed: ") + std::strerror(exec_errno);
        res.end_unix_ms = unix_ms_now();
        if (observer) observer->on_child_exited();
        return res;
    }
    res.started = true;
    if (on_started) on_started(pid);

    ::fcntl(out_r.get(), F_SETFL, O_NONBLOCK);
    ::fcntl(err_r.get(), F_SETFL, O_NONBLOCK);
    OutputSink so{&res.stdout_text, &res.stdout_truncated, spec.max_output_bytes};
    OutputSink se{&res.stderr_text, &res.stderr_truncated, spec.max_output_bytes};

    auto deadline = t_start + std::chrono::milliseconds(spec.timeout_ms);
    bool exited = false;
    bool kill_sent = false;
    Clock::time_point term_sent_at{};
    bool term_sent = false;
    int status = 0;
    struct rusage ru{};

    while (!exited) {
        std::vector<pollfd> pfds;
        if (!so.eof) pfds.push_back({out_r.get(), POLLIN, 0});
        if (!se.eof) pfds.push_back({err_r.get(), POLLIN, 0});
        if (observer) for (int fd : observer->poll_fds()) pfds.push_back({fd, POLLIN, 0});
        ::poll(pfds.data(), pfds.size(), 20);
        if (!so.eof) so.drain(out_r.get());
        if (!se.eof) se.drain(err_r.get());
        if (observer) observer->on_poll();

        pid_t w = ::wait4(pid, &status, WNOHANG, &ru);
        if (w == pid) { exited = true; break; }

        auto now = Clock::now();
        bool want_stop = (cancel && cancel->load()) || now >= deadline;
        if (want_stop && !term_sent) {
            if (cancel && cancel->load()) res.cancelled = true; else res.timed_out = true;
            kill_group(SIGTERM);
            term_sent = true;
            term_sent_at = now;
        } else if (term_sent && !kill_sent && now - term_sent_at > std::chrono::milliseconds(300)) {
            kill_group(SIGKILL);
            kill_sent = true;
        }
    }
    auto t_end = Clock::now();
    res.end_unix_ms = unix_ms_now();
    res.wall_seconds = std::chrono::duration<double>(t_end - t_start).count();

    // Kill any leftover members of the process group, then drain remaining output.
    kill_group(SIGKILL);
    auto drain_deadline = Clock::now() + std::chrono::milliseconds(500);
    while ((!so.eof || !se.eof) && Clock::now() < drain_deadline) {
        std::vector<pollfd> pfds;
        if (!so.eof) pfds.push_back({out_r.get(), POLLIN, 0});
        if (!se.eof) pfds.push_back({err_r.get(), POLLIN, 0});
        ::poll(pfds.data(), pfds.size(), 20);
        if (!so.eof) so.drain(out_r.get());
        if (!se.eof) se.drain(err_r.get());
    }
    if (observer) observer->on_child_exited();

    if (WIFEXITED(status)) {
        res.exited_normally = true;
        res.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.term_signal = WTERMSIG(status);
    }
    res.user_cpu_seconds = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    res.sys_cpu_seconds = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    res.max_rss_kb = ru.ru_maxrss;
    res.voluntary_ctx_switches = ru.ru_nvcsw;
    res.involuntary_ctx_switches = ru.ru_nivcsw;
    return res;
}

std::optional<std::string> run_and_capture(const std::vector<std::string>& argv, size_t max_bytes,
                                           int timeout_ms) {
    if (argv.empty()) return std::nullopt;
    ProcessSpec spec;
    spec.executable = argv[0];
    spec.args.assign(argv.begin() + 1, argv.end());
    spec.timeout_ms = timeout_ms;
    spec.max_output_bytes = max_bytes;
    ProcessRunner r;
    auto res = r.run(spec);
    if (!res.started || !res.exited_normally || res.exit_code != 0) return std::nullopt;
    return res.stdout_text;
}

}  // namespace detective
