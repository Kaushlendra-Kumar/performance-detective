#include "detective/reports.hpp"
#include "detective/statistics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace detective {

using nlohmann::json;

std::optional<InstrumentationReport> load_instrumentation_report(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "instrumentation report not found (was the target linked with detective_instr "
                            "and did it exit normally?)";
        return std::nullopt;
    }
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        if (error) *error = std::string("malformed instrumentation report: ") + e.what();
        return std::nullopt;
    }
    try {
        InstrumentationReport r;
        r.threads = j.value("threads", 0ULL);
        r.clock_overhead_ns = j.value("clock_overhead_ns", 0.0);
        // Merge call sites that share a name.
        std::map<std::string, ScopeStats> by_name;
        std::map<std::string, std::vector<double>> durations;
        for (const auto& s : j.at("scopes")) {
            std::string name = s.at("name").get<std::string>();
            auto& st = by_name[name];
            bool fresh = st.count == 0;
            st.name = name;
            uint64_t c = s.at("count").get<uint64_t>();
            st.count += c;
            st.total_ns += s.at("total_ns").get<double>();
            st.self_ns += s.at("self_ns").get<double>();
            double mn = s.at("min_ns").get<double>(), mx = s.at("max_ns").get<double>();
            st.min_ns = fresh ? mn : std::min(st.min_ns, mn);
            st.max_ns = fresh ? mx : std::max(st.max_ns, mx);
            if (s.contains("threads"))
                for (const auto& [tid, cnt] : s["threads"].items())
                    st.thread_counts[static_cast<uint32_t>(std::stoul(tid))] += cnt.get<uint64_t>();
            auto& d = durations[name];
            for (const auto& v : s.value("durations", json::array())) d.push_back(v.get<double>());
        }
        for (auto& [name, st] : by_name) {
            st.mean_ns = st.count ? st.total_ns / static_cast<double>(st.count) : 0;
            const auto& d = durations[name];
            st.retained_durations = d.size();
            // Percentiles only when enough durations were retained to make them meaningful.
            if (d.size() >= 100) {
                st.percentiles_valid = true;
                st.p50_ns = stats::percentile(d, 50);
                st.p90_ns = stats::percentile(d, 90);
                st.p99_ns = d.size() >= 1000 ? stats::percentile(d, 99) : -1;
            }
            r.scopes.push_back(st);
        }
        std::sort(r.scopes.begin(), r.scopes.end(), [](const ScopeStats& a, const ScopeStats& b) {
            return a.self_ns != b.self_ns ? a.self_ns > b.self_ns : a.name < b.name;
        });
        std::map<std::pair<std::string, std::string>, ScopeEdge> edges;
        for (const auto& e : j.value("edges", json::array())) {
            auto key = std::make_pair(e.at("parent").get<std::string>(), e.at("child").get<std::string>());
            auto& ed = edges[key];
            ed.parent = key.first;
            ed.child = key.second;
            ed.count += e.at("count").get<uint64_t>();
            ed.total_ns += e.at("total_ns").get<double>();
        }
        for (auto& [k, e] : edges) r.edges.push_back(e);
        const auto marks_obj = j.value("marks", json::object());
        for (const auto& [k, v] : marks_obj.items()) r.marks[k] = v.get<uint64_t>();
        return r;
    } catch (const std::exception& e) {
        if (error) *error = std::string("invalid instrumentation report: ") + e.what();
        return std::nullopt;
    }
}

}  // namespace detective
