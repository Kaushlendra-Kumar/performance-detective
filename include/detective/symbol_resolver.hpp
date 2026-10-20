// ELF symbol table reader and address -> function resolution.
//
// The ELF parser is self-contained (no libelf dependency) and treats every
// input file as untrusted: every offset and size is bounds-checked.
#pragma once

#include "detective/types.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace detective {

struct ElfSymbol {
    uint64_t value = 0;
    uint64_t size = 0;
    std::string name;  // raw (mangled) name
};

struct ElfLoadSegment {
    uint64_t offset = 0;
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
    bool executable = false;
};

class ElfImage {
public:
    // Returns nullopt (and fills *error) if the file cannot be parsed.
    static std::optional<ElfImage> load(const std::string& path, std::string* error = nullptr);
    static std::optional<ElfImage> parse(const std::vector<uint8_t>& bytes, std::string* error = nullptr);

    const ElfSymbol* lookup(uint64_t vaddr) const;
    std::optional<uint64_t> file_offset_to_vaddr(uint64_t offset) const;

    bool has_symtab() const { return has_symtab_; }
    bool has_debug_info() const { return has_debug_info_; }
    bool is_pie_or_shared() const { return type_dyn_; }
    size_t symbol_count() const { return symbols_.size(); }
    const std::vector<ElfLoadSegment>& segments() const { return segments_; }
    // Reads raw file bytes (bounds-checked).
    bool read_bytes(uint64_t offset, size_t n, uint8_t* out) const;
    const std::string& build_id() const { return build_id_; }  // lowercase hex, may be empty
    // Replace this image's symbols with those of a separate debug file (same build).
    void adopt_symbols(const ElfImage& debug);
    bool in_plt(uint64_t vaddr) const;

private:
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    std::string build_id_;
    std::vector<std::pair<uint64_t, uint64_t>> plt_ranges_;  // [start, end) vaddrs of PLT sections
    std::vector<ElfSymbol> symbols_;  // sorted by value
    std::vector<ElfLoadSegment> segments_;
    bool has_symtab_ = false;
    bool has_debug_info_ = false;
    bool type_dyn_ = false;
};

std::string demangle(const std::string& mangled);
// The kernel's vDSO image as mapped into this process (same image for every process).
std::optional<ElfImage> load_own_vdso();
std::string basename_of(const std::string& path);

struct ModuleInfo {
    std::string path;
    bool loaded = false;
    bool has_symtab = false;
    bool has_debug_info = false;
    std::string error;
    std::string debug_file;  // separate symbol file used, if any
};

// Directory searched for separate debug files (<dir>/.build-id/xx/yyyy.debug).
// Defaults to /usr/lib/debug; overridable with DETECTIVE_DEBUG_DIR.
std::string debug_file_dir();

class SymbolResolver {
public:
    void add_mapping(const MappingRecord& m);
    void add_mappings(const std::vector<MappingRecord>& ms);

    // Resolve an address in process `pid`. Thread-safe.
    ResolvedFrame resolve(uint32_t pid, uint64_t ip);

    // Module-relative virtual address for an ip (for source lookups), if mapped.
    std::optional<std::pair<std::string, uint64_t>> module_vaddr(uint32_t pid, uint64_t ip);
    std::optional<uint64_t> function_start_vaddr(uint32_t pid, uint64_t ip);

    // True if `addr` is mapped executable code immediately preceded by an x86-64 call
    // instruction, i.e. it is plausibly a return address.
    bool is_return_address(uint32_t pid, uint64_t addr);
    // If the instruction before `ret_addr` is a direct `call rel32`, its runtime target.
    std::optional<uint64_t> direct_call_target(uint32_t pid, uint64_t ret_addr);

    std::vector<ModuleInfo> modules() const;

private:
    const MappingRecord* find_mapping(uint32_t pid, uint64_t ip) const;
    const ElfImage* image_for(const std::string& path);

    std::vector<MappingRecord> mappings_;
    std::unordered_map<std::string, std::unique_ptr<ElfImage>> images_;
    std::unordered_map<std::string, ModuleInfo> module_info_;
    struct KeyHash {
        size_t operator()(const std::pair<uint32_t, uint64_t>& k) const noexcept {
            return std::hash<uint64_t>()(k.second * 1000003ULL ^ k.first);
        }
    };
    std::unordered_map<std::pair<uint32_t, uint64_t>, ResolvedFrame, KeyHash> cache_;
    mutable std::mutex mu_;
};

// Batch source-location lookup through binutils `addr2line` (optional tool).
// Input: module path -> list of module vaddrs. Output: (path, vaddr) -> "file:line".
// Never uses a shell; returns an empty map if addr2line is unavailable.
std::map<std::pair<std::string, uint64_t>, std::pair<std::string, uint32_t>>
lookup_source_locations(const std::map<std::string, std::vector<uint64_t>>& requests);

}  // namespace detective
