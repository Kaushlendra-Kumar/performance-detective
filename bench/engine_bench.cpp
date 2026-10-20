// Engine micro-benchmarks: measures the profiler's own costs so overhead claims are backed
// by numbers (see docs/profiling-methods.md, "Measured overhead").
//
//  1. Instrumentation cost per DETECTIVE_SCOPE (nested scope vs. empty loop)
//  2. Sample-processing throughput: Profile::build over real symbolization of this binary
//  3. Call-tree construction and flame-graph JSON generation
//  4. Peak RSS of the engine during processing
#include "detective/call_tree.hpp"
#include "detective/exporter.hpp"
#include "detective/instrument.hpp"
#include "detective/symbol_resolver.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace detective;
using Clock = std::chrono::steady_clock;

namespace {

__attribute__((noinline)) uint64_t work(uint64_t x) { return x * 6364136223846793005ULL + 1; }
__attribute__((noinline)) uint64_t scoped_work(uint64_t x) {
    DETECTIVE_SCOPE("bench.scope");
    return x * 6364136223846793005ULL + 1;
}
__attribute__((noinline)) void f0() { asm volatile(""); }
__attribute__((noinline)) void f1() { f0(); }
__attribute__((noinline)) void f2() { f1(); }
__attribute__((noinline)) void f3() { f2(); }
__attribute__((noinline)) void f4() { f3(); }

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

std::vector<MappingRecord> self_maps() {
    std::vector<MappingRecord> out;
    std::ifstream in("/proc/self/maps");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string range, perms, off, dev, inode, path;
        ss >> range >> perms >> off >> dev >> inode >> path;
        if (perms.size() < 3 || perms[2] != 'x') continue;
        MappingRecord m;
        m.pid = static_cast<uint32_t>(::getpid());
        auto dash = range.find('-');
        m.start = std::stoull(range.substr(0, dash), nullptr, 16);
        m.length = std::stoull(range.substr(dash + 1), nullptr, 16) - m.start;
        m.pgoff = std::stoull(off, nullptr, 16);
        m.path = path;
        out.push_back(m);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    size_t samples = argc > 1 ? std::stoull(argv[1]) : 1'000'000;
    std::printf("Performance Detective engine benchmark (%zu synthetic samples)\n", samples);

    // 1. Instrumentation overhead.
    const int iters = 5'000'000;
    uint64_t acc = 1;
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) acc = work(acc);
    auto t1 = Clock::now();
    for (int i = 0; i < iters; ++i) acc = scoped_work(acc);
    auto t2 = Clock::now();
    double base_ns = secs(t0, t1) / iters * 1e9, scoped_ns = secs(t1, t2) / iters * 1e9;
    std::printf("instrumentation: %.1f ns/call plain, %.1f ns/call with DETECTIVE_SCOPE -> %.1f ns per scope (checksum %llu)\n",
                base_ns, scoped_ns, scoped_ns - base_ns, static_cast<unsigned long long>(acc & 1));
    instr::reset_for_testing();

    // 2. Sample processing with real symbolization of this binary's own functions.
    void* fns[] = {reinterpret_cast<void*>(&f0), reinterpret_cast<void*>(&f1), reinterpret_cast<void*>(&f2),
                   reinterpret_cast<void*>(&f3), reinterpret_cast<void*>(&f4), reinterpret_cast<void*>(&work),
                   reinterpret_cast<void*>(&scoped_work), reinterpret_cast<void*>(&self_maps)};
    RawProfile raw;
    raw.collected = true;
    raw.sample_period_ns = 1'000'000;
    raw.mappings = self_maps();
    uint64_t x = 88172645463325252ULL;
    for (size_t i = 0; i < samples; ++i) {
        RawSample s;
        s.pid = static_cast<uint32_t>(::getpid());
        s.tid = s.pid + static_cast<uint32_t>(i % 4);
        size_t depth = 4 + (i % 8);
        for (size_t d = 0; d < depth; ++d) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            s.ips.push_back(reinterpret_cast<uint64_t>(fns[x % 8]) + 1 + (x % 3));
        }
        raw.samples.push_back(std::move(s));
    }
    SymbolResolver resolver;
    auto t3 = Clock::now();
    Profile p = Profile::build(raw, resolver, false);
    auto t4 = Clock::now();
    std::printf("aggregation: %zu samples -> %zu functions, %zu distinct stacks in %.3f s (%.2f M samples/s)\n",
                samples, p.functions().size(), p.stacks().size(), secs(t3, t4), samples / secs(t3, t4) / 1e6);

    // 3. Tree + flame JSON.
    auto t5 = Clock::now();
    CallTree t = p.tree();
    auto t6 = Clock::now();
    Json fj = flame_tree_to_json(p, {});
    std::string dumped = fj.dump();
    auto t7 = Clock::now();
    auto fn = functions_to_json(p);
    auto t8 = Clock::now();
    std::printf("call tree: %zu nodes in %.1f ms; flame JSON %zu nodes / %.1f KiB in %.1f ms; function table in %.1f ms\n",
                t.nodes.size(), secs(t5, t6) * 1e3, fj["nodes"].get<size_t>(), dumped.size() / 1024.0,
                secs(t6, t7) * 1e3, secs(t7, t8) * 1e3);

    // 4. Realistic shape: a 3,000-function program with a fixed call graph; 1M samples are
    //    random root-to-leaf walks (stack diversity comparable to a real application).
    {
        const uint32_t nf = 3000;
        std::vector<FunctionStats> funcs(nf);
        for (uint32_t i = 0; i < nf; ++i) funcs[i].name = "fn_" + std::to_string(i);
        std::vector<std::vector<uint32_t>> callees(nf);
        uint64_t r = 0x1234567ULL;
        auto rnd = [&] { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return r; };
        for (uint32_t i = 0; i < nf; ++i)
            for (int k = 0; k < 3; ++k) {
                uint32_t c = i + 1 + static_cast<uint32_t>(rnd() % 40);
                if (c < nf) callees[i].push_back(c);
            }
        std::map<std::vector<uint32_t>, uint64_t> agg;
        for (size_t i = 0; i < samples; ++i) {
            std::vector<uint32_t> st{0};
            while (!callees[st.back()].empty() && st.size() < 60 && rnd() % 10 != 0)
                st.push_back(callees[st.back()][rnd() % callees[st.back()].size()]);
            agg[st] += 1;
        }
        std::vector<StackEntry> stacks;
        for (auto& [k, c] : agg) stacks.push_back({1, c, k});
        auto a0 = Clock::now();
        Profile rp = Profile::from_parts(funcs, stacks, {}, 1'000'000);
        auto a1 = Clock::now();
        CallTree rt = rp.tree();
        auto a2 = Clock::now();
        std::string rj = flame_tree_to_json(rp, {}).dump();
        auto a3 = Clock::now();
        std::printf("realistic (3000 functions): %zu distinct stacks; finalize %.1f ms; tree %zu nodes in %.1f ms; "
                    "flame JSON %.1f KiB in %.1f ms\n", rp.stacks().size(), secs(a0, a1) * 1e3, rt.nodes.size(),
                    secs(a1, a2) * 1e3, rj.size() / 1024.0, secs(a2, a3) * 1e3);
    }

    struct rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    std::printf("peak RSS of benchmark process: %.1f MiB (includes the raw sample buffer)\n", ru.ru_maxrss / 1024.0);
    return 0;
}
