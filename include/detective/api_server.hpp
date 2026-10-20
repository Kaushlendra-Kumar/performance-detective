// Local HTTP API. Binds to loopback by default and is NOT intended for untrusted networks.
#pragma once

#include "detective/profiler.hpp"
#include "detective/session.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Server; }

namespace detective {

// Bounded FIFO work queue with a fixed number of worker threads.
class WorkQueue {
public:
    WorkQueue(size_t workers, size_t capacity);
    ~WorkQueue();
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;
    bool try_push(std::function<void()> job);  // false if full or shutting down
    size_t pending() const;
    size_t capacity() const { return capacity_; }
    size_t workers() const { return threads_.size(); }
    void shutdown();

private:
    void loop();
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    size_t capacity_;
    bool stopping_ = false;
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    int port = 7878;
    std::vector<std::string> allowed_dirs;  // executables must live under one of these
    std::string frontend_dir;
    std::string data_dir;  // persisted session JSON; empty = in-memory only
    ProfilerOptions profiler;
    size_t workers = 1;  // profiling jobs run one at a time by default (less interference)
    size_t queue_capacity = 16;
    size_t max_sessions = 1000;
};

class SessionManager {
public:
    SessionManager(ServerConfig cfg);
    ~SessionManager();

    struct Error { int status; std::string code; std::string message; };

    std::shared_ptr<Session> create(const SessionConfig& cfg, Error* err);
    std::shared_ptr<Session> get(const std::string& id) const;
    std::vector<std::shared_ptr<Session>> list() const;
    bool start(const std::string& id, Error* err);
    bool stop(const std::string& id, Error* err);
    bool remove(const std::string& id, Error* err);
    std::shared_ptr<Session> import_result(SessionResult r, SessionConfig cfg, const std::string& label, Error* err);
    void load_persisted();
    std::optional<std::string> check_executable_allowed(const std::string& path, std::string* canonical) const;
    std::vector<std::string> list_targets() const;
    const ServerConfig& config() const { return cfg_; }
    size_t queue_pending() const { return queue_.pending(); }
    void shutdown() { queue_.shutdown(); }

private:
    void persist(const Session& s) const;
    ServerConfig cfg_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    WorkQueue queue_;
};

class ApiServer {
public:
    explicit ApiServer(ServerConfig cfg);
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Binds (port 0 = ephemeral). Returns false on failure.
    bool bind();
    int port() const { return port_; }
    void listen();  // blocking
    void start_background();
    void stop();
    SessionManager& sessions() { return *manager_; }

private:
    void routes();
    ServerConfig cfg_;
    std::unique_ptr<SessionManager> manager_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    int port_ = 0;
};

}  // namespace detective
