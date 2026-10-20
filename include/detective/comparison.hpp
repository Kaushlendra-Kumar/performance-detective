// Baseline vs candidate comparison with explicit uncertainty.
//
// Timing verdicts require repeated benchmark runs (>= 5 per side) and use a
// Mann-Whitney U test plus a minimum practical effect size. Function-level deltas
// compare sample shares with a two-proportion z-test (Bonferroni-corrected).
#pragma once

#include "detective/session.hpp"

#include <string>
#include <vector>

namespace detective {

struct MetricComparison {
    std::string metric;  // "wall_seconds" / "cpu_seconds"
    size_t n_baseline = 0, n_candidate = 0;
    double baseline_median = 0, candidate_median = 0;
    double baseline_ci_low = 0, baseline_ci_high = 0;
    double candidate_ci_low = 0, candidate_ci_high = 0;
    double absolute_change = 0;   // candidate - baseline
    double relative_change = 0;   // (candidate - baseline) / baseline
    double p_value = 1.0;
    bool test_valid = false;
    std::string verdict;  // regression | improvement | no_significant_change | inconclusive
    std::string explanation;
};

struct FunctionDelta {
    std::string id, name, module;
    double baseline_self_share = 0, candidate_self_share = 0;
    double baseline_total_share = 0, candidate_total_share = 0;
    double delta_self_share = 0;  // candidate - baseline (percentage points as fraction)
    double p_value = 1.0;
    bool significant = false;
};

struct ComparisonResult {
    std::vector<MetricComparison> metrics;
    std::vector<FunctionDelta> functions;  // sorted by |delta_self_share| desc
    uint64_t baseline_samples = 0, candidate_samples = 0;
    std::vector<std::string> notes;
};

struct CompareConfig {
    double alpha = 0.05;
    double min_relative_effect = 0.02;  // changes smaller than 2% are reported as no practical change
    size_t min_runs = 5;
    size_t max_functions = 50;
};

ComparisonResult compare_results(const SessionResult& baseline, const SessionResult& candidate,
                                 const CompareConfig& cfg = {});

}  // namespace detective
