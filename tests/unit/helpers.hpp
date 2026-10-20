#pragma once

#include "detective/call_tree.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace testutil {

// Builds a Profile from stacks written as "a;b;c" (outermost first), with counts and tids.
struct Spec {
    std::string stack;
    uint64_t count;
    uint32_t tid = 1;
    std::string module = "app";
};

inline detective::Profile make_profile(const std::vector<Spec>& specs, uint64_t period_ns = 1'000'000) {
    std::vector<detective::FunctionStats> funcs;
    std::map<std::pair<std::string, std::string>, uint32_t> index;
    std::vector<detective::StackEntry> stacks;
    for (const auto& sp : specs) {
        detective::StackEntry e;
        e.tid = sp.tid;
        e.count = sp.count;
        size_t start = 0;
        while (start <= sp.stack.size()) {
            size_t end = sp.stack.find(';', start);
            if (end == std::string::npos) end = sp.stack.size();
            std::string name = sp.stack.substr(start, end - start);
            std::string mod = sp.module;
            // "lib:name" puts a frame in libc for category tests.
            if (name.rfind("libc:", 0) == 0) { name = name.substr(5); mod = "libc.so.6"; }
            auto key = std::make_pair(mod, name);
            auto it = index.find(key);
            if (it == index.end()) {
                detective::FunctionStats f;
                f.name = name;
                f.module = mod;
                f.resolved = true;
                it = index.emplace(key, static_cast<uint32_t>(funcs.size())).first;
                funcs.push_back(f);
            }
            e.frames.push_back(it->second);
            start = end + 1;
        }
        stacks.push_back(e);
    }
    return detective::Profile::from_parts(std::move(funcs), std::move(stacks), {{1, "main"}, {2, "worker"}}, period_ns);
}

inline const detective::FunctionStats& fn(const detective::Profile& p, const std::string& name) {
    for (const auto& f : p.functions())
        if (f.name == name) return f;
    static detective::FunctionStats none;
    return none;
}

}  // namespace testutil
