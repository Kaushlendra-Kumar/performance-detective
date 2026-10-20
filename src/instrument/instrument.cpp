// Instrumentation runtime. Hot path: one steady_clock read on entry and exit plus
// thread-local bookkeeping; no locks and no allocation in steady state. Per-thread
// data is merged into a global registry once, when the thread exits.
#include "detective/instrument.hpp"

#include <unistd.h>
#include <sys/syscall.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace detective::instr {

namespace {

constexpr size_t kReservoirPerThread = 4096;
constexpr size_t kReservoirGlobal = 65536;

inline int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct SiteStats {
    uint64_t count = 0;
    int64_t total = 0;  // inclusive, outermost activation only (recursion-safe)
    int64_t self = 0;
    int64_t min = INT64_MAX;
    int64_t max = 0;
    uint32_t active = 0;
    uint64_t seen = 0;  // durations offered to the reservoir
    std::vector<int64_t> reservoir;
    uint64_t marks = 0;
    std::map<uint32_t, uint64_t> threads;  // global only
};

struct EdgeStats {
    uint64_t count = 0;
    int64_t total = 0;
};

struct Registry {
    std::mutex mu;
    std::vector<std::string> names;  // by site id
    std::vector<SiteStats> sites;
    std::unordered_map<uint64_t, EdgeStats> edges;
    uint64_t threads = 0;
    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    bool atexit_registered = false;
};

Registry& registry() {
    static Registry* r = new Registry();  // intentionally leaked: must outlive all threads
    return *r;
}

void write_at_exit() {
    if (const char* p = std::getenv("DETECTIVE_INSTR_OUT")) write_report(p);
}

uint64_t xorshift(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

void offer(SiteStats& s, int64_t d, size_t cap, uint64_t& rng) {
    ++s.seen;
    if (s.reservoir.size() < cap) {
        s.reservoir.push_back(d);
    } else {
        // Lemire multiply-shift: uniform in [0, seen) without a 64-bit division.
        __extension__ typedef unsigned __int128 u128;
        uint64_t j = static_cast<uint64_t>((static_cast<u128>(xorshift(rng)) * s.seen) >> 64);
        if (j < cap) s.reservoir[j] = d;
    }
}

struct Frame {
    int32_t id;
    int64_t start;
    int64_t child;
};

thread_local bool t_alive = false;

constexpr size_t kEdgeCache = 256;  // direct-mapped parent->child edge cache per thread
struct EdgeSlot {
    uint64_t key = ~0ULL;
    uint64_t count = 0;
    int64_t total = 0;
};

struct ThreadData {
    std::vector<SiteStats> sites;
    std::vector<Frame> stack;
    std::unordered_map<uint64_t, EdgeStats> edges;  // overflow / evicted edges
    EdgeSlot edge_cache[kEdgeCache];
    uint32_t tid = 0;
    uint64_t rng;
    bool busy = false;

    ThreadData() {
        tid = static_cast<uint32_t>(::syscall(SYS_gettid));
        rng = 0x2545F4914F6CDD1DULL ^ tid;
        stack.reserve(64);
        t_alive = true;
        registry();  // ensure it exists
    }
    ~ThreadData() {
        merge();
        t_alive = false;
    }

    SiteStats& site(int32_t id) {
        if (static_cast<size_t>(id) >= sites.size()) sites.resize(static_cast<size_t>(id) + 1);
        return sites[static_cast<size_t>(id)];
    }

    void spill_edges() {
        for (auto& e : edge_cache) {
            if (e.key == ~0ULL) continue;
            auto& g = edges[e.key];
            g.count += e.count;
            g.total += e.total;
            e = EdgeSlot{};
        }
    }

    void merge() {
        busy = true;
        spill_edges();
        auto& r = registry();
        std::lock_guard lk(r.mu);
        bool any = false;
        for (size_t i = 0; i < sites.size(); ++i) {
            auto& l = sites[i];
            if (l.count == 0 && l.marks == 0) continue;
            any = true;
            if (i >= r.sites.size()) r.sites.resize(i + 1);
            auto& g = r.sites[i];
            g.count += l.count;
            g.total += l.total;
            g.self += l.self;
            g.min = std::min(g.min, l.min);
            g.max = std::max(g.max, l.max);
            g.marks += l.marks;
            if (l.count) g.threads[tid] += l.count;
            for (int64_t d : l.reservoir) offer(g, d, kReservoirGlobal, r.rng);
            uint32_t still_active = l.active;  // scopes open across a manual flush
            l = SiteStats{};
            l.active = still_active;
        }
        for (auto& [k, e] : edges) {
            auto& g = r.edges[k];
            g.count += e.count;
            g.total += e.total;
        }
        edges.clear();
        if (any) ++r.threads;
        busy = false;
    }
};

ThreadData* td() {
    thread_local ThreadData data;
    return t_alive ? &data : nullptr;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

}  // namespace

int32_t register_site(Site& site) noexcept {
    int32_t id = site.id.load(std::memory_order_acquire);
    if (id >= 0) return id;
    auto& r = registry();
    std::lock_guard lk(r.mu);
    id = site.id.load(std::memory_order_relaxed);
    if (id >= 0) return id;
    id = static_cast<int32_t>(r.names.size());
    r.names.emplace_back(site.name ? site.name : "(null)");
    if (!r.atexit_registered) {
        r.atexit_registered = true;
        std::atexit(write_at_exit);
    }
    site.id.store(id, std::memory_order_release);
    return id;
}

Scope::Scope(Site& site) noexcept {
    ThreadData* t = td();
    if (!t || t->busy) return;  // never instrument the profiler itself
    t->busy = true;
    id_ = register_site(site);
    t->site(id_).active++;
    start_ns_ = now_ns();
    t->stack.push_back({id_, start_ns_, 0});
    t->busy = false;
}

Scope::~Scope() {
    if (id_ < 0) return;
    int64_t end = now_ns();
    ThreadData* t = td();
    if (!t || t->stack.empty()) return;
    t->busy = true;
    Frame f = t->stack.back();
    t->stack.pop_back();
    int64_t d = end - f.start;
    auto& s = t->site(f.id);
    s.count++;
    s.self += d - f.child;
    if (--s.active == 0) s.total += d;
    s.min = std::min(s.min, d);
    s.max = std::max(s.max, d);
    offer(s, d, kReservoirPerThread, t->rng);
    if (!t->stack.empty()) {
        Frame& parent = t->stack.back();
        parent.child += d;
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(parent.id)) << 32) | static_cast<uint32_t>(f.id);
        EdgeSlot& slot = t->edge_cache[(key * 0x9E3779B97F4A7C15ULL) >> 56];
        if (slot.key != key) {
            if (slot.key != ~0ULL) {
                auto& e = t->edges[slot.key];
                e.count += slot.count;
                e.total += slot.total;
            }
            slot = EdgeSlot{key, 0, 0};
        }
        slot.count++;
        slot.total += d;
    }
    t->busy = false;
}

