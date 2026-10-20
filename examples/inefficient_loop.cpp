// Workload A: an inefficient algorithm next to an efficient alternative.
//
// Task: count how many values in a stream are unique.
//   --mode slow : for every value, linearly scan a std::vector of seen values  -> O(n^2)
//   --mode fast : sort + unique                                                -> O(n log n)
//
// Expected profile (slow): >80% of samples with self cost in `contains_linear`,
// called from `count_unique_slow` <- main.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define NOINLINE __attribute__((noinline))

static std::vector<uint32_t> make_input(size_t n, uint32_t seed) {
    std::vector<uint32_t> v(n);
    uint32_t x = seed;
    for (auto& e : v) {
        x = x * 1664525u + 1013904223u;
        e = x % static_cast<uint32_t>(n);  // plenty of duplicates
    }
    return v;
}

NOINLINE bool contains_linear(const std::vector<uint32_t>& seen, uint32_t value) {
    for (uint32_t s : seen)
        if (s == value) return true;
    return false;
}

NOINLINE size_t count_unique_slow(const std::vector<uint32_t>& input) {
    std::vector<uint32_t> seen;
    for (uint32_t v : input)
        if (!contains_linear(seen, v)) seen.push_back(v);
    return seen.size();
}

NOINLINE size_t count_unique_fast(std::vector<uint32_t> input) {
    std::sort(input.begin(), input.end());
    return static_cast<size_t>(std::unique(input.begin(), input.end()) - input.begin());
}

int main(int argc, char** argv) {
    const char* mode = "slow";
    size_t n = 90000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--n") && i + 1 < argc) n = std::strtoull(argv[++i], nullptr, 10);
    }
    auto input = make_input(n, 42);
    size_t uniq = !std::strcmp(mode, "fast") ? count_unique_fast(input) : count_unique_slow(input);
    std::printf("mode=%s n=%zu unique=%zu\n", mode, n, uniq);
    return 0;
}
