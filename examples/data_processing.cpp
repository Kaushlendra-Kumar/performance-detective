// Workload E: realistic data processing with two implementations.
//
// Generates synthetic "sales" CSV lines (region,product,quantity,price) and
// aggregates revenue per (region, product).
//   --impl naive : std::istringstream splitting, std::string copies, std::map keyed by
//                  a concatenated std::string, std::stod parsing
//   --impl fast  : std::string_view slicing, hand-rolled integer parsing,
//                  std::unordered_map with reserve()
//
// Expected profile (naive): stream extraction / string construction / map lookups
// (std::_Rb_tree, basic_string compare) dominate; fast mode is several times quicker.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define NOINLINE __attribute__((noinline))

static const char* kRegions[] = {"north", "south", "east", "west", "central"};
static const char* kProducts[] = {"widget", "gadget", "doohickey", "gizmo", "sprocket", "flange", "bolt"};

NOINLINE std::vector<std::string> generate(size_t rows) {
    std::vector<std::string> lines;
    lines.reserve(rows);
    uint32_t x = 7;
    char buf[128];
    for (size_t i = 0; i < rows; ++i) {
        x = x * 1664525u + 1013904223u;
        std::snprintf(buf, sizeof buf, "%s,%s,%u,%u.%02u", kRegions[x % 5], kProducts[(x >> 8) % 7],
                      (x >> 16) % 50 + 1, (x >> 4) % 900 + 100, x % 100);
        lines.emplace_back(buf);
    }
    return lines;
}

NOINLINE double process_naive(const std::vector<std::string>& lines) {
    std::map<std::string, double> revenue;
    for (const auto& line : lines) {
        std::istringstream ss(line);
        std::string region, product, qty, price;
        std::getline(ss, region, ',');
        std::getline(ss, product, ',');
        std::getline(ss, qty, ',');
        std::getline(ss, price, ',');
        revenue[region + "/" + product] += std::stod(qty) * std::stod(price);
    }
    double total = 0;
    for (const auto& [k, v] : revenue) total += v;
    return total;
}

static uint64_t parse_uint(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) v = v * 10 + static_cast<uint64_t>(c - '0');
    return v;
}

NOINLINE double process_fast(const std::vector<std::string>& lines) {
    struct Hash {
        size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>()(s); }
    };
    std::unordered_map<std::string_view, int64_t, Hash> cents;  // keys point into `lines`
    cents.reserve(64);
    for (const auto& line : lines) {
        std::string_view sv(line);
        size_t c1 = sv.find(','), c2 = sv.find(',', c1 + 1), c3 = sv.find(',', c2 + 1);
        std::string_view key = sv.substr(0, c2);  // "region,product"
        uint64_t qty = parse_uint(sv.substr(c2 + 1, c3 - c2 - 1));
        std::string_view price = sv.substr(c3 + 1);
        size_t dot = price.find('.');
        uint64_t p = parse_uint(price.substr(0, dot)) * 100 + parse_uint(price.substr(dot + 1));
        cents[key] += static_cast<int64_t>(qty * p);
    }
    int64_t total = 0;
    for (const auto& [k, v] : cents) total += v;
    return static_cast<double>(total) / 100.0;
}

int main(int argc, char** argv) {
    const char* impl = "naive";
    size_t rows = 2'000'000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--impl") && i + 1 < argc) impl = argv[++i];
        else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) rows = std::strtoull(argv[++i], nullptr, 10);
    }
    auto lines = generate(rows);
    double total = !std::strcmp(impl, "fast") ? process_fast(lines) : process_naive(lines);
    std::printf("impl=%s rows=%zu total_revenue=%.2f\n", impl, rows, total);
    return 0;
}
