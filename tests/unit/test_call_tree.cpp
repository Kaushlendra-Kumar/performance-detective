#include "helpers.hpp"

#include <catch_amalgamated.hpp>

using namespace detective;
using testutil::fn;
using testutil::make_profile;

TEST_CASE("self and inclusive costs are computed from stacks", "[calltree]") {
    // main -> a -> b (6), main -> a (2), main -> c (2)
    auto p = make_profile({{"main;a;b", 6}, {"main;a", 2}, {"main;c", 2}});
    CHECK(p.total_samples() == 10);
    CHECK(fn(p, "main").self_samples == 0);
    CHECK(fn(p, "main").total_samples == 10);
    CHECK(fn(p, "a").self_samples == 2);
    CHECK(fn(p, "a").total_samples == 8);
    CHECK(fn(p, "b").self_samples == 6);
    CHECK(fn(p, "b").total_samples == 6);
    CHECK(fn(p, "c").total_samples == 2);
    // Sum of self equals total samples.
    uint64_t self = 0;
    for (const auto& f : p.functions()) self += f.self_samples;
    CHECK(self == p.total_samples());
    CHECK(fn(p, "a").callers.size() == 1);
    CHECK(fn(p, "a").callees.size() == 1);
    CHECK(fn(p, "main").callees.size() == 2);
}

TEST_CASE("recursion is counted once per sample for inclusive cost", "[calltree]") {
    auto p = make_profile({{"main;fib;fib;fib;fib", 10}, {"main;fib;fib", 5}});
    CHECK(fn(p, "fib").total_samples == 15);
    CHECK(fn(p, "fib").self_samples == 15);
    CHECK(fn(p, "fib").max_recursion_depth == 4);
    CHECK(fn(p, "main").max_recursion_depth == 1);
}

TEST_CASE("call tree aggregates identical paths and orders children deterministically", "[calltree]") {
    auto p = make_profile({{"main;z", 1}, {"main;a;x", 3}, {"main;a;x", 2}, {"main;a", 1}});
    auto t = p.tree();
    REQUIRE(t.nodes.size() == 5);  // root, main, a, x, z
    CHECK(t.total == 7);
    const auto& root = t.nodes[0];
    REQUIRE(root.children.size() == 1);
    const auto& main = t.nodes[root.children[0]];
    CHECK(main.total == 7);
    REQUIRE(main.children.size() == 2);
    // Sorted by name: a before z regardless of insertion order.
    CHECK(p.functions()[static_cast<size_t>(t.nodes[main.children[0]].func)].name == "a");
    CHECK(t.nodes[main.children[0]].total == 6);
    CHECK(t.nodes[main.children[0]].self == 1);
    CHECK(t.nodes[main.children[1]].total == 1);
}

TEST_CASE("inverted tree is rooted at leaf functions (self cost view)", "[calltree]") {
    auto p = make_profile({{"main;a;leaf", 4}, {"main;b;leaf", 2}, {"main;b", 3}});
    TreeFilter f;
    f.inverted = true;
    auto t = p.tree(f);
    const auto& root = t.nodes[0];
    REQUIRE(root.children.size() == 2);
    // children: b (self 3) and leaf (self 6)
    uint64_t leaf_total = 0, b_total = 0;
    for (uint32_t c : root.children) {
        auto name = p.functions()[static_cast<size_t>(t.nodes[c].func)].name;
        if (name == "leaf") leaf_total = t.nodes[c].total;
        if (name == "b") b_total = t.nodes[c].total;
    }
    CHECK(leaf_total == 6);
    CHECK(b_total == 3);
}

TEST_CASE("thread and module filters", "[calltree]") {
    auto p = make_profile({{"main;a", 5, 1}, {"main;b", 3, 2}, {"main;libc:malloc", 2, 1}});
    TreeFilter f;
    f.tid = 2;
    CHECK(p.tree(f).total == 3);
    TreeFilter m;
    m.module = "libc.so.6";
    CHECK(p.tree(m).total == 2);
    auto threads = p.threads();
    REQUIRE(threads.size() == 2);
    CHECK(threads[0].tid == 1);
    CHECK(threads[0].samples == 7);
    CHECK(threads[0].name == "main");
    auto mods = p.modules();
    CHECK(mods[0].name == "app");
    CHECK(mods[0].self_samples == 8);
}

TEST_CASE("ranking is deterministic and ids are stable across runs", "[calltree]") {
    auto p1 = make_profile({{"main;a", 5}, {"main;b", 5}, {"main;c", 9}});
    auto p2 = make_profile({{"main;c", 9}, {"main;b", 5}, {"main;a", 5}});
    auto r1 = p1.ranked_by_self();
    auto r2 = p2.ranked_by_self();
    REQUIRE(r1.size() == r2.size());
    for (size_t i = 0; i < r1.size(); ++i) CHECK(p1.functions()[r1[i]].id == p2.functions()[r2[i]].id);
    CHECK(p1.functions()[r1[0]].name == "c");
    CHECK(stable_function_id("m", "f") == stable_function_id("m", "f"));
    CHECK(stable_function_id("m", "f") != stable_function_id("m2", "f"));
    CHECK(fn(p1, "a").id.size() == 16);
}

TEST_CASE("stack completeness classification", "[calltree]") {
    auto p = make_profile({{"_start;main;a", 6}, {"start_thread;w", 2}, {"a;b", 2}});
    CHECK(p.complete_stacks() == 8);
    CHECK(p.incomplete_stacks() == 2);
    CHECK(is_entry_point_symbol("__GI___clone3"));
    CHECK_FALSE(is_entry_point_symbol("main"));
}

TEST_CASE("corrupt aggregated input is rejected safely", "[calltree]") {
    std::vector<FunctionStats> funcs(1);
    funcs[0].name = "f";
    std::vector<StackEntry> stacks{{1, 3, {0}}, {1, 2, {7}}, {1, 0, {0}}, {1, 4, {}}};
    auto p = Profile::from_parts(funcs, stacks, {}, 1000);
    CHECK(p.total_samples() == 3);
    CHECK(p.stacks().size() == 1);
}

TEST_CASE("empty profile", "[calltree]") {
    auto p = Profile::from_parts({}, {}, {}, 1000);
    CHECK(p.total_samples() == 0);
    CHECK(p.tree().nodes.size() == 1);
    CHECK(p.ranked_by_self().empty());
    CHECK(p.threads().empty());
}

TEST_CASE("large sample collections aggregate correctly", "[calltree][reliability]") {
    std::vector<testutil::Spec> specs;
    uint64_t expected = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string s = "main;f" + std::to_string(i % 500) + ";g" + std::to_string(i % 37);
        specs.push_back({s, static_cast<uint64_t>(i % 9 + 1), static_cast<uint32_t>(i % 4 + 1)});
        expected += static_cast<uint64_t>(i % 9 + 1);
    }
    auto p = make_profile(specs);
    CHECK(p.total_samples() == expected);
    CHECK(fn(p, "main").total_samples == expected);
    auto t = p.tree();
    CHECK(t.nodes[0].total == expected);
    uint64_t self_sum = 0;
    for (const auto& n : t.nodes) self_sum += n.self;
    CHECK(self_sum == expected);
}
