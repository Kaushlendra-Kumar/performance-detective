#include "detective/flame_graph.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace detective {

FlameLayout layout_flame(const CallTree& tree, uint32_t root_node, double min_width) {
    FlameLayout out;
    if (root_node >= tree.nodes.size()) return out;
    const uint64_t root_total = tree.nodes[root_node].total;
    out.total = root_total;
    if (root_total == 0) return out;

    struct Item { uint32_t node; uint32_t depth; double x; };
    std::vector<Item> stack{{root_node, 0, 0.0}};
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        const CallNode& n = tree.nodes[it.node];
        double w = static_cast<double>(n.total) / static_cast<double>(root_total);
        out.rects.push_back({it.node, n.func, it.depth, it.x, w, n.total, n.self});
        out.max_depth = std::max(out.max_depth, it.depth);
        // Push children in reverse so they are emitted left-to-right.
        double cx = it.x;
        std::vector<Item> kids;
        for (uint32_t c : n.children) {
            double cw = static_cast<double>(tree.nodes[c].total) / static_cast<double>(root_total);
            if (cw < min_width) {
                ++out.pruned;
            } else {
                kids.push_back({c, it.depth + 1, cx});
            }
            cx += cw;
        }
        for (auto k = kids.rbegin(); k != kids.rend(); ++k) stack.push_back(*k);
    }
    return out;
}

std::string xml_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') o += '?';
                else o += c;
        }
    }
    return o;
}

namespace {
// Deterministic warm palette keyed by function name; hue carries no measurement meaning.
std::string color_for(const std::string& name, const std::string& module) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    bool user = module.find(".so") == std::string::npos && module.rfind('[', 0) != 0;
    int r = user ? 205 + static_cast<int>(h % 50) : 150 + static_cast<int>(h % 40);
    int g = user ? 80 + static_cast<int>((h >> 8) % 110) : 150 + static_cast<int>((h >> 8) % 40);
    int b = user ? 40 + static_cast<int>((h >> 16) % 40) : 160 + static_cast<int>((h >> 16) % 50);
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}
}  // namespace

std::string render_flame_svg(const Profile& profile, const CallTree& tree, const FlameSvgOptions& opt) {
    double min_w = opt.width_px > 0 ? opt.min_width_px / opt.width_px : 0;
    FlameLayout lay = layout_flame(tree, 0, min_w);
    const int header = 44;
    int height = header + static_cast<int>(lay.max_depth + 1) * opt.row_px + 10;
    std::ostringstream s;
    s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << opt.width_px << "\" height=\"" << height
      << "\" font-family=\"monospace\" font-size=\"11\">\n"
      << "<rect width=\"100%\" height=\"100%\" fill=\"#0f1115\"/>\n"
      << "<text x=\"10\" y=\"18\" fill=\"#e6e6e6\" font-size=\"14\">" << xml_escape(opt.title) << "</text>\n"
      << "<text x=\"10\" y=\"34\" fill=\"#9aa0a6\">" << xml_escape(opt.subtitle) << "</text>\n";
    const auto& funcs = profile.functions();
    for (const auto& r : lay.rects) {
        double x = r.x * opt.width_px;
        double w = r.width * opt.width_px;
        // Root at the bottom (classic flame graph orientation).
        int y = height - 10 - static_cast<int>(r.depth + 1) * opt.row_px;
        std::string name = r.func < 0 ? "all" : funcs[static_cast<size_t>(r.func)].name;
        std::string module = r.func < 0 ? "" : funcs[static_cast<size_t>(r.func)].module;
        double pct = lay.total ? 100.0 * static_cast<double>(r.value) / static_cast<double>(lay.total) : 0;
        char tip[96];
        std::snprintf(tip, sizeof tip, " — %llu samples (%.2f%% est.), self %llu",
                      static_cast<unsigned long long>(r.value), pct, static_cast<unsigned long long>(r.self));
        s << "<g><title>" << xml_escape(name + (module.empty() ? "" : " [" + module + "]") + tip) << "</title>"
          << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << std::max(0.0, w - 0.5) << "\" height=\""
          << opt.row_px - 1 << "\" fill=\"" << (r.func < 0 ? "#5f6368" : color_for(name, module)) << "\" rx=\"1\"/>";
        size_t chars = w > 20 ? static_cast<size_t>((w - 6) / 6.6) : 0;
        if (chars >= 3) {
            std::string label = name.size() > chars ? name.substr(0, chars - 2) + ".." : name;
            s << "<text x=\"" << x + 3 << "\" y=\"" << y + opt.row_px - 5 << "\" fill=\"#111\">" << xml_escape(label)
              << "</text>";
        }
        s << "</g>\n";
    }
    s << "</svg>\n";
    return s.str();
}

}  // namespace detective
