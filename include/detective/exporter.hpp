// JSON / CSV serialization of sessions and derived views (used by the API, CLI and exports).
#pragma once

#include "detective/comparison.hpp"
#include "detective/session.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace detective {

using Json = nlohmann::json;

Json config_to_json(const SessionConfig& c);
// Parses and validates a session config from untrusted JSON.
std::optional<SessionConfig> config_from_json(const Json& j, std::string* error);

Json process_to_json(const ProcessResult& p);
Json summary_to_json(const SessionResult& r);
Json findings_to_json(const std::vector<Finding>& f);
Json instrumentation_to_json(const InstrumentationReport& r);
Json memory_to_json(const MemoryReport& m);
Json benchmark_to_json(const BenchmarkResult& b);
Json comparison_to_json(const ComparisonResult& c);

struct FunctionQuery {
    std::string sort = "self";  // self | total | name
    std::string search;
    size_t limit = 200;
    std::optional<uint32_t> tid;
};
Json functions_to_json(const Profile& p, const FunctionQuery& q = {});
Json function_detail_to_json(const Profile& p, uint32_t index, const std::vector<Finding>& findings);
Json threads_to_json(const Profile& p);
Json modules_to_json(const Profile& p);

// Nested flame-graph tree. Nodes below `min_fraction` of the total are dropped
// (counted in "pruned"); at most `max_nodes` nodes are emitted.
Json flame_tree_to_json(const Profile& p, const TreeFilter& filter, size_t max_nodes = 20000,
                        double min_fraction = 0.0001);

// Full, re-importable representation (includes aggregated stacks).
Json result_to_json(const SessionResult& r);
std::optional<SessionResult> result_from_json(const Json& j, std::string* error);

Json session_to_json(const Session& s, bool include_full_result);

std::string functions_csv(const Profile& p);

}  // namespace detective
