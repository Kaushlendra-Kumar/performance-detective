// Instrumentation (Mode B) example: a toy request pipeline with explicit scopes.
//
// Each request: parse -> cache lookup (MARK) -> on miss "db.query" (CPU work, and
// optionally a short sleep to show wall vs CPU) -> serialize.
// Expected: "db.query" dominates instrumented self time; "checksum_byte" is a very
// frequently called tiny scope (exercises the excessive-calls rule and shows marker overhead).
#include "detective/instrument.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define NOINLINE __attribute__((noinline))

NOINLINE uint32_t checksum_byte(uint32_t acc, unsigned char c) {
    DETECTIVE_FUNCTION();
    return (acc * 31u) ^ c;
}

NOINLINE std::string parse_request(int id) {
    DETECTIVE_SCOPE("request.parse");
    std::string key = "user:" + std::to_string(id % 500);
    uint32_t acc = 0;
    for (unsigned char c : key) acc = checksum_byte(acc, c);
    return key + "#" + std::to_string(acc % 7);
}

NOINLINE uint64_t db_query(const std::string& key, bool sleepy) {
    DETECTIVE_SCOPE("db.query");
    uint64_t x = std::hash<std::string>()(key);
    for (int i = 0; i < 40000; ++i) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    if (sleepy) std::this_thread::sleep_for(std::chrono::microseconds(200));
    return x;
}

NOINLINE std::string serialize(uint64_t v) {
    DETECTIVE_SCOPE("response.serialize");
    char buf[64];
    std::snprintf(buf, sizeof buf, "{\"value\":%llu}", static_cast<unsigned long long>(v));
    return buf;
}

void worker(int first, int count, bool sleepy, size_t* bytes) {
    std::unordered_map<std::string, uint64_t> cache;
    for (int i = first; i < first + count; ++i) {
        DETECTIVE_SCOPE("request.handle");
        std::string key = parse_request(i);
        uint64_t v;
        auto it = cache.find(key);
        if (it != cache.end()) {
            DETECTIVE_MARK("cache.hit");
            v = it->second;
        } else {
            DETECTIVE_MARK("cache.miss");
            v = db_query(key, sleepy);
            cache.emplace(key, v);
        }
        *bytes += serialize(v).size();
    }
}

int main(int argc, char** argv) {
    int requests = 20000;
    int threads = 2;
    bool sleepy = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--requests") && i + 1 < argc) requests = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sleepy")) sleepy = true;
    }
    std::vector<std::thread> pool;
    std::vector<size_t> bytes(static_cast<size_t>(threads), 0);
    int per = requests / threads;
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker, t * per, per, sleepy, &bytes[static_cast<size_t>(t)]);
    for (auto& t : pool) t.join();
    size_t total = 0;
    for (size_t b : bytes) total += b;
    std::printf("requests=%d threads=%d bytes=%zu\n", requests, threads, total);
    return 0;
}
