// Deterministic, rule-based bottleneck detection.
//
// Each finding separates (1) observed facts, (2) a heuristic interpretation and
// (3) suggested next actions, and carries its own limitations. The ranking score is
//   score = 100 * impact * evidence_weight
// where impact is the fraction of measured activity involved (0..1) and
// evidence_weight is 1.0 / 0.7 / 0.4 for high / medium / low confidence.
// See docs/optimization-guide.md.
#pragma once

#include "detective/call_tree.hpp"
#include "detective/reports.hpp"
#include "detective/types.hpp"

#include <string>
#include <vector>

namespace detective {

enum class Severity { Info, Low, Medium, High };
enum class Confidence { Low, Medium, High };
const char* to_string(Severity s);
const char* to_string(Confidence c);
double evidence_weight(Confidence c);

struct Measurement {
    std::string name;
    double value = 0;
    std::string unit;
};

struct Recommendation {
    std::string title;
    std::string observed;
    std::string why_it_matters;
    std::string change_to_investigate;
    std::string expected_benefit;  // qualitative unless measured
    std::string tradeoffs;
    std::string how_to_validate;
    std::string would_disprove;
};

struct Finding {
    std::string id;    // deterministic: kind + affected function id
    std::string kind;  // cpu_hotspot, hot_path, recursion, allocator_cpu, sync_cpu, ...
    std::string title;
    Severity severity = Severity::Info;
    Confidence confidence = Confidence::Low;
    double score = 0;
    double impact = 0;  // 0..1
    std::string source;  // "sampling", "process", "instrumentation", "allocation"
    std::vector<std::string> facts;
    std::string interpretation;
    std::vector<std::string> next_actions;
    std::string remediation;
    std::vector<Measurement> measurements;
    std::vector<std::string> affected_functions;  // function ids (sampling) or scope names
    std::vector<std::string> affected_names;
    std::vector<std::string> limitations;
    std::vector<Recommendation> recommendations;
};

// Coarse classification of well-known runtime functions by name.
enum class FuncCategory { Other, Allocator, Synchronization, StringOps, MemCopy, HashTable, OrderedMap,
                          IoStream, Sorting, Syscall, MathLib };
FuncCategory classify_function(const std::string& name, const std::string& module);
std::string function_base_name(const std::string& demangled);
const char* to_string(FuncCategory c);

struct DetectorConfig {
    double hotspot_self_fraction = 0.10;    // CPU hotspot threshold (self share)
    double hot_path_fraction = 0.50;        // follow children holding >= this share of all samples
    double category_fraction = 0.10;        // allocator / string / hash share threshold
    double sync_fraction = 0.05;
    uint32_t recursion_depth = 16;
    double recursion_fraction = 0.20;
    uint64_t min_samples = 50;              // below this, sampling findings are suppressed
    size_t max_hotspots = 5;
};

struct AnalysisInput {
    const Profile* profile = nullptr;  // sampling (may be null)
    const ProcessResult* process = nullptr;
    const InstrumentationReport* instrumentation = nullptr;
    const MemoryReport* memory = nullptr;
    size_t sampled_threads = 0;
};

std::vector<Finding> detect_bottlenecks(const AnalysisInput& in, const DetectorConfig& cfg = {});

// Optimization recommendation engine: attaches evidence-specific advice to findings.
void attach_recommendations(std::vector<Finding>& findings, const AnalysisInput& in);

}  // namespace detective
