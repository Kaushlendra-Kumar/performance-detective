#include "detective/comparison.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace detective {

namespace {

std::vector<double> timings(const SessionResult& r, bool cpu) {
    if (r.benchmark && !(cpu ? r.benchmark->cpu_seconds : r.benchmark->wall_seconds).empty())
        return cpu ? r.benchmark->cpu_seconds : r.benchmark->wall_seconds;
    // Fall back to the single profiled run (marked inconclusive below).
    if (r.process.started && r.process.exited_normally)
        return {cpu ? r.process.user_cpu_seconds + r.process.sys_cpu_seconds : r.process.wall_seconds};
    return {};
}

MetricComparison compare_metric(const std::string& name, const std::vector<double>& a,
                                const std::vector<double>& b, const CompareConfig& cfg) {
    MetricComparison m;
    m.metric = name;
    m.n_baseline = a.size();
    m.n_candidate = b.size();
    if (a.empty() || b.empty()) {
        m.verdict = "inconclusive";
        m.explanation = "No successful runs on one side.";
        return m;
    }
    auto sa = stats::summarize(a), sb = stats::summarize(b);
    m.baseline_median = sa.median;
    m.candidate_median = sb.median;
    m.baseline_ci_low = sa.ci95_low;
    m.baseline_ci_high = sa.ci95_high;
    m.candidate_ci_low = sb.ci95_low;
    m.candidate_ci_high = sb.ci95_high;
    m.absolute_change = sb.median - sa.median;
    m.relative_change = sa.median > 0 ? m.absolute_change / sa.median : 0;
    if (a.size() < cfg.min_runs || b.size() < cfg.min_runs) {
        m.verdict = "inconclusive";
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "Only %zu vs %zu runs; at least %zu benchmark runs per session are needed before a "
                      "regression or improvement can be claimed.",
                      a.size(), b.size(), cfg.min_runs);
        m.explanation = buf;
        return m;
    }
    auto mw = stats::mann_whitney(a, b);
    m.test_valid = mw.valid;
    m.p_value = mw.p_value;
    bool significant = mw.valid && mw.p_value < cfg.alpha;
    bool practical = std::fabs(m.relative_change) >= cfg.min_relative_effect;
    char buf[200];
    if (significant && practical) {
        m.verdict = m.absolute_change > 0 ? "regression" : "improvement";
        std::snprintf(buf, sizeof buf, "Median changed by %+.1f%% (Mann-Whitney p=%.3g < %.2g).",
                      m.relative_change * 100, m.p_value, cfg.alpha);
    } else if (significant) {
        m.verdict = "no_significant_change";
        std::snprintf(buf, sizeof buf,
                      "Statistically detectable but below the %.0f%% practical threshold (%+.2f%%, p=%.3g).",
                      cfg.min_relative_effect * 100, m.relative_change * 100, m.p_value);
    } else {
        m.verdict = "no_significant_change";
        std::snprintf(buf, sizeof buf, "Difference %+.1f%% is within run-to-run noise (p=%.3g).",
                      m.relative_change * 100, m.p_value);
    }
    m.explanation = buf;
    return m;
}

}  // namespace

ComparisonResult compare_results(const SessionResult& base, const SessionResult& cand, const CompareConfig& cfg) {
    ComparisonResult out;
    out.metrics.push_back(compare_metric("wall_seconds", timings(base, false), timings(cand, false), cfg));
    out.metrics.push_back(compare_metric("cpu_seconds", timings(base, true), timings(cand, true), cfg));
    if (!base.benchmark || !cand.benchmark)
        out.notes.push_back("Timing comparison uses benchmark runs when present; sessions without benchmark runs "
                            "contribute a single profiled run, which cannot establish significance.");
    out.notes.push_back("Benchmark timings come from unprofiled runs; function shares come from sampling "
                        "estimates of the profiled run. The two are not mixed.");

    if (base.profile && cand.profile) {
        const auto& pa = *base.profile;
        const auto& pb = *cand.profile;
        out.baseline_samples = pa.total_samples();
        out.candidate_samples = pb.total_samples();
        struct Pair { const FunctionStats* a = nullptr; const FunctionStats* b = nullptr; };
        std::map<std::string, Pair> by_id;  // stable ids make runs comparable
        for (const auto& f : pa.functions()) by_id[f.id].a = &f;
        for (const auto& f : pb.functions()) by_id[f.id].b = &f;
        double tests = static_cast<double>(std::max<size_t>(1, by_id.size()));
        for (const auto& [id, p] : by_id) {
            FunctionDelta d;
            const FunctionStats* any = p.a ? p.a : p.b;
            d.id = id;
            d.name = any->name;
            d.module = any->module;
            uint64_t sa = p.a ? p.a->self_samples : 0, sb = p.b ? p.b->self_samples : 0;
            uint64_t ta = p.a ? p.a->total_samples : 0, tb = p.b ? p.b->total_samples : 0;
            if (out.baseline_samples) {
                d.baseline_self_share = static_cast<double>(sa) / static_cast<double>(out.baseline_samples);
                d.baseline_total_share = static_cast<double>(ta) / static_cast<double>(out.baseline_samples);
            }
            if (out.candidate_samples) {
                d.candidate_self_share = static_cast<double>(sb) / static_cast<double>(out.candidate_samples);
                d.candidate_total_share = static_cast<double>(tb) / static_cast<double>(out.candidate_samples);
            }
            d.delta_self_share = d.candidate_self_share - d.baseline_self_share;
            auto t = stats::two_proportion(sa, out.baseline_samples, sb, out.candidate_samples);
            d.p_value = std::min(1.0, t.p_value * tests);  // Bonferroni
            d.significant = t.valid && d.p_value < cfg.alpha && std::fabs(d.delta_self_share) >= 0.01;
            if (sa == 0 && sb == 0 && ta == 0 && tb == 0) continue;
            out.functions.push_back(d);
        }
        std::sort(out.functions.begin(), out.functions.end(), [](const FunctionDelta& x, const FunctionDelta& y) {
            double ax = std::max(std::fabs(x.delta_self_share), std::fabs(x.candidate_total_share - x.baseline_total_share));
            double ay = std::max(std::fabs(y.delta_self_share), std::fabs(y.candidate_total_share - y.baseline_total_share));
            return ax != ay ? ax > ay : x.id < y.id;
        });
        if (out.functions.size() > cfg.max_functions) out.functions.resize(cfg.max_functions);
        out.notes.push_back("Function deltas compare shares of each run's own samples (percentage points), "
                            "not absolute time; p-values are Bonferroni-corrected across functions.");
    } else {
        out.notes.push_back("Function-level comparison requires sampling profiles in both sessions.");
    }
    return out;
}

}  // namespace detective
