#include "detective/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace detective::stats {

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0;
    double m = mean(v), acc = 0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / static_cast<double>(v.size() - 1));
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    p = std::clamp(p, 0.0, 100.0);
    double rank = p / 100.0 * static_cast<double>(v.size() - 1);
    auto lo = static_cast<size_t>(std::floor(rank));
    auto hi = static_cast<size_t>(std::ceil(rank));
    double frac = rank - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
}

double median(const std::vector<double>& v) { return percentile(v, 50); }

double mad(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double m = median(v);
    std::vector<double> d;
    d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - m));
    return median(d);
}

namespace {
// xorshift64* — tiny deterministic PRNG for bootstrap resampling.
struct Rng {
    uint64_t s;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
};
}  // namespace

void bootstrap_median_ci(const std::vector<double>& v, double* lo, double* hi, int iterations, uint64_t seed) {
    if (v.empty()) { *lo = *hi = 0; return; }
    if (v.size() == 1) { *lo = *hi = v[0]; return; }
    Rng rng{seed ? seed : 1};
    std::vector<double> meds;
    meds.reserve(static_cast<size_t>(iterations));
    std::vector<double> resample(v.size());
    for (int i = 0; i < iterations; ++i) {
        for (auto& x : resample) x = v[rng.next() % v.size()];
        meds.push_back(median(resample));
    }
    *lo = percentile(meds, 2.5);
    *hi = percentile(meds, 97.5);
}

Summary summarize(const std::vector<double>& v) {
    Summary s;
    s.n = v.size();
    if (v.empty()) return s;
    s.mean = mean(v);
    s.median = median(v);
    s.stddev = stddev(v);
    s.mad = mad(v);
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    s.p5 = percentile(v, 5);
    s.p95 = percentile(v, 95);
    bootstrap_median_ci(v, &s.ci95_low, &s.ci95_high);
    return s;
}

double normal_two_sided_p(double z) { return std::erfc(std::fabs(z) / std::sqrt(2.0)); }

MannWhitney mann_whitney(const std::vector<double>& a, const std::vector<double>& b) {
    MannWhitney r;
    const size_t n1 = a.size(), n2 = b.size();
    if (n1 < 3 || n2 < 3) return r;
    struct Item { double v; int group; };
    std::vector<Item> all;
    for (double x : a) all.push_back({x, 0});
    for (double x : b) all.push_back({x, 1});
    std::sort(all.begin(), all.end(), [](const Item& x, const Item& y) { return x.v < y.v; });
    std::vector<double> ranks(all.size());
    double tie_term = 0;
    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j + 1 < all.size() && all[j + 1].v == all[i].v) ++j;
        double avg = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
        for (size_t k = i; k <= j; ++k) ranks[k] = avg;
        double t = static_cast<double>(j - i + 1);
        tie_term += t * t * t - t;
        i = j + 1;
    }
    double r1 = 0;
    for (size_t i = 0; i < all.size(); ++i) if (all[i].group == 0) r1 += ranks[i];
    double dn1 = static_cast<double>(n1), dn2 = static_cast<double>(n2), n = dn1 + dn2;
    double u1 = r1 - dn1 * (dn1 + 1) / 2.0;
    double mu = dn1 * dn2 / 2.0;
    double sigma2 = dn1 * dn2 / 12.0 * ((n + 1) - tie_term / (n * (n - 1)));
    if (sigma2 <= 0) {  // all values identical
        r.u = u1;
        r.valid = true;
        r.p_value = 1.0;
        return r;
    }
    // Continuity correction.
    double diff = u1 - mu;
    double cc = diff > 0 ? -0.5 : (diff < 0 ? 0.5 : 0.0);
    r.u = u1;
    r.z = (diff + cc) / std::sqrt(sigma2);
    r.p_value = normal_two_sided_p(r.z);
    r.valid = true;
    return r;
}

ProportionTest two_proportion(uint64_t hits_a, uint64_t n_a, uint64_t hits_b, uint64_t n_b) {
    ProportionTest t;
    if (n_a == 0 || n_b == 0) return t;
    double pa = static_cast<double>(hits_a) / static_cast<double>(n_a);
    double pb = static_cast<double>(hits_b) / static_cast<double>(n_b);
    t.diff = pb - pa;
    double pooled = static_cast<double>(hits_a + hits_b) / static_cast<double>(n_a + n_b);
    double se = std::sqrt(pooled * (1 - pooled) * (1.0 / static_cast<double>(n_a) + 1.0 / static_cast<double>(n_b)));
    if (se <= 0) {
        t.valid = true;
        t.p_value = t.diff == 0 ? 1.0 : 0.0;
        return t;
    }
    t.z = t.diff / se;
    t.p_value = normal_two_sided_p(t.z);
    t.valid = true;
    return t;
}

double proportion_stderr(double p, uint64_t n) {
    if (n == 0) return 0;
    return std::sqrt(std::max(0.0, p * (1 - p)) / static_cast<double>(n));
}

}  // namespace detective::stats