void mark(Site& site) noexcept {
    ThreadData* t = td();
    if (!t || t->busy) return;
    t->busy = true;
    int32_t id = register_site(site);
    t->site(id).marks++;
    t->busy = false;
}

void flush_current_thread() noexcept {
    if (ThreadData* t = td()) t->merge();
}

void reset_for_testing() noexcept {
    if (ThreadData* t = td()) {
        t->sites.clear();
        t->edges.clear();
        for (auto& e : t->edge_cache) e = EdgeSlot{};
        t->stack.clear();
    }
    auto& r = registry();
    std::lock_guard lk(r.mu);
    r.sites.clear();
    r.edges.clear();
    r.threads = 0;
}

bool write_report(const char* path) noexcept {
    // Merge the calling thread (normally main, at exit) first.
    if (ThreadData* t = td()) t->merge();
    auto& r = registry();
    // Measure the cost of the clock pair used per scope (overhead estimate).
    int64_t t0 = now_ns();
    int64_t sink = 0;
    for (int i = 0; i < 20000; ++i) sink += now_ns();
    double clock_ns = static_cast<double>(now_ns() - t0) / 20000.0 * 2.0;
    (void)sink;

    std::lock_guard lk(r.mu);
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "{\"version\":1,\"threads\":%llu,\"clock_overhead_ns\":%.2f,\"scopes\":[",
                 static_cast<unsigned long long>(r.threads), clock_ns);
    bool first = true;
    for (size_t i = 0; i < r.sites.size() && i < r.names.size(); ++i) {
        const auto& s = r.sites[i];
        if (s.count == 0) continue;
        std::fprintf(f, "%s{\"name\":\"%s\",\"count\":%llu,\"total_ns\":%lld,\"self_ns\":%lld,\"min_ns\":%lld,"
                        "\"max_ns\":%lld,\"threads\":{",
                     first ? "" : ",", json_escape(r.names[i]).c_str(), static_cast<unsigned long long>(s.count),
                     static_cast<long long>(s.total), static_cast<long long>(s.self), static_cast<long long>(s.min),
                     static_cast<long long>(s.max));
        bool tf = true;
        for (const auto& [tid, c] : s.threads) {
            std::fprintf(f, "%s\"%u\":%llu", tf ? "" : ",", tid, static_cast<unsigned long long>(c));
            tf = false;
        }
        std::fprintf(f, "},\"durations\":[");
        for (size_t k = 0; k < s.reservoir.size(); ++k)
            std::fprintf(f, "%s%lld", k ? "," : "", static_cast<long long>(s.reservoir[k]));
        std::fprintf(f, "]}");
        first = false;
    }
    std::fprintf(f, "],\"edges\":[");
    first = true;
    for (const auto& [k, e] : r.edges) {
        auto p = static_cast<size_t>(k >> 32), c = static_cast<size_t>(k & 0xffffffffu);
        if (p >= r.names.size() || c >= r.names.size()) continue;
        std::fprintf(f, "%s{\"parent\":\"%s\",\"child\":\"%s\",\"count\":%llu,\"total_ns\":%lld}", first ? "" : ",",
                     json_escape(r.names[p]).c_str(), json_escape(r.names[c]).c_str(),
                     static_cast<unsigned long long>(e.count), static_cast<long long>(e.total));
        first = false;
    }
    std::fprintf(f, "],\"marks\":{");
    first = true;
    for (size_t i = 0; i < r.sites.size() && i < r.names.size(); ++i) {
        if (r.sites[i].marks == 0) continue;
        std::fprintf(f, "%s\"%s\":%llu", first ? "" : ",", json_escape(r.names[i]).c_str(),
                     static_cast<unsigned long long>(r.sites[i].marks));
        first = false;
    }
    std::fprintf(f, "}}\n");
    return std::fclose(f) == 0;
}

}  // namespace detective::instr
