// HTTP API tests against a real server bound to an ephemeral loopback port.
#include "detective/api_server.hpp"
#include "detective/exporter.hpp"

#include <httplib.h>

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace detective;

namespace {

struct Fixture {
    std::string data_dir;
    std::unique_ptr<ApiServer> server;
    std::unique_ptr<httplib::Client> client;

    explicit Fixture(size_t queue_capacity = 16) {
        data_dir = "/tmp/detective-api-test-" + std::to_string(::getpid()) + "-" + generate_session_id();
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allowed_dirs = {std::filesystem::path(FIXTURE_TARGET).parent_path().string(), EXAMPLES_DIR};
        cfg.data_dir = data_dir;
        cfg.profiler.alloc_lib_path = ALLOC_LIB;
        cfg.profiler.scratch_dir = "/tmp";
        cfg.queue_capacity = queue_capacity;
        server = std::make_unique<ApiServer>(cfg);
        REQUIRE(server->bind());
        server->start_background();
        client = std::make_unique<httplib::Client>("127.0.0.1", server->port());
        client->set_read_timeout(30, 0);
    }
    ~Fixture() {
        server->stop();
        std::filesystem::remove_all(data_dir);
    }
    Json get(const std::string& path, int expect = 200) {
        auto r = client->Get(path);
        REQUIRE(r);
        INFO(path << " -> " << r->status << " " << r->body.substr(0, 300));
        CHECK(r->status == expect);
        return r->body.empty() || r->get_header_value("Content-Type").find("json") == std::string::npos
                   ? Json() : Json::parse(r->body);
    }
    Json post(const std::string& path, const Json& body, int expect) {
        auto r = client->Post(path, body.dump(), "application/json");
        REQUIRE(r);
        INFO(path << " -> " << r->status << " " << r->body.substr(0, 300));
        CHECK(r->status == expect);
        return Json::parse(r->body);
    }
    Json wait_terminal(const std::string& id, int timeout_s = 60) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        Json s;
        while (std::chrono::steady_clock::now() < deadline) {
            s = get("/api/sessions/" + id);
            auto st = s["state"].get<std::string>();
            if (st == "COMPLETED" || st == "FAILED" || st == "CANCELLED") return s;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        FAIL("session did not finish");
        return s;
    }
};

}  // namespace

TEST_CASE("API: full profiling session through HTTP", "[api]") {
    Fixture f;
    auto health = f.get("/api/health");
    CHECK(health["status"] == "ok");
    CHECK(health["capabilities"]["software_cpu_clock"] == true);

    auto targets = f.get("/api/targets");
    CHECK_FALSE(targets["targets"].empty());

    auto created = f.post("/api/sessions", {{"executable", FIXTURE_TARGET}, {"args", {"--spin", "400"}}}, 201);
    std::string id = created["id"];
    CHECK(created["state"] == "CREATED");
    f.post("/api/sessions/" + id + "/start", Json::object(), 202);
    auto done = f.wait_terminal(id);
    REQUIRE(done["state"] == "COMPLETED");

    auto summary = f.get("/api/sessions/" + id + "/summary");
    CHECK(summary["sampling"]["total_samples"].get<int>() > 100);
    CHECK(summary["process"]["exit_code"] == 0);
    CHECK_FALSE(summary["threads"].empty());

    auto funcs = f.get("/api/sessions/" + id + "/functions?sort=self&limit=5");
    REQUIRE_FALSE(funcs["functions"].empty());
    CHECK(funcs["functions"][0]["name"] == "spin_hot(int)");
    std::string fid = funcs["functions"][0]["id"];
    auto detail = f.get("/api/sessions/" + id + "/functions/" + fid);
    CHECK(detail["callers"][0]["name"] == "main");
    CHECK_FALSE(detail["related_findings"].empty());

    auto fg = f.get("/api/sessions/" + id + "/flamegraph");
    CHECK(fg["root"]["value"] == summary["sampling"]["total_samples"]);
    auto inv = f.get("/api/sessions/" + id + "/flamegraph?view=inverted");
    CHECK(inv["inverted"] == true);
    auto findings = f.get("/api/sessions/" + id + "/findings");
    CHECK(findings["findings"][0]["kind"] == "cpu_hotspot");
    auto out = f.get("/api/sessions/" + id + "/output");
    CHECK(out["stdout"].get<std::string>().find("spin=") == 0);

    // Exports: JSON (re-importable), CSV and SVG.
    auto exported = f.get("/api/sessions/" + id + "/export?format=json");
    CHECK(exported["result"]["profile"]["stacks"].size() > 0);
    auto csv = f.client->Get("/api/sessions/" + id + "/export?format=csv");
    REQUIRE(csv);
    CHECK(csv->body.find("spin_hot") != std::string::npos);
    auto svg = f.client->Get("/api/sessions/" + id + "/export?format=svg");
    REQUIRE(svg);
    CHECK(svg->body.find("<svg") != std::string::npos);
    f.get("/api/sessions/" + id + "/export?format=../../etc/passwd", 400);

    auto imported = f.post("/api/import", exported, 201);
    CHECK(imported["state"] == "COMPLETED");
    CHECK(imported["id"] != id);
    auto cmp = f.get("/api/compare?baseline=" + id + "&candidate=" + imported["id"].get<std::string>());
    CHECK(cmp["metrics"][0]["verdict"] == "inconclusive");  // single runs cannot establish significance

    // Persistence: a new server instance reloads the session from disk.
    auto listed = f.get("/api/sessions");
    CHECK(listed["sessions"].size() == 2);
    {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.data_dir = f.data_dir;
        SessionManager m(cfg);
        m.load_persisted();
        auto restored = m.get(id);
        REQUIRE(restored);
        CHECK(restored->state() == SessionState::Completed);
        REQUIRE(restored->result());
        CHECK(restored->result()->profile->total_samples() == summary["sampling"]["total_samples"].get<uint64_t>());
    }

    f.client->Delete("/api/sessions/" + id);
    f.get("/api/sessions/" + id, 404);
}

