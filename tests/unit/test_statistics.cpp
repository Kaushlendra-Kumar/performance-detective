#include "detective/statistics.hpp"

#include <catch_amalgamated.hpp>

using namespace detective::stats;
using Catch::Approx;

TEST_CASE("basic descriptive statistics", "[stats]") {
    std::vector<double> v{1, 2, 3, 4, 100};
    CHECK(mean(v) == Approx(22.0));
    CHECK(median(v) == Approx(3.0));
    CHECK(mad(v) == Approx(1.0));  // |1-3|,|2-3|,0,1,97 -> median 1
    CHECK(percentile(v, 0) == Approx(1));
    CHECK(percentile(v, 100) == Approx(100));
    CHECK(percentile({1, 2, 3, 4}, 50) == Approx(2.5));
    CHECK(stddev({2, 4, 4, 4, 5, 5, 7, 9}) == Approx(2.138).epsilon(0.001));
    CHECK(mean({}) == 0);
    CHECK(median({}) == 0);
    CHECK(stddev({5}) == 0);
}

TEST_CASE("summary and bootstrap CI are deterministic and bracket the median", "[stats]") {
    std::vector<double> v;
    for (int i = 0; i < 30; ++i) v.push_back(10.0 + (i % 7) * 0.1);
    auto a = summarize(v), b = summarize(v);
    CHECK(a.ci95_low == b.ci95_low);
    CHECK(a.ci95_high == b.ci95_high);
    CHECK(a.ci95_low <= a.median);
    CHECK(a.median <= a.ci95_high);
    CHECK(a.n == 30);
    CHECK(a.min == Approx(10.0));
    CHECK(a.max == Approx(10.6));
}

TEST_CASE("Mann-Whitney detects shifted distributions and not identical ones", "[stats]") {
    std::vector<double> a{1.00, 1.02, 0.99, 1.01, 1.03, 0.98, 1.00, 1.02};
    std::vector<double> b{1.20, 1.22, 1.19, 1.21, 1.23, 1.18, 1.20, 1.22};
    auto r = mann_whitney(a, b);
    REQUIRE(r.valid);
    CHECK(r.p_value < 0.01);
    auto same = mann_whitney(a, a);
    CHECK(same.p_value > 0.5);
    auto tiny = mann_whitney({1, 2}, {3, 4});
    CHECK_FALSE(tiny.valid);
    auto ties = mann_whitney({1, 1, 1, 1}, {1, 1, 1, 1});
    CHECK(ties.valid);
    CHECK(ties.p_value == Approx(1.0));
}

TEST_CASE("two-proportion z-test", "[stats]") {
    auto t = two_proportion(500, 1000, 300, 1000);
    REQUIRE(t.valid);
    CHECK(t.diff == Approx(-0.2));
    CHECK(t.p_value < 1e-6);
    auto n = two_proportion(50, 1000, 52, 1000);
    CHECK(n.p_value > 0.5);
    CHECK_FALSE(two_proportion(1, 0, 1, 10).valid);
    CHECK(proportion_stderr(0.5, 100) == Approx(0.05));
}
