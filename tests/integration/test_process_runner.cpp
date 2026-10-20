#include "detective/process_runner.hpp"

#include <catch_amalgamated.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace detective;

namespace {
ProcessSpec spec(std::vector<std::string> args) {
    ProcessSpec s;
    s.executable = FIXTURE_TARGET;
    s.args = std::move(args);
    s.timeout_ms = 10'000;
    return s;
}
}  // namespace

TEST_CASE("launches a real executable and captures output and exit status", "[process]") {
    ProcessRunner r;
    auto res = r.run(spec({"--args", "--stderr", "oops", "--exit", "3"}));
    REQUIRE(res.started);
    CHECK(res.pid > 0);
    CHECK(res.exited_normally);
    CHECK(res.exit_code == 3);
    CHECK(res.stdout_text.find("--args") != std::string::npos);
    CHECK(res.stderr_text == "oops");
    CHECK(res.wall_seconds > 0);
    CHECK(res.end_unix_ms >= res.start_unix_ms);
}

TEST_CASE("arguments are passed verbatim, never through a shell", "[process][security]") {
    ProcessRunner r;
    auto res = r.run(spec({"--args", "a b", "$(touch /tmp/detective-pwned)", "; rm -rf /", "`id`"}));
    REQUIRE(res.started);
    CHECK(res.stdout_text.find("a b\n") != std::string::npos);
    CHECK(res.stdout_text.find("$(touch /tmp/detective-pwned)\n") != std::string::npos);
    CHECK(res.stdout_text.find("; rm -rf /\n") != std::string::npos);
    CHECK(::access("/tmp/detective-pwned", F_OK) != 0);
}

TEST_CASE("environment variables are configurable", "[process]") {
    ProcessRunner r;
    auto s = spec({"--env", "DETECTIVE_TEST_VAR"});
    s.env.emplace_back("DETECTIVE_TEST_VAR", "hello world");
    auto res = r.run(s);
    CHECK(res.stdout_text == "hello world\n");
}

TEST_CASE("a crashing target is reported with its signal", "[process]") {
    ProcessRunner r;
    auto res = r.run(spec({"--crash"}));
    REQUIRE(res.started);
    CHECK_FALSE(res.exited_normally);
    CHECK(res.term_signal == SIGSEGV);
}

TEST_CASE("startup failures are reported", "[process]") {
    ProcessRunner r;
    ProcessSpec s;
    s.executable = "/nonexistent/program";
    auto res = r.run(s);
    CHECK_FALSE(res.started);
    CHECK(res.start_error.find("exec failed") != std::string::npos);
    ProcessSpec w = spec({});
    w.working_dir = "/nonexistent-dir";
    auto res2 = r.run(w);
    CHECK_FALSE(res2.started);
}

TEST_CASE("timeouts kill the target", "[process]") {
    ProcessRunner r;
    auto s = spec({"--sleep", "10000"});
    s.timeout_ms = 300;
    auto res = r.run(s);
    CHECK(res.timed_out);
    CHECK_FALSE(res.exited_normally);
    CHECK(res.wall_seconds < 3.0);
}

TEST_CASE("cancellation stops a running target", "[process]") {
    ProcessRunner r;
    std::atomic<bool> cancel{false};
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel = true;
    });
    auto res = r.run(spec({"--sleep", "10000"}), nullptr, &cancel);
    t.join();
    CHECK(res.cancelled);
    CHECK(res.wall_seconds < 3.0);
}

TEST_CASE("output capture is bounded", "[process][reliability]") {
    ProcessRunner r;
    auto s = spec({"--spam", "5000000"});
    s.max_output_bytes = 64 * 1024;
    auto res = r.run(s);
    REQUIRE(res.started);
    CHECK(res.exited_normally);  // the target is never blocked on a full pipe
    CHECK(res.stdout_text.size() == 64 * 1024);
    CHECK(res.stdout_truncated);
}

TEST_CASE("repeated launches leave no zombies or leaked descriptors", "[process][reliability]") {
    auto count_fds = [] {
        int n = 0;
        for (int fd = 0; fd < 1024; ++fd)
            if (::fcntl(fd, F_GETFD) != -1) ++n;
        return n;
    };
    int before = count_fds();
    ProcessRunner r;
    for (int i = 0; i < 50; ++i) {
        auto res = r.run(spec({"--exit", "0"}));
        REQUIRE(res.started);
    }
    CHECK(count_fds() == before);
    CHECK(::waitpid(-1, nullptr, WNOHANG) <= 0);  // nothing left to reap
}