TEST_CASE("API: input validation and structured errors", "[api][validation]") {
    Fixture f;
    auto e1 = f.post("/api/sessions", {{"executable", "/bin/sh"}}, 403);  // outside allowed dirs
    CHECK(e1["error"]["code"] == "executable_not_allowed");
    auto e2 = f.post("/api/sessions", {{"executable", FIXTURE_TARGET}, {"frequency_hz", 99999}}, 400);
    CHECK(e2["error"]["code"] == "invalid_config");
    auto bad = f.client->Post("/api/sessions", "{not json", "application/json");
    REQUIRE(bad);
    CHECK(bad->status == 400);
    CHECK(Json::parse(bad->body)["error"]["code"] == "invalid_json");
    auto form = f.client->Post("/api/sessions", "executable=/bin/sh", "application/x-www-form-urlencoded");
    REQUIRE(form);
    CHECK(form->status == 415);
    f.get("/api/sessions/does-not-exist", 404);
    f.get("/api/sessions/BAD_ID!", 404);
    f.get("/api/nope", 404);
    auto symlink_escape = f.post("/api/sessions", {{"executable", std::string(EXAMPLES_DIR) + "/../../../../bin/sh"}}, 403);
    CHECK(symlink_escape["error"]["code"] == "executable_not_allowed");

    // Results are not available before the session ran.
    auto created = f.post("/api/sessions", {{"executable", FIXTURE_TARGET}}, 201);
    f.get("/api/sessions/" + created["id"].get<std::string>() + "/functions", 409);

    // DNS-rebinding protection.
    httplib::Headers h = {{"Host", "evil.example.com"}};
    auto r = f.client->Get("/api/health", h);
    REQUIRE(r);
    CHECK(r->status == 403);
}

TEST_CASE("API: stopping a long-running session", "[api]") {
    Fixture f;
    auto created = f.post("/api/sessions?start=1", {{"executable", FIXTURE_TARGET}, {"args", {"--spin", "30000"}}}, 201);
    std::string id = created["id"];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (f.get("/api/sessions/" + id)["state"] != "RUNNING" && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.post("/api/sessions/" + id + "/stop", Json::object(), 202);
    auto done = f.wait_terminal(id, 15);
    CHECK(done["state"] == "CANCELLED");
    f.post("/api/sessions/" + id + "/stop", Json::object(), 409);   // already finished
    f.post("/api/sessions/" + id + "/start", Json::object(), 409);  // cannot restart a terminal session
}

TEST_CASE("API: bounded queue and concurrent requests", "[api][reliability]") {
    Fixture f(2);
    // One running + two queued fills the queue; further starts are rejected with 503.
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        auto c = f.post("/api/sessions", {{"executable", FIXTURE_TARGET}, {"args", {"--sleep", "600"}}, {"mode", "none"}}, 201);
        ids.push_back(c["id"]);
    }
    int accepted = 0, rejected = 0;
    for (const auto& id : ids) {
        auto r = f.client->Post("/api/sessions/" + id + "/start", "{}", "application/json");
        REQUIRE(r);
        if (r->status == 202) ++accepted;
        if (r->status == 503) ++rejected;
    }
    CHECK(rejected >= 1);
    CHECK(accepted + rejected == 5);

    // Concurrent readers while jobs run: every request gets a well-formed response.
    std::atomic<int> ok{0}, failed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            httplib::Client c("127.0.0.1", f.server->port());
            for (int i = 0; i < 25; ++i) {
                auto r = c.Get(i % 2 ? "/api/sessions" : "/api/sessions/" + ids[static_cast<size_t>((t + i) % 5)]);
                if (r && (r->status == 200) && Json::accept(r->body)) ++ok; else ++failed;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(failed == 0);
    CHECK(ok == 200);
    for (const auto& id : ids) {
        auto s = f.get("/api/sessions/" + id);
        if (s["state"] != "FAILED" && s["state"] != "CREATED") f.wait_terminal(id);
    }
}

TEST_CASE("API: repeated session creation and deletion", "[api][reliability]") {
    Fixture f;
    for (int i = 0; i < 100; ++i) {
        auto c = f.post("/api/sessions", {{"executable", FIXTURE_TARGET}}, 201);
        auto r = f.client->Delete("/api/sessions/" + c["id"].get<std::string>());
        REQUIRE(r);
        CHECK(r->status == 200);
    }
    CHECK(f.get("/api/sessions")["sessions"].empty());
}
