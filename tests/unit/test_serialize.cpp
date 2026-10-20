#include "detective/exporter.hpp"
#include "detective/profiler.hpp"

#include "helpers.hpp"

#include <catch_amalgamated.hpp>

using namespace detective;
using testutil::make_profile;

TEST_CASE("config parsing validates untrusted input", "[serialize][validation]") {
    std::string err;
    auto ok = config_from_json(Json::parse(R"({"executable":"/bin/true","args":["a","b"],"frequency_hz":500,
        "mode":"sampling","env":{"FOO":"1"},"benchmark_runs":5})"), &err);
    REQUIRE(ok);
    CHECK(ok->args.size() == 2);
    CHECK(ok->frequency_hz == 500);
    CHECK(ok->env.at(0).first == "FOO");
    CHECK(ok->benchmark_runs == 5);

    struct Bad { const char* json; const char* expect; };
    Bad bad[] = {
        {R"([1,2])", "JSON object"},
        {R"({"executable":"/bin/true","frequency_hz":0})", "frequency_hz"},
        {R"({"executable":"/bin/true","frequency_hz":"fast"})", "invalid type"},
        {R"({"executable":"/bin/true","mode":"magic"})", "mode"},
        {R"({"executable":"/bin/true","env":{"LD_PRELOAD":"/evil.so"}})", "LD_PRELOAD"},
        {R"({"executable":"/bin/true","env":{"BAD NAME":"x"}})", "environment variable name"},
        {R"({"executable":"/bin/true","env":{"A":5}})", "strings"},
        {R"({"executable":"/bin/true","benchmark_runs":1000})", "benchmark_runs"},
        {R"({"executable":"/bin/true","timeout_ms":1})", "timeout_ms"},
        {R"({"executable":""})", "required"},
        {R"({"executable":"/bin/true","surprise":1})", "unknown field"},
        {R"({"executable":"/bin/true","args":"not-an-array"})", "invalid type"},
    };
    for (const auto& b : bad) {
        std::string e;
        CAPTURE(b.json);
        CHECK_FALSE(config_from_json(Json::parse(b.json), &e));
        CHECK(e.find(b.expect) != std::string::npos);
    }
}

TEST_CASE("session results round-trip through JSON", "[serialize]") {
    SessionResult r;
    r.process.started = true;
    r.process.exited_normally = true;
    r.process.exit_code = 0;
    r.process.wall_seconds = 1.25;
    r.process.stdout_text = "hello \xff\xfe invalid utf8";
    r.collection.sampling_requested = r.collection.sampling_collected = true;
    r.collection.period_ns = 1'000'000;
    r.profile = make_profile({{"_start;main;hot", 800, 1}, {"_start;main;cold", 200, 2}});
    BenchmarkResult b;
    b.wall_seconds = {1.0, 1.1, 1.2};
    b.cpu_seconds = {0.9, 1.0, 1.1};
    r.benchmark = b;
    MemoryReport m;
    m.total_allocations = 42;
    m.sites.push_back({1, "f", "app", 42, 1000});
    r.memory = m;
    analyze_result(r);

    Json j = result_to_json(r);
    // Must serialize even with invalid UTF-8 from the target's output.
    std::string text = j.dump(-1, ' ', false, Json::error_handler_t::replace);
    std::string err;
    auto back = result_from_json(Json::parse(text), &err);
    REQUIRE(back);
    CHECK(back->process.wall_seconds == 1.25);
    REQUIRE(back->profile);
    CHECK(back->profile->total_samples() == 1000);
    CHECK(back->profile->functions().size() == r.profile->functions().size());
    for (size_t i = 0; i < r.profile->functions().size(); ++i)
        CHECK(back->profile->functions()[i].id == r.profile->functions()[i].id);
    REQUIRE(back->benchmark);
    CHECK(back->benchmark->wall.median == Catch::Approx(1.1));
    REQUIRE(back->memory);
    CHECK(back->memory->total_allocations == 42);
}

TEST_CASE("malformed session JSON is rejected without crashing", "[serialize][validation]") {
    const char* cases[] = {R"({})", R"({"process":5})",
                           R"({"process":{},"profile":{"functions":[{"name":"f"}],"stacks":[["x",1,[0]]]}})",
                           R"({"process":{},"profile":{"functions":"nope","stacks":[]}})"};
    for (const char* c : cases) {
        std::string err;
        CAPTURE(c);
        CHECK_FALSE(result_from_json(Json::parse(c), &err));
        CHECK_FALSE(err.empty());
    }
    // Out-of-range frame indices are dropped rather than trusted.
    std::string err;
    auto r = result_from_json(Json::parse(R"({"process":{},"profile":{"period_ns":1000,
        "functions":[{"name":"f"}],"stacks":[[1,5,[0]],[1,9,[3]]]}})"), &err);
    REQUIRE(r);
    CHECK(r->profile->total_samples() == 5);
}

TEST_CASE("derived views: functions, tree, csv", "[serialize]") {
    auto p = make_profile({{"_start;main;hot", 80, 1}, {"_start;main;cold", 20, 2}, {"_start;main;=SUM(A1)", 1, 1}});
    auto fj = functions_to_json(p);
    CHECK(fj["functions"][0]["name"] == "hot");
    CHECK(fj["functions"][0]["self_share"].get<double>() == Catch::Approx(80.0 / 101.0));
    FunctionQuery q;
    q.search = "COL";
    CHECK(functions_to_json(p, q)["matched"] == 1);
    FunctionQuery t;
    t.tid = 2;
    auto tj = functions_to_json(p, t);
    CHECK(tj["total_samples"] == 20);

    auto tree = flame_tree_to_json(p, {});
    CHECK(tree["total"] == 101);
    CHECK(tree["root"]["children"][0]["name"] == "_start");
    auto pruned = flame_tree_to_json(p, {}, 20000, 0.05);
    CHECK(pruned["pruned"].get<int>() >= 1);
    auto capped = flame_tree_to_json(p, {}, 2, 0.0);
    CHECK(capped["nodes"] == 3);

    auto csv = functions_csv(p);
    CHECK(csv.rfind("id,function", 0) == 0);
    CHECK(csv.find("\"'=SUM(A1)\"") != std::string::npos);  // formula injection neutralized
}
