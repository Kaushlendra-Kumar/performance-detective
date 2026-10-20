// Workload C: recursive computation with measurable depth and CPU cost.
//
//   --mode naive : exponential-time recursive Fibonacci (depth ~= n)
//   --mode memo  : memoized recursion (linear)
//   --depth D    : additionally walk a D-deep recursive chain (to exercise deep stacks)
//
// Expected profile (naive): `fib_naive` dominates with self cost spread over many
// recursion levels; max recursion depth reported ~= n.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define NOINLINE __attribute__((noinline))

NOINLINE uint64_t fib_naive(int n) {
    if (n < 2) return static_cast<uint64_t>(n);
    return fib_naive(n - 1) + fib_naive(n - 2);
}

NOINLINE uint64_t fib_memo(int n, std::vector<uint64_t>& memo) {
    if (n < 2) return static_cast<uint64_t>(n);
    if (memo[static_cast<size_t>(n)]) return memo[static_cast<size_t>(n)];
    return memo[static_cast<size_t>(n)] = fib_memo(n - 1, memo) + fib_memo(n - 2, memo);
}

// Recurses `depth` levels, doing a little work at the bottom many times.
NOINLINE uint64_t deep_chain(int depth, uint64_t acc) {
    if (depth == 0) {
        for (int i = 0; i < 2000; ++i) acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        return acc;
    }
    uint64_t r = deep_chain(depth - 1, acc + static_cast<uint64_t>(depth));
    asm volatile("" ::: "memory");  // prevent tail-call elimination
    return r;
}

int main(int argc, char** argv) {
    const char* mode = "naive";
    int n = 40;
    int depth = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--n") && i + 1 < argc) n = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) depth = std::atoi(argv[++i]);
    }
    uint64_t r;
    if (!std::strcmp(mode, "memo")) {
        std::vector<uint64_t> memo(static_cast<size_t>(n) + 1, 0);
        r = fib_memo(n, memo);
    } else {
        r = fib_naive(n);
    }
    uint64_t chain = 0;
    if (depth > 0)
        for (int rep = 0; rep < 20000; ++rep) chain ^= deep_chain(depth, static_cast<uint64_t>(rep));
    std::printf("mode=%s fib(%d)=%llu chain=%llu\n", mode, n, static_cast<unsigned long long>(r),
                static_cast<unsigned long long>(chain));
    return 0;
}
