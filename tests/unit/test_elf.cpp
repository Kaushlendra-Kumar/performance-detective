#include "detective/symbol_resolver.hpp"

#include <catch_amalgamated.hpp>

#include <fstream>
#include <random>

using namespace detective;

TEST_CASE("demangling and basename", "[elf]") {
    CHECK(demangle("_Z8spin_hoti") == "spin_hot(int)");
    CHECK(demangle("main") == "main");
    CHECK(demangle("_Zgarbage") == "_Zgarbage");
    CHECK(basename_of("/usr/lib/libc.so.6") == "libc.so.6");
    CHECK(basename_of("plain") == "plain");
}

TEST_CASE("ELF symbols of a real binary resolve through a mapping", "[elf]") {
    std::string err;
    auto img = ElfImage::load(DETECTIVE_SELF_EXE_HINT, &err);
    REQUIRE(img);
    CHECK(img->has_symtab());
    CHECK(img->symbol_count() > 0);
    CHECK_FALSE(img->build_id().empty());

    // Find spin_hot's vaddr by scanning the image through lookup on executable segments.
    const ElfLoadSegment* text = nullptr;
    for (const auto& s : img->segments())
        if (s.executable) text = &s;
    REQUIRE(text);
    uint64_t found_vaddr = 0;
    for (uint64_t v = text->vaddr; v < text->vaddr + text->filesz; v += 1) {
        const ElfSymbol* sym = img->lookup(v);
        if (sym && demangle(sym->name) == "spin_hot(int)") { found_vaddr = v; break; }
    }
    REQUIRE(found_vaddr != 0);

    // Map the whole file at a fake base address, as the kernel would for a PIE.
    SymbolResolver r;
    MappingRecord m;
    m.pid = 42;
    m.start = 0x5555'0000'0000ULL;
    m.length = 1 << 24;
    m.pgoff = 0;
    m.path = DETECTIVE_SELF_EXE_HINT;
    r.add_mapping(m);
    uint64_t ip = m.start + (found_vaddr - text->vaddr + text->offset) + 3;
    auto f = r.resolve(42, ip);
    CHECK(f.resolved);
    CHECK(f.function == "spin_hot(int)");
    CHECK(f.module == "fixture_target");
    // Unmapped address.
    auto u = r.resolve(42, 0x10);
    CHECK_FALSE(u.resolved);
    CHECK(u.module == "[unmapped]");
    // Mapping for a missing file degrades to module+offset, never a fabricated name.
    MappingRecord gone = m;
    gone.start = 0x7000'0000'0000ULL;
    gone.path = "/nonexistent/lib.so";
    r.add_mapping(gone);
    auto g = r.resolve(42, gone.start + 0x100);
    CHECK_FALSE(g.resolved);
    CHECK(g.function.find("lib.so+0x") == 0);
    auto mods = r.modules();
    CHECK(mods.size() == 2);
}

TEST_CASE("malformed and truncated ELF data is rejected safely", "[elf][reliability]") {
    std::string err;
    CHECK_FALSE(ElfImage::parse({}, &err));
    CHECK_FALSE(ElfImage::parse({0x7f, 'E', 'L', 'F'}, &err));
    CHECK_FALSE(ElfImage::load("/nonexistent", &err));

    // Load a real binary, then corrupt / truncate it in many ways: must never crash.
    std::ifstream in(DETECTIVE_SELF_EXE_HINT, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 1000);
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 200; ++trial) {
        auto copy = bytes;
        if (trial % 2 == 0) {
            copy.resize(rng() % copy.size());
        } else {
            for (int k = 0; k < 64; ++k) copy[rng() % 256 % copy.size()] = static_cast<uint8_t>(rng());
            for (int k = 0; k < 64; ++k) copy[rng() % copy.size()] = static_cast<uint8_t>(rng());
        }
        auto img = ElfImage::parse(copy, &err);
        if (img) {
            (void)img->lookup(0x1000);
            (void)img->file_offset_to_vaddr(0x1000);
        }
    }
    SUCCEED("no crash on corrupt input");
}
