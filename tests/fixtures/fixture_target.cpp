// Multi-purpose target used by integration tests.
//   --exit N        exit with status N
//   --crash         dereference null (SIGSEGV)
//   --sleep MS      sleep (off-CPU)
//   --spin MS       busy-loop in spin_hot() for MS milliseconds (on-CPU)
//   --spam BYTES    write BYTES bytes to stdout
//   --stderr TEXT   write TEXT to stderr
//   --env NAME      print the value of environment variable NAME
//   --args          echo argv, one per line
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

__attribute__((noinline)) unsigned long spin_hot(int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    unsigned long x = 1;
    while (std::chrono::steady_clock::now() < end)
        for (int i = 0; i < 1000; ++i) x = x * 2862933555777941757UL + 3037000493UL;
    return x;
}

int main(int argc, char** argv) {
    int code = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--exit") code = std::atoi(next());
        else if (a == "--crash") { volatile int* p = nullptr; *p = 1; }
        else if (a == "--sleep") std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(next())));
        else if (a == "--spin") std::printf("spin=%lu\n", spin_hot(std::atoi(next())));
        else if (a == "--spam") {
            long n = std::atol(next());
            std::string chunk(4096, 'x');
            while (n > 0) { long k = n < 4096 ? n : 4096; std::fwrite(chunk.data(), 1, static_cast<size_t>(k), stdout); n -= k; }
        } else if (a == "--stderr") std::fprintf(stderr, "%s", next());
        else if (a == "--env") { const char* v = std::getenv(next()); std::printf("%s\n", v ? v : "(unset)"); }
        else if (a == "--args") for (int j = 0; j < argc; ++j) std::printf("%s\n", argv[j]);
    }
    std::fflush(stdout);
    return code;
}
