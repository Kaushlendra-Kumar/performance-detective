#include "detective/flame_graph.hpp"

#include "helpers.hpp"

#include <catch_amalgamated.hpp>

#include <map>

using namespace detective;
using Catch::Approx;
using testutil::make_profile;

namespace {
std::map<std::string, FlameRect> by_name(const Profile& p, const FlameLayout& l) {
    std::map<std::string, FlameRect> m;
    for (const auto& r : l.rects) m[r.func < 0 ? "<root>" : p.functions()[static_cast<size_t>(r.func)].name] = r;
    return m;
}
}  // namespace

TEST_CASE("flame layout widths, offsets and depths", "[flame]") {
    // main(10) -> a(6) -> x(4); main -> b(4)
    auto p = make_profile({{"main;a;x", 4}, {"main;a", 2}, {"main;b", 4}});
    auto t = p.tree();
    auto l = layout_flame(t);
    auto m = by_name(p, l);
    CHECK(l.total == 10);
    CHECK(l.max_depth == 3);
    CHECK(m["<root>"].width == Approx(1.0));
    CHECK(m["main"].depth == 1);
    CHECK(m["main"].width == Approx(1.0));
    CHECK(m["a"].x == Approx(0.0));
    CHECK(m["a"].width == Approx(0.6));
    CHECK(m["b"].x == Approx(0.6));  // placed right after its sibling
    CHECK(m["b"].width == Approx(0.4));
    CHECK(m["x"].depth == 3);
    CHECK(m["x"].x == Approx(0.0));
    CHECK(m["x"].width == Approx(0.4));
    CHECK(m["a"].self == 2);
}

TEST_CASE("children never exceed their parent's extent", "[flame]") {
    std::vector<testutil::Spec> specs;
    for (int i = 0; i < 200; ++i)
        specs.push_back({"main;f" + std::to_string(i % 13) + ";g" + std::to_string(i % 7) + ";h" + std::to_string(i % 3),
                         static_cast<uint64_t>(i % 5 + 1)});
    auto p = make_profile(specs);
    auto t = p.tree();
    auto l = layout_flame(t);
    std::map<uint32_t, FlameRect> rect_of;
    for (const auto& r : l.rects) rect_of[r.node] = r;
    for (uint32_t ni = 0; ni < t.nodes.size(); ++ni) {
        const auto& parent = rect_of[ni];
        double used = 0;
        for (uint32_t c : t.nodes[ni].children) {
            const auto& cr = rect_of[c];
            CHECK(cr.depth == parent.depth + 1);
            CHECK(cr.x >= parent.x - 1e-12);
            CHECK(cr.x + cr.width <= parent.x + parent.width + 1e-9);
            used += cr.width;
        }
        // Remaining width is exactly the self cost.
        CHECK(parent.width - used ==
              Approx(static_cast<double>(t.nodes[ni].self) / static_cast<double>(t.total)).margin(1e-9));
    }
}

TEST_CASE("zoomed layout and pruning", "[flame]") {
    auto p = make_profile({{"main;a;x", 98}, {"main;a;tiny", 1}, {"main;b", 1}});
    auto t = p.tree();
    // Find node "a" and lay out from it.
    uint32_t a_node = 0;
    for (uint32_t i = 0; i < t.nodes.size(); ++i)
        if (t.nodes[i].func >= 0 && p.functions()[static_cast<size_t>(t.nodes[i].func)].name == "a") a_node = i;
    auto z = layout_flame(t, a_node);
    CHECK(z.total == 99);
    CHECK(z.rects[0].width == Approx(1.0));
    CHECK(z.rects[0].depth == 0);
    auto pruned = layout_flame(t, 0, 0.05);
    CHECK(pruned.pruned == 2);  // "tiny" and "b" are below 5%
    auto empty = layout_flame(CallTree{});
    CHECK(empty.rects.empty());
}

TEST_CASE("SVG export is well-formed and escapes names", "[flame]") {
    auto p = make_profile({{"main;operator<<(std::ostream&)", 3}, {"main;a&b", 1}});
    FlameSvgOptions o;
    o.title = "t<1>";
    auto svg = render_flame_svg(p, p.tree(), o);
    CHECK(svg.find("<svg") != std::string::npos);
    CHECK(svg.find("</svg>") != std::string::npos);
    CHECK(svg.find("operator&lt;&lt;") != std::string::npos);
    CHECK(svg.find("a&amp;b") != std::string::npos);
    CHECK(svg.find("t&lt;1&gt;") != std::string::npos);
    CHECK(svg.find("a&b") == std::string::npos);
}
