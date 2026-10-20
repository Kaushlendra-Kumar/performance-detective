// Workload B: allocation-heavy program.
//
//   --mode naive  : builds a fresh std::vector<std::string> per record, with many
//                   temporary strings (heap allocations for every non-SSO string)
//   --mode reuse  : reuses buffers across iterations (capacity retained)
//
// Expected evidence:
//   * sampling: a noticeable share of samples inside malloc/free/operator new
//   * allocation tracker (LD_PRELOAD libdetective_alloc.so): millions of allocations
//     in naive mode, orders of magnitude fewer in reuse mode.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define NOINLINE __attribute__((noinline))

NOINLINE std::vector<std::string> tokenize_naive(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            out.push_back(cur);  // copy
            cur = std::string();
        } else {
            cur = cur + c;  // temporary string per character
        }
    }
    out.push_back(cur);
    return out;
}

NOINLINE void tokenize_reuse(const std::string& line, std::vector<std::string>& out) {
    size_t n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            if (n == out.size()) out.emplace_back();
            out[n++].assign(line, start, i - start);
            start = i + 1;
        }
    }
    out.resize(n);
}

NOINLINE std::string make_line(int i) {
    std::string s;
    for (int f = 0; f < 8; ++f) {
        s += "field_value_number_" + std::to_string(i * 8 + f);
        if (f != 7) s += ',';
    }
    return s;
}

int main(int argc, char** argv) {
    const char* mode = "naive";
    int records = 400000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--records") && i + 1 < argc) records = std::atoi(argv[++i]);
    }
    bool naive = std::strcmp(mode, "reuse") != 0;
    size_t checksum = 0;
    std::vector<std::string> fields;
    std::string line;
    for (int i = 0; i < records; ++i) {
        line = make_line(i % 1000);
        if (naive) {
            auto f = tokenize_naive(line);
            checksum += f.size() + f[3].size();
        } else {
            tokenize_reuse(line, fields);
            checksum += fields.size() + fields[3].size();
        }
    }
    std::printf("mode=%s records=%d checksum=%zu\n", mode, records, checksum);
    return 0;
}
