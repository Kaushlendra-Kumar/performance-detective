// Flame-graph layout (pure function, unit-tested) and standalone SVG rendering for exports.
#pragma once

#include "detective/call_tree.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace detective {

struct FlameRect {
    uint32_t node = 0;
    int32_t func = -1;  // -1 for the root
    uint32_t depth = 0;
    double x = 0;      // [0,1] fraction of the zoom root's width
    double width = 0;  // [0,1]
    uint64_t value = 0;  // inclusive samples
    uint64_t self = 0;
};

struct FlameLayout {
    std::vector<FlameRect> rects;
    uint32_t max_depth = 0;
    uint64_t total = 0;
    size_t pruned = 0;  // nodes skipped because narrower than min_width
};

// Lays out the subtree rooted at `root_node`. Children are placed left-to-right in the
// tree's (deterministic) child order; a child's width is value/root_value. Nodes whose
// width is below `min_width` are skipped together with their descendants.
FlameLayout layout_flame(const CallTree& tree, uint32_t root_node = 0, double min_width = 0.0);

struct FlameSvgOptions {
    int width_px = 1200;
    int row_px = 18;
    double min_width_px = 0.5;
    std::string title = "Flame graph";
    std::string subtitle;
};

// Self-contained SVG (tooltips via <title>), used by the export endpoint.
std::string render_flame_svg(const Profile& profile, const CallTree& tree, const FlameSvgOptions& opt);

std::string xml_escape(const std::string& s);

}  // namespace detective
