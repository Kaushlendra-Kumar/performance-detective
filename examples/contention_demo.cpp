// Workload D: multithreaded mutex contention.
//
//   --mode coarse : every thread does its computation while holding one global mutex
//                   (threads serialize; most time is spent blocked, i.e. OFF-CPU)
//   --mode fine   : compute outside the lock, hold it only to publish the result
//
// Expected evidence:
//   * coarse: wall time ~= threads x per-thread work, average CPU utilization ~1 core,
//     many voluntary context switches, samples in pthread_mutex_lock / futex paths.
//   * fine: wall time ~= per-thread work / cores, higher CPU utilization.
// Note: cpu-clock sampling cannot see time spent blocked; the evidence is indirect.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define NOINLINE __attribute__((noinline))

static std::mutex g_mutex;
static uint64_t g_total = 0;

NOINLINE uint64_t compute_chunk(uint64_t seed) {
    uint64_t x = seed;
    for (int i = 0; i < 20000; ++i) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    return x;
}

NOINLINE void worker_coarse(int id, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_total += compute_chunk(static_cast<uint64_t>(id * 1000003 + i));
    }
}

NOINLINE void worker_fine(int id, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        uint64_t v = compute_chunk(static_cast<uint64_t>(id * 1000003 + i));
        std::lock_guard<std::mutex> lk(g_mutex);
        g_total += v;
    }
}

int main(int argc, char** argv) {
    const char* mode = "coarse";
    int threads = 4;
    int iterations = 8000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = std::atoi(argv[++i]);
    }
    bool coarse = std::strcmp(mode, "fine") != 0;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(coarse ? worker_coarse : worker_fine, t, iterations);
    for (auto& t : pool) t.join();
    std::printf("mode=%s threads=%d total=%llu\n", mode, threads, static_cast<unsigned long long>(g_total));
    return 0;
}
