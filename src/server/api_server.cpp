#include "detective/api_server.hpp"

#include "detective/comparison.hpp"
#include "detective/exporter.hpp"
#include "detective/flame_graph.hpp"
#include "detective/process_runner.hpp"
#include "detective/sampler.hpp"

#include <httplib.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace detective {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string dump(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

void send_json(httplib::Response& res, const Json& j, int status = 200) {
    res.status = status;
    res.set_content(dump(j), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& code, const std::string& message) {
    send_json(res, {{"error", {{"code", code}, {"message", message}}}}, status);
}

std::string canonical_dir(const std::string& d) {
    char buf[PATH_MAX];
    if (!::realpath(d.c_str(), buf)) return {};
    return buf;
}

}  // namespace

// ------------------------------------------------------------------ WorkQueue
WorkQueue::WorkQueue(size_t workers, size_t capacity) : capacity_(capacity) {
    for (size_t i = 0; i < std::max<size_t>(1, workers); ++i) threads_.emplace_back([this] { loop(); });
}

WorkQueue::~WorkQueue() { shutdown(); }

void WorkQueue::shutdown() {
    {
        std::lock_guard lk(mu_);
        if (stopping_ && threads_.empty()) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

bool WorkQueue::try_push(std::function<void()> job) {
    {
        std::lock_guard lk(mu_);
        if (stopping_ || jobs_.size() >= capacity_) return false;
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

size_t WorkQueue::pending() const {
    std::lock_guard lk(mu_);
    return jobs_.size();
}

void WorkQueue::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) return;  // stopping
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();  // jobs catch their own exceptions
    }
}

// ------------------------------------------------------------- SessionManager
SessionManager::SessionManager(ServerConfig cfg) : cfg_(std::move(cfg)), queue_(cfg_.workers, cfg_.queue_capacity) {
    std::vector<std::string> canon;
    for (const auto& d : cfg_.allowed_dirs) {
        auto c = canonical_dir(d);
        if (!c.empty()) canon.push_back(c);
    }
    cfg_.allowed_dirs = canon;
    if (!cfg_.data_dir.empty()) ::mkdir(cfg_.data_dir.c_str(), 0700);
}

SessionManager::~SessionManager() {
    for (auto& s : list()) s->request_cancel();
    queue_.shutdown();
}

std::optional<std::string> SessionManager::check_executable_allowed(const std::string& path,
                                                                    std::string* canonical) const {
    std::string canon;
    if (auto err = validate_executable(path, &canon)) return err;
    for (const auto& d : cfg_.allowed_dirs) {
        if (canon.size() > d.size() && canon.compare(0, d.size(), d) == 0 && canon[d.size()] == '/') {
            if (canonical) *canonical = canon;
            return std::nullopt;
        }
    }
    return "executable is outside the allowed target directories (start the server with --allow-dir)";
}

std::vector<std::string> SessionManager::list_targets() const {
    std::vector<std::string> out;
    for (const auto& d : cfg_.allowed_dirs) {
        DIR* dir = ::opendir(d.c_str());
        if (!dir) continue;
        while (dirent* e = ::readdir(dir)) {
            std::string p = d + "/" + e->d_name;
            if (e->d_name[0] == '.') continue;
            if (!validate_executable(p, nullptr)) out.push_back(p);
        }
        ::closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<Session> SessionManager::create(const SessionConfig& cfg_in, Error* err) {
    SessionConfig cfg = cfg_in;
    if (auto e = validate_config(cfg)) {
        *err = {400, "invalid_config", *e};
        return nullptr;
    }
    std::string canon;
    if (auto e = check_executable_allowed(cfg.executable, &canon)) {
        *err = {403, "executable_not_allowed", *e};
        return nullptr;
    }
    cfg.executable = canon;
    if (!cfg.working_dir.empty()) {
        struct stat st{};
        if (::stat(cfg.working_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            *err = {400, "invalid_config", "working_dir does not exist"};
            return nullptr;
        }
    }
    std::lock_guard lk(mu_);
    if (sessions_.size() >= cfg_.max_sessions) {
        *err = {429, "too_many_sessions", "session limit reached; delete old sessions"};
        return nullptr;
    }
    auto s = std::make_shared<Session>(generate_session_id(), std::move(cfg), now_ms());
    sessions_[s->id()] = s;
    return s;
}

std::shared_ptr<Session> SessionManager::get(const std::string& id) const {
    if (!valid_session_id(id)) return nullptr;
    std::lock_guard lk(mu_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Session>> SessionManager::list() const {
    std::lock_guard lk(mu_);
    std::vector<std::shared_ptr<Session>> v;
    for (const auto& [k, s] : sessions_) v.push_back(s);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a->created_ms() != b->created_ms() ? a->created_ms() > b->created_ms() : a->id() < b->id();
    });
    return v;
}

bool SessionManager::start(const std::string& id, Error* err) {
    auto s = get(id);
    if (!s) { *err = {404, "not_found", "no such session"}; return false; }
    if (s->demo || s->imported) { *err = {409, "invalid_state", "imported sessions cannot be re-run"}; return false; }
    if (!s->transition(SessionState::Queued, "queued")) {
        *err = {409, "invalid_state", std::string("cannot start a session in state ") + to_string(s->state())};
        return false;
    }
    ProfilerOptions opt = cfg_.profiler;
    auto job = [this, s, opt] {
        try {
            execute_session(*s, opt);
        } catch (const std::exception& e) {
            s->transition(SessionState::Failed, std::string("internal error: ") + e.what());
        }
        persist(*s);
    };
    if (!queue_.try_push(job)) {
        s->transition(SessionState::Failed, "work queue full");
        *err = {503, "queue_full", "too many queued profiling jobs; try again later"};
        return false;
    }
    return true;
}

bool SessionManager::stop(const std::string& id, Error* err) {
    auto s = get(id);
    if (!s) { *err = {404, "not_found", "no such session"}; return false; }
    if (is_terminal(s->state())) { *err = {409, "invalid_state", "session already finished"}; return false; }
    s->request_cancel();
    // A queued (not yet running) session is cancelled immediately.
    if (s->state() == SessionState::Queued || s->state() == SessionState::Created)
        s->transition(SessionState::Cancelled, "cancelled before start");
    return true;
}

bool SessionManager::remove(const std::string& id, Error* err) {
    auto s = get(id);
    if (!s) { *err = {404, "not_found", "no such session"}; return false; }
    if (!is_terminal(s->state()) && s->state() != SessionState::Created) {
        *err = {409, "invalid_state", "stop the session before deleting it"};
        return false;
    }
    {
        std::lock_guard lk(mu_);
        sessions_.erase(id);
    }
    if (!cfg_.data_dir.empty()) std::remove((cfg_.data_dir + "/" + id + ".json").c_str());
    return true;
}

std::shared_ptr<Session> SessionManager::import_result(SessionResult r, SessionConfig cfg, const std::string& label,
                                                       Error* err) {
    std::lock_guard lk(mu_);
    if (sessions_.size() >= cfg_.max_sessions) {
        *err = {429, "too_many_sessions", "session limit reached"};
        return nullptr;
    }
    cfg.label = label.empty() ? cfg.label : label;
    auto s = std::make_shared<Session>(generate_session_id(), std::move(cfg), now_ms());
    s->imported = true;
    analyze_result(r);  // findings are recomputed by this engine version
    s->set_result(std::make_shared<SessionResult>(std::move(r)));
    s->restore(SessionState::Completed, "imported");
    sessions_[s->id()] = s;
    persist(*s);
    return s;
}

void SessionManager::persist(const Session& s) const {
    if (cfg_.data_dir.empty() || s.demo) return;
    Json j = session_to_json(s, true);
    std::string path = cfg_.data_dir + "/" + s.id() + ".json";
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << dump(j);
        if (!out) return;
    }
    std::rename(tmp.c_str(), path.c_str());
}

void SessionManager::load_persisted() {
    if (cfg_.data_dir.empty()) return;
    DIR* dir = ::opendir(cfg_.data_dir.c_str());
    if (!dir) return;
    std::vector<std::string> files;
    while (dirent* e = ::readdir(dir)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) files.push_back(n);
    }
    ::closedir(dir);
    for (const auto& f : files) {
        std::ifstream in(cfg_.data_dir + "/" + f);
        Json j;
        try {
            j = Json::parse(in);
        } catch (...) {
            std::fprintf(stderr, "warning: skipping unreadable session file %s\n", f.c_str());
            continue;
        }
        std::string id = j.value("id", "");
        if (!valid_session_id(id)) continue;
        std::string e;
        auto cfg = j.contains("config") ? config_from_json(j["config"], &e) : std::nullopt;
        SessionConfig c = cfg.value_or(SessionConfig{});
        if (!cfg && j.contains("config")) c.executable = j["config"].value("executable", "");
        auto s = std::make_shared<Session>(id, c, j.value("created_ms", 0LL));
        auto st = session_state_from_string(j.value("state", "FAILED")).value_or(SessionState::Failed);
        if (is_terminal(st)) {
            s->restore(st, j.value("message", ""));
        } else {
            s->restore(SessionState::Failed, "interrupted by server restart");  // was mid-run
        }
        s->imported = j.value("imported", false);
        if (j.contains("result")) {
            std::string err;
            if (auto r = result_from_json(j["result"], &err)) {
                analyze_result(*r);
                s->set_result(std::make_shared<SessionResult>(std::move(*r)));
            }
        }
        std::lock_guard lk(mu_);
        sessions_[id] = s;
    }
}

// ------------------------------------------------------------------ ApiServer
ApiServer::ApiServer(ServerConfig cfg) : cfg_(std::move(cfg)) {
    manager_ = std::make_unique<SessionManager>(cfg_);
    svr_ = std::make_unique<httplib::Server>();
    routes();
}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::bind() {
    if (cfg_.port == 0) {
        port_ = svr_->bind_to_any_port(cfg_.host);
        return port_ > 0;
    }
    if (!svr_->bind_to_port(cfg_.host, cfg_.port)) return false;
    port_ = cfg_.port;
    return true;
}

void ApiServer::listen() { svr_->listen_after_bind(); }

void ApiServer::start_background() {
    thread_ = std::thread([this] { svr_->listen_after_bind(); });
    svr_->wait_until_ready();
}

void ApiServer::stop() {
    if (svr_) svr_->stop();
    if (thread_.joinable()) thread_.join();
    if (manager_) manager_->shutdown();
}

void ApiServer::routes() {
    auto& svr = *svr_;
    auto* mgr = manager_.get();
    const bool loopback = cfg_.host == "127.0.0.1" || cfg_.host == "localhost" || cfg_.host == "::1";

    svr.set_payload_max_length(64 * 1024 * 1024);
    // Defence against DNS rebinding / cross-site requests: only accept loopback Host headers
    // when bound to loopback, and require JSON content type for state-changing requests.
    svr.set_pre_routing_handler([loopback](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) != 0) return httplib::Server::HandlerResponse::Unhandled;
        if (loopback) {
            std::string host = req.get_header_value("Host");
            auto colon = host.rfind(':');
            std::string name = (colon != std::string::npos && host.find(']') == std::string::npos) ? host.substr(0, colon) : host;
            if (!name.empty() && name != "127.0.0.1" && name != "localhost" && name != "[::1]") {
                send_error(res, 403, "forbidden_host", "requests must target localhost");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        // Runs before the body is read, so decide from headers: any request body must be JSON.
        bool has_body = req.has_header("Content-Type") ||
                        (req.has_header("Content-Length") && req.get_header_value("Content-Length") != "0");
        if ((req.method == "POST" || req.method == "DELETE") && has_body &&
            req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
            send_error(res, 415, "unsupported_media_type", "use Content-Type: application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try { if (ep) std::rethrow_exception(ep); } catch (const std::exception& e) { msg = e.what(); } catch (...) {}
        send_error(res, 500, "internal_error", msg);
    });

    auto with_session = [mgr](const httplib::Request& req, httplib::Response& res) -> std::shared_ptr<Session> {
        const std::string& id = req.matches[1];
        auto s = mgr->get(id);
        if (!s) send_error(res, 404, "not_found", "no such session");
        return s;
    };
    auto with_result = [with_session](const httplib::Request& req, httplib::Response& res)
        -> std::pair<std::shared_ptr<Session>, std::shared_ptr<const SessionResult>> {
        auto s = with_session(req, res);
        if (!s) return {};
        auto r = s->result();
        if (!r) {
            send_error(res, 409, "no_result", std::string("session has no results yet (state ") + to_string(s->state()) + ")");
            return {};
        }
        return {s, r};
    };
    auto with_profile = [with_result](const httplib::Request& req, httplib::Response& res)
        -> std::shared_ptr<const SessionResult> {
        auto [s, r] = with_result(req, res);
        if (!r) return nullptr;
        if (!r->profile) {
            send_error(res, 404, "no_profile",
                       r->collection.sampling_error.empty() ? "this session has no sampling profile"
                                                            : "no sampling profile: " + r->collection.sampling_error);
            return nullptr;
        }
        return r;
    };
    auto parse_tid = [](const httplib::Request& req, std::optional<uint32_t>* out) -> bool {
        if (!req.has_param("tid") || req.get_param_value("tid").empty()) return true;
        try {
            unsigned long v = std::stoul(req.get_param_value("tid"));
            if (v > UINT32_MAX) return false;
            *out = static_cast<uint32_t>(v);
            return true;
        } catch (...) { return false; }
    };

    svr.Get("/api/health", [mgr](const httplib::Request&, httplib::Response& res) {
        auto caps = probe_sampler_capabilities();
        send_json(res, {{"status", "ok"},
                        {"version", "0.1.0"},
                        {"sessions", mgr->list().size()},
                        {"queue_pending", mgr->queue_pending()},
                        {"queue_capacity", mgr->config().queue_capacity},
                        {"workers", mgr->config().workers},
                        {"allowed_dirs", mgr->config().allowed_dirs},
                        {"capabilities", {{"perf_event_open", caps.perf_event_open_available},
                                          {"software_cpu_clock", caps.software_cpu_clock},
                                          {"hardware_counters", caps.hardware_counters},
                                          {"perf_event_paranoid", caps.perf_event_paranoid},
                                          {"max_stack", caps.max_stack},
                                          {"allocation_tracking", !mgr->config().profiler.alloc_lib_path.empty() &&
                                                                      ::access(mgr->config().profiler.alloc_lib_path.c_str(), R_OK) == 0},
                                          {"summary", caps.summary}}}});
    });

    svr.Get("/api/targets", [mgr](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"allowed_dirs", mgr->config().allowed_dirs}, {"targets", mgr->list_targets()}});
    });

    svr.Get("/api/sessions", [mgr](const httplib::Request&, httplib::Response& res) {
        Json arr = Json::array();
        for (const auto& s : mgr->list()) arr.push_back(session_to_json(*s, false));
        send_json(res, {{"sessions", arr}});
    });

    svr.Post("/api/sessions", [mgr](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const std::exception& e) {
            return send_error(res, 400, "invalid_json", e.what());
        }
        bool autostart = req.has_param("start") && req.get_param_value("start") == "1";
        std::string err;
        auto cfg = config_from_json(body, &err);
        if (!cfg) return send_error(res, 400, "invalid_config", err);
        SessionManager::Error e;
        auto s = mgr->create(*cfg, &e);
        if (!s) return send_error(res, e.status, e.code, e.message);
        if (autostart && !mgr->start(s->id(), &e)) return send_error(res, e.status, e.code, e.message);
        send_json(res, session_to_json(*s, false), 201);
    });

    svr.Post("/api/import", [mgr](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const std::exception& e) {
            return send_error(res, 400, "invalid_json", e.what());
        }
        const Json& rj = body.contains("result") ? body["result"] : body;
        std::string err;
        auto r = result_from_json(rj, &err);
        if (!r) return send_error(res, 400, "invalid_session", err);
        SessionConfig cfg;
        if (body.contains("config") && body["config"].is_object()) {
            cfg.executable = body["config"].value("executable", "");
            cfg.label = body["config"].value("label", "");
            auto a = body["config"].value("args", std::vector<std::string>{});
            cfg.args = a;
        }
        SessionManager::Error e;
        auto s = mgr->import_result(std::move(*r), cfg, req.has_param("label") ? req.get_param_value("label") : "", &e);
        if (!s) return send_error(res, e.status, e.code, e.message);
        send_json(res, session_to_json(*s, false), 201);
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+))", [with_session](const httplib::Request& req, httplib::Response& res) {
        if (auto s = with_session(req, res)) send_json(res, session_to_json(*s, false));
    });

    svr.Delete(R"(/api/sessions/([a-z0-9-]+))", [mgr](const httplib::Request& req, httplib::Response& res) {
        SessionManager::Error e;
        if (!mgr->remove(req.matches[1], &e)) return send_error(res, e.status, e.code, e.message);
        send_json(res, {{"deleted", std::string(req.matches[1])}});
    });

    svr.Post(R"(/api/sessions/([a-z0-9-]+)/start)", [mgr](const httplib::Request& req, httplib::Response& res) {
        SessionManager::Error e;
        if (!mgr->start(req.matches[1], &e)) return send_error(res, e.status, e.code, e.message);
        send_json(res, session_to_json(*mgr->get(req.matches[1]), false), 202);
    });

    svr.Post(R"(/api/sessions/([a-z0-9-]+)/stop)", [mgr](const httplib::Request& req, httplib::Response& res) {
        SessionManager::Error e;
        if (!mgr->stop(req.matches[1], &e)) return send_error(res, e.status, e.code, e.message);
        send_json(res, session_to_json(*mgr->get(req.matches[1]), false), 202);
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/summary)", [with_result](const httplib::Request& req, httplib::Response& res) {
        auto [s, r] = with_result(req, res);
        if (!r) return;
        Json j = summary_to_json(*r);
        j["process"] = process_to_json(r->process);
        j["process"].erase("stdout");
        j["process"].erase("stderr");
        if (r->profile) {
            j["threads"] = threads_to_json(*r->profile);
            j["modules"] = modules_to_json(*r->profile);
        }
        send_json(res, j);
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/output)", [with_result](const httplib::Request& req, httplib::Response& res) {
        auto [s, r] = with_result(req, res);
        if (!r) return;
        send_json(res, {{"stdout", r->process.stdout_text}, {"stderr", r->process.stderr_text},
                        {"stdout_truncated", r->process.stdout_truncated},
                        {"stderr_truncated", r->process.stderr_truncated}});
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/functions)",
            [with_profile, parse_tid](const httplib::Request& req, httplib::Response& res) {
                auto r = with_profile(req, res);
                if (!r) return;
                FunctionQuery q;
                if (req.has_param("sort")) q.sort = req.get_param_value("sort");
                if (q.sort != "self" && q.sort != "total" && q.sort != "name")
                    return send_error(res, 400, "invalid_param", "sort must be self, total or name");
                if (req.has_param("q")) q.search = req.get_param_value("q").substr(0, 200);
                if (req.has_param("limit")) {
                    try { q.limit = std::clamp<size_t>(std::stoul(req.get_param_value("limit")), 1, 5000); }
                    catch (...) { return send_error(res, 400, "invalid_param", "limit must be a number"); }
                }
                if (!parse_tid(req, &q.tid)) return send_error(res, 400, "invalid_param", "tid must be a number");
                send_json(res, functions_to_json(*r->profile, q));
            });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/functions/([0-9a-f]{16}))",
            [with_profile](const httplib::Request& req, httplib::Response& res) {
                auto r = with_profile(req, res);
                if (!r) return;
                auto idx = r->profile->find_function(req.matches[2]);
                if (!idx) return send_error(res, 404, "not_found", "no such function in this session");
                send_json(res, function_detail_to_json(*r->profile, *idx, r->findings));
            });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/flamegraph)",
            [with_profile, parse_tid](const httplib::Request& req, httplib::Response& res) {
                auto r = with_profile(req, res);
                if (!r) return;
                TreeFilter f;
                f.inverted = req.has_param("view") && req.get_param_value("view") == "inverted";
                if (!parse_tid(req, &f.tid)) return send_error(res, 400, "invalid_param", "tid must be a number");
                if (req.has_param("module") && !req.get_param_value("module").empty())
                    f.module = req.get_param_value("module").substr(0, 512);
                double minf = 0.0001;
                if (req.has_param("min_fraction")) {
                    try { minf = std::clamp(std::stod(req.get_param_value("min_fraction")), 0.0, 0.5); }
                    catch (...) { return send_error(res, 400, "invalid_param", "min_fraction must be a number"); }
                }
                send_json(res, flame_tree_to_json(*r->profile, f, 20000, minf));
            });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/findings)", [with_result](const httplib::Request& req, httplib::Response& res) {
        auto [s, r] = with_result(req, res);
        if (!r) return;
        send_json(res, {{"findings", findings_to_json(r->findings)},
                        {"scoring", "score = 100 × impact × evidence_weight (high 1.0, medium 0.7, low 0.4)"}});
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/instrumentation)", [with_result](const httplib::Request& req, httplib::Response& res) {
        auto [s, r] = with_result(req, res);
        if (!r) return;
        if (!r->instrumentation)
            return send_error(res, 404, "no_instrumentation",
                              r->collection.instrumentation_error.empty() ? "session was not run in instrumentation mode"
                                                                          : r->collection.instrumentation_error);
        send_json(res, instrumentation_to_json(*r->instrumentation));
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/memory)", [with_result](const httplib::Request& req, httplib::Response& res) {
        auto [s, r] = with_result(req, res);
        if (!r) return;
        if (!r->memory)
            return send_error(res, 404, "no_memory_data",
                              r->collection.allocation_error.empty() ? "allocation tracking was not enabled"
                                                                     : r->collection.allocation_error);
        Json j = memory_to_json(*r->memory);
        j["max_rss_kb"] = r->process.max_rss_kb;
        send_json(res, j);
    });

    svr.Get(R"(/api/sessions/([a-z0-9-]+)/export)", [with_result](const httplib::Request& req, httplib::Response& res) {
        auto [s, r] = with_result(req, res);
        if (!r) return;
        std::string fmt = req.has_param("format") ? req.get_param_value("format") : "json";
        // Export content is generated in memory from session data; no file paths are accepted.
        if (fmt == "json") {
            res.set_header("Content-Disposition", "attachment; filename=\"" + s->id() + ".json\"");
            send_json(res, session_to_json(*s, true));
        } else if (fmt == "csv") {
            if (!r->profile) return send_error(res, 404, "no_profile", "no sampling profile to export");
            res.set_header("Content-Disposition", "attachment; filename=\"" + s->id() + "-functions.csv\"");
            res.set_content(functions_csv(*r->profile), "text/csv");
        } else if (fmt == "svg") {
            if (!r->profile) return send_error(res, 404, "no_profile", "no sampling profile to export");
            FlameSvgOptions o;
            o.title = "Performance Detective — " + s->config().executable.substr(s->config().executable.rfind('/') + 1);
            o.subtitle = std::to_string(r->profile->total_samples()) + " samples (cpu-clock, estimates)";
            res.set_header("Content-Disposition", "attachment; filename=\"" + s->id() + "-flamegraph.svg\"");
            res.set_content(render_flame_svg(*r->profile, r->profile->tree(), o), "image/svg+xml");
        } else {
            send_error(res, 400, "invalid_param", "format must be json, csv or svg");
        }
    });

    svr.Get("/api/compare", [mgr](const httplib::Request& req, httplib::Response& res) {
        auto a = mgr->get(req.get_param_value("baseline"));
        auto b = mgr->get(req.get_param_value("candidate"));
        if (!a || !b) return send_error(res, 404, "not_found", "baseline and candidate must be existing session ids");
        auto ra = a->result(), rb = b->result();
        if (!ra || !rb) return send_error(res, 409, "no_result", "both sessions must have results");
        Json j = comparison_to_json(compare_results(*ra, *rb));
        j["baseline"] = session_to_json(*a, false);
        j["candidate"] = session_to_json(*b, false);
        send_json(res, j);
    });

    if (!cfg_.frontend_dir.empty()) {
        svr.set_mount_point("/", cfg_.frontend_dir);
        svr.set_file_extension_and_mimetype_mapping("js", "text/javascript");
        svr.set_file_extension_and_mimetype_mapping("mjs", "text/javascript");
    }
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0 && res.body.empty())
            send_error(res, res.status, res.status == 404 ? "not_found" : "error", "no such endpoint");
    });
}

}  // namespace detective
