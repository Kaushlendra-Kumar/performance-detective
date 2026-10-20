#include "detective/bottleneck_detector.hpp"
#include "detective/comparison.hpp"
#include "detective/profiler.hpp"

#include "helpers.hpp"

#include <catch_amalgamated.hpp>

#include <algorithm>

using namespace detective;
using testutil::make_profile;

namespace {
const Finding* find_kind(const std::vector<Finding>& fs, const std::string& kind) {
    for (const auto& f : fs)
        if (f.kind == kind) return &f;
    return nullptr;
}
std::vector<Finding> analyze(const Profile& p, const ProcessResult* pr = nullptr) {
    AnalysisInput in;
    in.profile = &p;
    in.process = pr;
    in.sampled_threads = 1;
    auto fs = detect_bottlenecks(in);
    attach_recommendations(fs, in);
    return fs;
}
}  // namespace

TEST_CASE("function name normalization and classification", "[analysis]") {
    CHECK(function_base_name("process(std::vector<std::__cxx11::basic_string<char> > const&)") == "process");
    CHECK(function_base_name("std::basic_istream<char>& std::getline<char>(std::basic_istream<char>&)") ==
          "std::getline<char>");
    CHECK(function_base_name("Foo::operator()(int) const") == "Foo::operator()");
    CHECK(function_base_name("main") == "main");
    CHECK(classify_function("__GI___libc_malloc", "libc.so.6") == FuncCategory::Allocator);
    CHECK(classify_function("_int_free", "libc.so.6") == FuncCategory::Allocator);
    CHECK(classify_function("operator new(unsigned long)", "libstdc++.so.6") == FuncCategory::Allocator);
    CHECK(classify_function("__memmove_avx_unaligned_erms", "libc.so.6") == FuncCategory::MemCopy);
    CHECK(classify_function("__GI___pthread_mutex_lock", "libc.so.6") == FuncCategory::Synchronization);
    CHECK(classify_function("std::_Hashtable<int, int>::find(int const&)", "app") == FuncCategory::HashTable);
    CHECK(classify_function("std::_Rb_tree<std::string, int>::_M_lower_bound()", "app") == FuncCategory::OrderedMap);
    // Parameters mentioning std::string must not make an application function a "string" function.
    CHECK(classify_function("tokenize(std::__cxx11::basic_string<char> const&)", "app") == FuncCategory::Other);
    CHECK(classify_function("compute_chunk(unsigned long)", "app") == FuncCategory::Other);
    CHECK(classify_function("write", "libc.so.6") == FuncCategory::Syscall);
}

TEST_CASE("hotspot finding with evidence, separation and recommendations", "[analysis]") {
    auto p = make_profile({{"_start;main;count_unique_slow;contains_linear", 90}, {"_start;main", 10}});
    auto fs = analyze(p);
    const Finding* hot = find_kind(fs, "cpu_hotspot");
    REQUIRE(hot);
    CHECK(hot->affected_names[0] == "contains_linear");
    CHECK(hot->impact == Catch::Approx(0.9));
    CHECK(hot->severity == Severity::High);
    CHECK(hot->confidence == Confidence::Medium);  // only 100 samples
    CHECK_FALSE(hot->facts.empty());
    CHECK_FALSE(hot->interpretation.empty());
    CHECK_FALSE(hot->next_actions.empty());
    CHECK_FALSE(hot->limitations.empty());
    CHECK(hot->score == Catch::Approx(100 * 0.9 * 0.7));
    // The name hints at a search: algorithmic + indexed-lookup advice, no promised speedups.
    REQUIRE(hot->recommendations.size() == 2);
    for (const auto& r : hot->recommendations) {
        CHECK(r.expected_benefit.find('%') == std::string::npos);
        CHECK_FALSE(r.how_to_validate.empty());
        CHECK_FALSE(r.would_disprove.empty());
    }
    // The dominant path ends in the hotspot itself: it is folded into that finding, not duplicated.
    CHECK(find_kind(fs, "hot_path") == nullptr);
    bool path_fact = false;
    for (const auto& fact : hot->facts) path_fact |= fact.find("count_unique_slow → contains_linear") != std::string::npos;
    CHECK(path_fact);
    // Findings are sorted by score.
    for (size_t i = 1; i < fs.size(); ++i) CHECK(fs[i - 1].score >= fs[i].score);
}

