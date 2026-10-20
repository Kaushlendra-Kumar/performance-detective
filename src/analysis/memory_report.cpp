#include "detective/reports.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace detective {

using nlohmann::json;

namespace {

std::optional<MappingRecord> parse_maps_line(const std::string& line) {
    std::istringstream ss(line);
    std::string range, perms, offset, dev, inode, path;
    ss >> range >> perms >> offset >> dev >> inode;
    std::getline(ss, path);
    auto first = path.find_first_not_of(' ');
    path = first == std::string::npos ? "" : path.substr(first);
    auto dash = range.find('-');
    if (dash == std::string::npos) return std::nullopt;
    try {
        MappingRecord m;
        m.start = std::stoull(range.substr(0, dash), nullptr, 16);
        m.length = std::stoull(range.substr(dash + 1), nullptr, 16) - m.start;
        m.pgoff = std::stoull(offset, nullptr, 16);
        m.path = path;
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

std::optional<MemoryReport> load_memory_report(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "allocation report not found (static binary, or the target did not exit normally)";
        return std::nullopt;
    }
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        if (error) *error = std::string("malformed allocation report: ") + e.what();
        return std::nullopt;
    }
    try {
        MemoryReport r;
        r.malloc_calls = j.at("malloc").get<uint64_t>();
        r.calloc_calls = j.at("calloc").get<uint64_t>();
        r.realloc_calls = j.at("realloc").get<uint64_t>();
        r.memalign_calls = j.at("memalign").get<uint64_t>();
        r.new_calls = j.at("new").get<uint64_t>();
        r.free_calls = j.at("free").get<uint64_t>();
        r.total_allocations = r.malloc_calls + r.calloc_calls + r.realloc_calls + r.memalign_calls + r.new_calls;
        r.bytes_requested = j.at("bytes_requested").get<uint64_t>();
        r.peak_live_bytes = static_cast<uint64_t>(std::max<int64_t>(0, j.at("peak_live").get<int64_t>()));
        r.live_bytes_at_exit = static_cast<uint64_t>(std::max<int64_t>(0, j.at("live_at_exit").get<int64_t>()));
        r.sites_overflowed = j.value("site_overflow", 0ULL);
        r.peak_error_bytes_per_thread = j.value("live_flush_bytes", 0ULL);
        for (const auto& h : j.at("histogram")) r.size_histogram.push_back(h.get<uint64_t>());
        while (!r.size_histogram.empty() && r.size_histogram.back() == 0) r.size_histogram.pop_back();

        SymbolResolver resolver;
        for (const auto& l : j.value("maps", json::array())) {
            if (auto m = parse_maps_line(l.get<std::string>())) resolver.add_mapping(*m);
        }
        std::map<std::string, AllocSite> merged;  // by function+module
        for (const auto& s : j.at("sites")) {
            uint64_t addr = s.at(0).get<uint64_t>();
            ResolvedFrame f = resolver.resolve(0, addr ? addr - 1 : 0);
            std::string key = f.module + "!" + f.function;
            auto& site = merged[key];
            if (site.count == 0) {
                site.address = addr;
                site.function = f.function;
                site.module = f.module;
            }
            site.count += s.at(1).get<uint64_t>();
            site.bytes += s.at(2).get<uint64_t>();
        }
        for (auto& [k, v] : merged) r.sites.push_back(v);
        std::sort(r.sites.begin(), r.sites.end(), [](const AllocSite& a, const AllocSite& b) {
            return a.count != b.count ? a.count > b.count : a.function < b.function;
        });
        if (r.sites.size() > 200) r.sites.resize(200);
        r.scope_note = "Heap allocations made through malloc-family functions and operator new in dynamically "
                       "linked code. Call site = immediate caller of the allocation function.";
        return r;
    } catch (const std::exception& e) {
        if (error) *error = std::string("invalid allocation report: ") + e.what();
        return std::nullopt;
    }
}

}  // namespace detective
