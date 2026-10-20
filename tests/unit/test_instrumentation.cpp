#include "detective/instrument.hpp"
#include "detective/reports.hpp"

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

using namespace detective;

namespace {
void busy_us(int us) {
    auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {}
}
void inner() {
    DETECTIVE_SCOPE("inner");
    busy_us(200);
}
void outer() {
    DETECTIVE_SCOPE("outer");
    busy_us(100);
    inner();
    inner();
}
int recurse(int n) {
    DETECTIVE_SCOPE("recurse");
    return n == 0 ? 0 : 1 + recurse(n - 1);
}
std::string tmp_path(const char* name) {
    return std::string("/tmp/detective-test-") + std::to_string(::getpid()) + "-" + name;
}
}  // namespace

TEST_CASE("instrumentation records counts, self vs total, edges and marks", "[instrumentation]") {
    instr::reset_for_testing();
    for (int i = 0; i < 10; ++i) outer();
    for (int i = 0; i < 5; ++i) DETECTIVE_MARK("tick");
    std::thread t([] {
        for (int i = 0; i < 3; ++i) outer();
    });
    t.join();  // worker thread flushes at exit
    auto path = tmp_path("instr.json");
    REQUIRE(instr::write_report(path.c_str()));
    std::string err;
    auto r = load_instrumentation_report(path, &err);
    std::remove(path.c_str());
    REQUIRE(r);
    const ScopeStats* o = nullptr;
    const ScopeStats* in = nullptr;
    for (const auto& s : r->scopes) {
        if (s.name == "outer") o = &s;
        if (s.name == "inner") in = &s;
    }
    REQUIRE(o);
    REQUIRE(in);
    CHECK(o->count == 13);
    CHECK(in->count == 26);
    CHECK(o->thread_counts.size() == 2);
    // Self excludes instrumented children; total includes them.
    CHECK(o->total_ns > o->self_ns);
    CHECK(o->self_ns + in->total_ns == Catch::Approx(o->total_ns).epsilon(0.01));
    CHECK(in->min_ns >= 200'000);
    CHECK(o->max_ns >= o->min_ns);
    CHECK_FALSE(o->percentiles_valid);  // only 13 durations: percentiles are withheld
    bool edge = false;
    for (const auto& e : r->edges)
        if (e.parent == "outer" && e.child == "inner") {
            edge = true;
            CHECK(e.count == 26);
        }
    CHECK(edge);
    CHECK(r->marks.at("tick") == 5);
    CHECK(r->clock_overhead_ns > 0);
}

TEST_CASE("recursive scopes do not double count inclusive time", "[instrumentation]") {
    instr::reset_for_testing();
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) recurse(20);
    double wall = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    auto path = tmp_path("rec.json");
    REQUIRE(instr::write_report(path.c_str()));
    std::string err;
    auto r = load_instrumentation_report(path, &err);
    std::remove(path.c_str());
    REQUIRE(r);
    REQUIRE(r->scopes.size() == 1);
    CHECK(r->scopes[0].count == 200 * 21);
    CHECK(r->scopes[0].total_ns <= wall);  // outermost activations only
    CHECK(r->scopes[0].percentiles_valid);
}

TEST_CASE("missing or malformed instrumentation reports are reported, not fabricated", "[instrumentation]") {
    std::string err;
    CHECK_FALSE(load_instrumentation_report("/nonexistent/report.json", &err));
    CHECK(err.find("not found") != std::string::npos);
    auto path = tmp_path("bad.json");
    {
        FILE* f = std::fopen(path.c_str(), "w");
        std::fputs("{\"scopes\": [ {\"name\": 1} ]", f);
        std::fclose(f);
    }
    CHECK_FALSE(load_instrumentation_report(path, &err));
    CHECK(err.find("malformed") != std::string::npos);
    std::remove(path.c_str());
}