TEST_CASE("a dominant path through non-hotspot frames is its own finding", "[analysis]") {
    std::vector<testutil::Spec> specs;
    for (int i = 0; i < 12; ++i) specs.push_back({"_start;main;pipeline;stage;leaf" + std::to_string(i), 70});
    for (int i = 0; i < 4; ++i) specs.push_back({"_start;main;other" + std::to_string(i), 40});
    auto fs = analyze(make_profile(specs));
    CHECK(find_kind(fs, "cpu_hotspot") == nullptr);  // each leaf is only ~7%
    const Finding* path = find_kind(fs, "hot_path");
    REQUIRE(path);
    CHECK(path->affected_names.back() == "stage");
    CHECK(path->impact == Catch::Approx(840.0 / 1000.0));
}

TEST_CASE("high confidence requires enough samples", "[analysis]") {
    auto p = make_profile({{"_start;main;hot", 4000}, {"_start;main", 1000}});
    auto pf = analyze(p);
    const Finding* hot = find_kind(pf, "cpu_hotspot");
    REQUIRE(hot);
    CHECK(hot->confidence == Confidence::High);
    // Moderate sample count but an overwhelming share: high.
    auto qf = analyze(make_profile({{"_start;main;hot", 290}, {"_start;main", 10}}));
    REQUIRE(find_kind(qf, "cpu_hotspot"));
    CHECK(find_kind(qf, "cpu_hotspot")->confidence == Confidence::High);
    // Moderate sample count, share near the threshold: not high.
    std::vector<testutil::Spec> near{{"_start;main;hot", 45}};
    for (int i = 0; i < 9; ++i) near.push_back({"_start;main;other" + std::to_string(i), 28});
    auto rf = analyze(make_profile(near));  // hot = 15% of 297; every other leaf < 10%
    REQUIRE(find_kind(rf, "cpu_hotspot"));
    CHECK(find_kind(rf, "cpu_hotspot")->affected_names[0] == "hot");
    CHECK(find_kind(rf, "cpu_hotspot")->confidence == Confidence::Medium);
}

TEST_CASE("no findings for a flat profile, and no hotspot below threshold", "[analysis]") {
    std::vector<testutil::Spec> specs;
    for (int i = 0; i < 20; ++i) specs.push_back({"_start;main;f" + std::to_string(i), 50});
    auto fs = analyze(make_profile(specs));
    CHECK(find_kind(fs, "cpu_hotspot") == nullptr);
    CHECK(find_kind(fs, "hot_path") == nullptr);
}

TEST_CASE("too few samples suppresses sampling findings", "[analysis]") {
    auto fs = analyze(make_profile({{"_start;main;hot", 10}}));
    REQUIRE(fs.size() == 1);
    CHECK(fs[0].id == "insufficient_samples");
}

TEST_CASE("recursion and allocator category rules", "[analysis]") {
    auto p = make_profile({{"_start;main;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib;fib", 600},
                           {"_start;main;build;libc:malloc", 150},
                           {"_start;main;build;libc:_int_free", 100},
                           {"_start;main;build", 150}});
    auto fs = analyze(p);
    const Finding* rec = find_kind(fs, "recursion");
    REQUIRE(rec);
    CHECK(rec->measurements[0].value == 17);
    const Finding* alloc = find_kind(fs, "allocator_cpu");
    REQUIRE(alloc);
    CHECK(alloc->impact == Catch::Approx(0.25));
    // The nearest application caller is identified as the driver.
    REQUIRE_FALSE(alloc->affected_names.empty());
    CHECK(alloc->affected_names[0] == "build");
    CHECK(alloc->recommendations.at(0).title.find("allocation") != std::string::npos);
}

TEST_CASE("off-CPU and kernel-time process rules", "[analysis]") {
    ProcessResult pr;
    pr.started = true;
    pr.exited_normally = true;
    pr.wall_seconds = 4.0;
    pr.user_cpu_seconds = 0.5;
    pr.sys_cpu_seconds = 0.5;
    pr.voluntary_ctx_switches = 20000;
    AnalysisInput in;
    in.process = &pr;
    in.sampled_threads = 4;
    auto fs = detect_bottlenecks(in);
    const Finding* off = find_kind(fs, "off_cpu");
    REQUIRE(off);
    CHECK(off->source == "process");
    CHECK(find_kind(fs, "kernel_time") != nullptr);
    // A fully busy process produces no off-CPU finding.
    pr.user_cpu_seconds = 3.9;
    pr.sys_cpu_seconds = 0.05;
    in.sampled_threads = 1;
    CHECK(find_kind(detect_bottlenecks(in), "off_cpu") == nullptr);
}

