// End-to-end profiling of real compiled programs. These tests validate that the
// profiler *finds* the known bottlenecks; they do not hardcode any results.
#include "detective/comparison.hpp"
#include "detective/profiler.hpp"
#include "detective/sampler.hpp"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <thread>

using namespace detective;

namespace {
ProfilerOptions options() {
    ProfilerOptions o;
    o.alloc_lib_path = ALLOC_LIB;
    o.scratch_dir = "/tmp";
    return o;
}
SessionConfig cfg(const std::string& exe, std::vector<std::string> args) {
    SessionConfig c;
    c.executable = exe;
    c.args = std::move(args);
    c.timeout_ms = 60'000;
    return c;
}
const FunctionStats* top_self(const Profile& p) {
    auto r = p.ranked_by_self();
    return r.empty() ? nullptr : &p.functions()[r[0]];
}
bool has_kind(const SessionResult& r, const std::string& k) {
    return std::any_of(r.findings.begin(), r.findings.end(), [&](const Finding& f) { return f.kind == k; });
}
}  // namespace

TEST_CASE("perf_event_open capability probe", "[profiling]") {
    auto c = probe_sampler_capabilities();
    INFO(c.summary);
    REQUIRE(c.software_cpu_clock);  // the rest of this file depends on it
}

TEST_CASE("sampling profiler finds the hot function of a known workload", "[profiling]") {
    auto r = run_profile(cfg(FIXTURE_TARGET, {"--spin", "600"}), options(), nullptr, {});
    REQUIRE(r->process.started);
    REQUIRE(r->collection.sampling_collected);
    REQUIRE(r->profile);
    const auto& p = *r->profile;
    CHECK(p.total_samples() > 300);  // ~600 ms at ~1 kHz
    const FunctionStats* top = top_self(p);
    REQUIRE(top);
    CHECK(top->name == "spin_hot(int)");
    CHECK(static_cast<double>(top->self_samples) / static_cast<double>(p.total_samples()) > 0.8);
    CHECK(top->source_file.find("fixture_target.cpp") != std::string::npos);
    // Stack contains main -> spin_hot.
    bool main_caller = false;
    for (uint32_t c : top->callers) main_caller |= p.functions()[c].name == "main";
    CHECK(main_caller);
    CHECK(has_kind(*r, "cpu_hotspot"));
    // Estimated CPU from samples roughly agrees with getrusage.
    double cpu = r->process.user_cpu_seconds + r->process.sys_cpu_seconds;
    CHECK(p.estimated_cpu_seconds() == Catch::Approx(cpu).epsilon(0.25));
}

TEST_CASE("Workload A: dominant call path is count_unique_slow -> contains_linear", "[profiling][workloads]") {
    auto r = run_profile(cfg(std::string(EXAMPLES_DIR) + "/inefficient_loop", {"--mode", "slow", "--n", "60000"}),
                         options(), nullptr, {});
    REQUIRE(r->profile);
    const auto& p = *r->profile;
    REQUIRE(top_self(p));
    CHECK(top_self(p)->name.rfind("contains_linear", 0) == 0);
    // Frameless-leaf caller recovery puts count_unique_slow between main and contains_linear.
    bool via_slow = false;
    for (uint32_t c : top_self(p)->callers) via_slow |= p.functions()[c].name.rfind("count_unique_slow", 0) == 0;
    CHECK(via_slow);
    REQUIRE(has_kind(*r, "cpu_hotspot"));
    bool path_fact = false;
    for (const auto& fact : r->findings[0].facts) path_fact |= fact.find("dominant call path") != std::string::npos;
    CHECK(path_fact);
}

TEST_CASE("Workload C: recursion is detected with depth", "[profiling][workloads]") {
    auto r = run_profile(cfg(std::string(EXAMPLES_DIR) + "/recursive_workload", {"--mode", "naive", "--n", "37"}),
                         options(), nullptr, {});
    REQUIRE(r->profile);
    auto it = std::find_if(r->findings.begin(), r->findings.end(), [](const Finding& f) { return f.kind == "recursion"; });
    REQUIRE(it != r->findings.end());
    CHECK(it->measurements[0].value >= 16);
}

TEST_CASE("Workload D: coarse locking shows off-CPU waiting across threads", "[profiling][workloads]") {
    auto r = run_profile(cfg(std::string(EXAMPLES_DIR) + "/contention_demo", {"--mode", "coarse", "--iterations", "4000"}),
                         options(), nullptr, {});
    REQUIRE(r->profile);
    CHECK(r->profile->threads().size() >= 4);
    CHECK(r->process.voluntary_ctx_switches > 100);
    CHECK(has_kind(*r, "off_cpu"));
}

TEST_CASE("stripped binaries are handled honestly", "[profiling][symbols]") {
    auto r = run_profile(cfg(std::string(EXAMPLES_DIR) + "/inefficient_loop_stripped", {"--n", "50000"}), options(),
                         nullptr, {});
    REQUIRE(r->profile);
    const FunctionStats* top = top_self(*r->profile);
    REQUIRE(top);
    CHECK_FALSE(top->resolved);
    CHECK(top->name.find("inefficient_loop_stripped+0x") == 0);  // address, never an invented name
    bool warned = std::any_of(r->collection.warnings.begin(), r->collection.warnings.end(),
                              [](const std::string& w) { return w.find("no symbol table") != std::string::npos; });
    CHECK(warned);
}

