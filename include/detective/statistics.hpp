// Small, dependency-free statistics toolkit used by benchmarks, comparison and findings.
#pragma once

#include <cstdint>
#include <vector>

namespace detective::stats {

double mean(const std::vector<double>& v);
double stddev(const std::vector<double>& v);  // sample standard deviation (n-1)
double percentile(std::vector<double> v, double p);  // p in [0,100], linear interpolation
double median(const std::vector<double>& v);
double mad(const std::vector<double>& v);  // median absolute deviation (unscaled)

struct Summary {
    size_t n = 0;
    double mean = 0, median = 0, stddev = 0, mad = 0, min = 0, max = 0, p5 = 0, p95 = 0;
    double ci95_low = 0, ci95_high = 0;  // bootstrap CI of the median (deterministic seed)
};
Summary summarize(const std::vector<double>& v);

// Percentile bootstrap CI of the median, fixed seed so results are reproducible.
void bootstrap_median_ci(const std::vector<double>& v, double* lo, double* hi, int iterations = 2000,
                         uint64_t seed = 0x5eed);

// Two-sided Mann-Whitney U test (normal approximation with tie correction).
struct MannWhitney {
    double u = 0;
    double z = 0;
    double p_value = 1.0;
    bool valid = false;  // false if samples are too small for a meaningful test
};
MannWhitney mann_whitney(const std::vector<double>& a, const std::vector<double>& b);

// Two-proportion z-test (e.g. a function's share of samples in run A vs run B).
struct ProportionTest {
    double diff = 0;  // pb - pa
    double z = 0;
    double p_value = 1.0;
    bool valid = false;
};
ProportionTest two_proportion(uint64_t hits_a, uint64_t n_a, uint64_t hits_b, uint64_t n_b);

// Standard error of a sampled proportion.
double proportion_stderr(double p, uint64_t n);

double normal_two_sided_p(double z);

}  // namespace detective::stats