TEST_CASE("instrumentation and allocation rules", "[analysis]") {
    InstrumentationReport ir;
    ir.clock_overhead_ns = 40;
    ScopeStats big;
    big.name = "db.query";
    big.count = 100;
    big.total_ns = big.self_ns = 8e6;
    big.mean_ns = 8e4;
    ScopeStats tiny;
    tiny.name = "tiny";
    tiny.count = 500000;
    tiny.total_ns = tiny.self_ns = 2e6;
    tiny.mean_ns = 4;
    ir.scopes = {big, tiny};
    MemoryReport m;
    m.total_allocations = 5'000'000;
    m.bytes_requested = 1 << 30;
    m.sites.push_back({0x1234, "tokenize", "app", 4'000'000, 100});
    ProcessResult pr;
    pr.started = true;
    pr.wall_seconds = 1.0;
    AnalysisInput in;
    in.instrumentation = &ir;
    in.memory = &m;
    in.process = &pr;
    auto fs = detect_bottlenecks(in);
    attach_recommendations(fs, in);
    REQUIRE(find_kind(fs, "instr_hot_scope"));
    CHECK(find_kind(fs, "instr_hot_scope")->affected_names[0] == "db.query");
    const Finding* calls = find_kind(fs, "excessive_calls");
    REQUIRE(calls);
    CHECK(calls->confidence == Confidence::Low);  // marker overhead exceeds measured time
    const Finding* alloc = find_kind(fs, "allocation_pressure");
    REQUIRE(alloc);
    CHECK(alloc->affected_names[0] == "tokenize");
    CHECK_FALSE(alloc->recommendations.empty());
}

TEST_CASE("comparison requires repeated runs before claiming significance", "[analysis][compare]") {
    SessionResult a, b;
    a.process.started = b.process.started = true;
    a.process.exited_normally = b.process.exited_normally = true;
    a.process.wall_seconds = 1.0;
    b.process.wall_seconds = 1.5;
    auto c1 = compare_results(a, b);
    CHECK(c1.metrics[0].verdict == "inconclusive");

    a.benchmark = BenchmarkResult{};
    b.benchmark = BenchmarkResult{};
    for (int i = 0; i < 8; ++i) {
        a.benchmark->wall_seconds.push_back(1.0 + 0.01 * (i % 3));
        b.benchmark->wall_seconds.push_back(1.3 + 0.01 * (i % 3));
        a.benchmark->cpu_seconds.push_back(1.0);
        b.benchmark->cpu_seconds.push_back(1.0);
    }
    auto c2 = compare_results(a, b);
    CHECK(c2.metrics[0].verdict == "regression");
    CHECK(c2.metrics[0].relative_change == Catch::Approx(0.3).margin(0.01));
    CHECK(c2.metrics[1].verdict == "no_significant_change");

    // Noise around the same value: no claim.
    for (int i = 0; i < 8; ++i) b.benchmark->wall_seconds[static_cast<size_t>(i)] = 1.0 + 0.01 * ((i + 1) % 3);
    CHECK(compare_results(a, b).metrics[0].verdict == "no_significant_change");
}

TEST_CASE("function-level comparison uses stable ids", "[analysis][compare]") {
    SessionResult a, b;
    a.profile = make_profile({{"_start;main;slow", 900}, {"_start;main;other", 100}});
    b.profile = make_profile({{"_start;main;slow", 100}, {"_start;main;other", 900}});
    auto c = compare_results(a, b);
    REQUIRE(c.functions.size() >= 2);
    bool saw_slow = false;
    for (const auto& f : c.functions) {
        if (f.name == "slow") {
            saw_slow = true;
            CHECK(f.delta_self_share == Catch::Approx(-0.8));
            CHECK(f.significant);
        }
    }
    CHECK(saw_slow);
}