TEST_CASE("unavailable perf_event_open is reported, never faked", "[profiling]") {
    ProfilerOptions o = options();
    o.simulate_perf_unavailable = true;
    Session s("s-test", cfg(FIXTURE_TARGET, {"--spin", "50"}), 0);
    execute_session(s, o);
    CHECK(s.state() == SessionState::Failed);
    auto r = s.result();
    REQUIRE(r);
    CHECK(r->process.started);  // the program still ran
    CHECK_FALSE(r->collection.sampling_collected);
    CHECK_FALSE(r->profile);
    CHECK(s.message().find("profiling unavailable") != std::string::npos);
}

TEST_CASE("session lifecycle for a crashing target keeps partial results", "[profiling][session]") {
    Session s("s-crash", cfg(FIXTURE_TARGET, {"--spin", "100", "--crash"}), 0);
    execute_session(s, options());
    CHECK(s.state() == SessionState::Failed);
    CHECK(s.message().find("signal 11") != std::string::npos);
    REQUIRE(s.result());
    CHECK(s.result()->profile.has_value());
}

TEST_CASE("cancelling a running session", "[profiling][session]") {
    Session s("s-cancel", cfg(FIXTURE_TARGET, {"--spin", "20000"}), 0);
    std::thread stopper([&] {
        while (s.state() != SessionState::Running) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        s.request_cancel();
    });
    execute_session(s, options());
    stopper.join();
    CHECK(s.state() == SessionState::Cancelled);
    REQUIRE(s.result());
    CHECK(s.result()->process.cancelled);
    CHECK(s.result()->process.wall_seconds < 5);
}

TEST_CASE("instrumentation mode collects scope data from a rebuilt target", "[profiling][instrumentation]") {
    auto c = cfg(INSTR_DEMO, {"--requests", "4000"});
    c.mode = ProfilingMode::Instrumentation;
    auto r = run_profile(c, options(), nullptr, {});
    REQUIRE(r->instrumentation);
    CHECK_FALSE(r->profile);  // instrumentation is never mixed into sampling data
    auto& sc = r->instrumentation->scopes;
    auto it = std::find_if(sc.begin(), sc.end(), [](const ScopeStats& s) { return s.name == "db.query"; });
    REQUIRE(it != sc.end());
    CHECK(it->count == 1000);
    CHECK(r->instrumentation->marks.at("cache.hit") + r->instrumentation->marks.at("cache.miss") == 4000);
}

TEST_CASE("instrumentation mode on an uninstrumented binary reports missing data", "[profiling][instrumentation]") {
    auto c = cfg(FIXTURE_TARGET, {"--exit", "0"});
    c.mode = ProfilingMode::Instrumentation;
    auto r = run_profile(c, options(), nullptr, {});
    CHECK_FALSE(r->instrumentation);
    CHECK_FALSE(r->collection.instrumentation_error.empty());
}

TEST_CASE("allocation tracking counts real allocations", "[profiling][memory]") {
    auto naive = cfg(std::string(EXAMPLES_DIR) + "/allocation_stress", {"--mode", "naive", "--records", "20000"});
    naive.track_allocations = true;
    naive.mode = ProfilingMode::None;
    auto reuse = naive;
    reuse.args = {"--mode", "reuse", "--records", "20000"};
    auto a = run_profile(naive, options(), nullptr, {});
    auto b = run_profile(reuse, options(), nullptr, {});
    REQUIRE(a->memory);
    REQUIRE(b->memory);
    CHECK(a->memory->total_allocations > 1'000'000);
    CHECK(a->memory->total_allocations > 4 * b->memory->total_allocations);
    CHECK(a->memory->free_calls > 0);
    REQUIRE_FALSE(a->memory->sites.empty());
    CHECK(a->memory->sites[0].function.rfind("tokenize_naive", 0) == 0);
    CHECK(has_kind(*a, "allocation_pressure"));
}

TEST_CASE("benchmark runs and comparison of two implementations", "[profiling][compare]") {
    auto slow = cfg(std::string(EXAMPLES_DIR) + "/inefficient_loop", {"--mode", "slow", "--n", "30000"});
    slow.benchmark_runs = 5;
    slow.warmup_runs = 1;
    auto fast = slow;
    fast.args = {"--mode", "fast", "--n", "30000"};
    auto a = run_profile(slow, options(), nullptr, {});
    auto b = run_profile(fast, options(), nullptr, {});
    REQUIRE(a->benchmark);
    CHECK(a->benchmark->wall_seconds.size() == 5);
    CHECK(a->benchmark->warmup_runs == 1);
    auto c = compare_results(a.operator*(), b.operator*());
    CHECK(c.metrics[0].verdict == "improvement");
    CHECK(c.metrics[0].relative_change < -0.5);
    // The function that disappeared is the top decrease.
    auto it = std::find_if(c.functions.begin(), c.functions.end(),
                           [](const FunctionDelta& f) { return f.name.rfind("contains_linear", 0) == 0; });
    REQUIRE(it != c.functions.end());
    CHECK(it - c.functions.begin() < 3);
    CHECK(it->delta_self_share < -0.5);
    CHECK(it->significant);
}
