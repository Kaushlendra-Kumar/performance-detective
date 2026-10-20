#include "detective/symbol_resolver.hpp"

#include "detective/process_runner.hpp"

#include <cxxabi.h>
#include <elf.h>
#include <sys/auxv.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fstream>
#include <sstream>

namespace detective {

namespace {

constexpr uint64_t kMaxElfBytes = 1ULL << 30;  // refuse to load >1 GiB images

template <typename T>
bool read_struct(const std::vector<uint8_t>& b, uint64_t off, T* out) {
    if (off > b.size() || sizeof(T) > b.size() - off) return false;
    std::memcpy(out, b.data() + off, sizeof(T));
    return true;
}

std::string read_cstr(const std::vector<uint8_t>& b, uint64_t table_off, uint64_t table_size,
                      uint64_t idx) {
    if (idx >= table_size) return {};
    uint64_t start = table_off + idx;
    uint64_t end_limit = std::min<uint64_t>(table_off + table_size, b.size());
    if (start >= end_limit) return {};
    const char* p = reinterpret_cast<const char*>(b.data() + start);
    size_t n = strnlen(p, end_limit - start);
    return std::string(p, n);
}

}  // namespace

std::optional<ElfImage> load_own_vdso() {
    auto base = reinterpret_cast<const uint8_t*>(::getauxval(AT_SYSINFO_EHDR));
    if (!base) return std::nullopt;
    Elf64_Ehdr eh;
    std::memcpy(&eh, base, sizeof eh);
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_phentsize != sizeof(Elf64_Phdr)) return std::nullopt;
    // Image size: the end of the furthest PT_LOAD or section header table.
    uint64_t size = eh.e_shoff + uint64_t(eh.e_shnum) * eh.e_shentsize;
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        std::memcpy(&ph, base + eh.e_phoff + uint64_t(i) * sizeof ph, sizeof ph);
        if (ph.p_type == PT_LOAD) size = std::max<uint64_t>(size, ph.p_offset + ph.p_filesz);
    }
    if (size == 0 || size > (1u << 20)) return std::nullopt;
    std::vector<uint8_t> bytes(base, base + size);
    return ElfImage::parse(bytes, nullptr);
}

std::string demangle(const std::string& mangled) {
    if (mangled.size() < 2 || mangled[0] != '_' || mangled[1] != 'Z') return mangled;
    int status = 0;
    char* out = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status != 0 || out == nullptr) {
        std::free(out);
        return mangled;
    }
    std::string s(out);
    std::free(out);
    return s;
}

std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::optional<ElfImage> ElfImage::load(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return std::nullopt;
    }
    auto size = static_cast<uint64_t>(in.tellg());
    if (size > kMaxElfBytes) {
        if (error) *error = "file too large: " + path;
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(size);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        if (error) *error = "read failed: " + path;
        return std::nullopt;
    }
    auto img = parse(bytes, error);
    if (img) img->bytes_ = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    return img;
}

bool ElfImage::read_bytes(uint64_t offset, size_t n, uint8_t* out) const {
    if (!bytes_ || offset > bytes_->size() || n > bytes_->size() - offset) return false;
    std::memcpy(out, bytes_->data() + offset, n);
    return true;
}

std::optional<ElfImage> ElfImage::parse(const std::vector<uint8_t>& b, std::string* error) {
    auto fail = [&](const char* msg) -> std::optional<ElfImage> {
        if (error) *error = msg;
        return std::nullopt;
    };
    Elf64_Ehdr eh;
    if (!read_struct(b, 0, &eh)) return fail("truncated ELF header");
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return fail("not an ELF file");
    if (eh.e_ident[EI_CLASS] != ELFCLASS64) return fail("only ELF64 is supported");
    if (eh.e_ident[EI_DATA] != ELFDATA2LSB) return fail("only little-endian ELF is supported");

    ElfImage img;
    img.type_dyn_ = eh.e_type == ET_DYN;

    // Program headers (load segments) for file-offset -> vaddr translation.
    if (eh.e_phentsize == sizeof(Elf64_Phdr) && eh.e_phnum < 4096) {
        for (uint16_t i = 0; i < eh.e_phnum; ++i) {
            Elf64_Phdr ph;
            if (!read_struct(b, eh.e_phoff + uint64_t(i) * sizeof(Elf64_Phdr), &ph)) break;
            if (ph.p_type != PT_LOAD) continue;
            img.segments_.push_back({ph.p_offset, ph.p_vaddr, ph.p_filesz, ph.p_memsz,
                                     (ph.p_flags & PF_X) != 0});
        }
    }

    // Section headers: .symtab/.dynsym and their string tables.
    std::vector<Elf64_Shdr> sh;
    if (eh.e_shentsize == sizeof(Elf64_Shdr) && eh.e_shnum < 65535) {
        for (uint16_t i = 0; i < eh.e_shnum; ++i) {
            Elf64_Shdr s;
            if (!read_struct(b, eh.e_shoff + uint64_t(i) * sizeof(Elf64_Shdr), &s)) break;
            sh.push_back(s);
        }
    }
    // Section names: debug info, PLT ranges, build-id note.
    if (eh.e_shstrndx < sh.size()) {
        const auto& names = sh[eh.e_shstrndx];
        for (const auto& s : sh) {
            auto nm = read_cstr(b, names.sh_offset, names.sh_size, s.sh_name);
            if (nm == ".debug_info") img.has_debug_info_ = true;
            if ((nm == ".plt" || nm == ".plt.sec" || nm == ".plt.got") && s.sh_size > 0)
                img.plt_ranges_.emplace_back(s.sh_addr, s.sh_addr + s.sh_size);
            if (nm == ".note.gnu.build-id" && s.sh_type == SHT_NOTE) {
                uint32_t hdr[3];
                if (read_struct(b, s.sh_offset, &hdr) && hdr[0] == 4 && hdr[2] == NT_GNU_BUILD_ID &&
                    hdr[1] <= 64 && s.sh_offset + 16 + hdr[1] <= b.size()) {
                    static const char* hex = "0123456789abcdef";
                    for (uint32_t i = 0; i < hdr[1]; ++i) {
                        uint8_t v = b[s.sh_offset + 16 + i];
                        img.build_id_ += hex[v >> 4];
                        img.build_id_ += hex[v & 15];
                    }
                }
            }
        }
    }

    bool have_symtab = false;
    for (const auto& s : sh) if (s.sh_type == SHT_SYMTAB) have_symtab = true;
    img.has_symtab_ = have_symtab;

    for (const auto& s : sh) {
        if (s.sh_type != SHT_SYMTAB && s.sh_type != SHT_DYNSYM) continue;
        // .dynsym is a subset of .symtab; only use it when .symtab is absent.
        if (s.sh_type == SHT_DYNSYM && have_symtab) continue;
        if (s.sh_entsize != sizeof(Elf64_Sym) || s.sh_link >= sh.size()) continue;
        const auto& strtab = sh[s.sh_link];
        uint64_t count = s.sh_size / sizeof(Elf64_Sym);
        if (count > 10'000'000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            Elf64_Sym sym;
            if (!read_struct(b, s.sh_offset + i * sizeof(Elf64_Sym), &sym)) break;
            int type = ELF64_ST_TYPE(sym.st_info);
            if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
            if (sym.st_value == 0 || sym.st_shndx == SHN_UNDEF) continue;
            auto name = read_cstr(b, strtab.sh_offset, strtab.sh_size, sym.st_name);
            if (name.empty()) continue;
            img.symbols_.push_back({sym.st_value, sym.st_size, std::move(name)});
        }
    }
    std::sort(img.symbols_.begin(), img.symbols_.end(), [](const ElfSymbol& a, const ElfSymbol& c) {
        if (a.value != c.value) return a.value < c.value;
        // Prefer sized, then shortest (non-aliased-looking) name for determinism.
        if (a.size != c.size) return a.size > c.size;
        return a.name < c.name;
    });
    // Drop aliases at the same address (keep the first after sorting).
    img.symbols_.erase(std::unique(img.symbols_.begin(), img.symbols_.end(),
                                   [](const ElfSymbol& a, const ElfSymbol& c) { return a.value == c.value; }),
                       img.symbols_.end());
    return img;
}

void ElfImage::adopt_symbols(const ElfImage& debug) {
    symbols_ = debug.symbols_;
    has_symtab_ = debug.has_symtab_;
    has_debug_info_ = has_debug_info_ || debug.has_debug_info_;
}

bool ElfImage::in_plt(uint64_t vaddr) const {
    for (const auto& [a, e] : plt_ranges_)
        if (vaddr >= a && vaddr < e) return true;
    return false;
}

std::string debug_file_dir() {
    const char* d = std::getenv("DETECTIVE_DEBUG_DIR");
    return d && *d ? d : "/usr/lib/debug";
}

const ElfSymbol* ElfImage::lookup(uint64_t vaddr) const {
    auto it = std::upper_bound(symbols_.begin(), symbols_.end(), vaddr,
                               [](uint64_t v, const ElfSymbol& s) { return v < s.value; });
    if (it == symbols_.begin()) return nullptr;
    --it;
    if (it->size > 0) {
        return vaddr < it->value + it->size ? &*it : nullptr;
    }
    // Zero-sized symbol (hand-written asm): accept only if no later symbol starts first.
    auto next = std::next(it);
    if (next == symbols_.end() || vaddr < next->value) {
        return (vaddr - it->value) < 256 ? &*it : nullptr;
    }
    return nullptr;
}

std::optional<uint64_t> ElfImage::file_offset_to_vaddr(uint64_t offset) const {
    for (const auto& seg : segments_) {
        if (offset >= seg.offset && offset < seg.offset + std::max(seg.filesz, seg.memsz)) {
            return offset - seg.offset + seg.vaddr;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------

void SymbolResolver::add_mapping(const MappingRecord& m) {
    std::lock_guard lk(mu_);
    mappings_.push_back(m);
}

void SymbolResolver::add_mappings(const std::vector<MappingRecord>& ms) {
    std::lock_guard lk(mu_);
    mappings_.insert(mappings_.end(), ms.begin(), ms.end());
}

const MappingRecord* SymbolResolver::find_mapping(uint32_t pid, uint64_t ip) const {
    // Most recent mapping wins (mmap over an existing range replaces it).
    const MappingRecord* fallback = nullptr;
    for (auto it = mappings_.rbegin(); it != mappings_.rend(); ++it) {
        if (ip >= it->start && ip - it->start < it->length) {
            if (it->pid == pid) return &*it;
            if (!fallback) fallback = &*it;
        }
    }
    return fallback;
}

const ElfImage* SymbolResolver::image_for(const std::string& path) {
    auto it = images_.find(path);
    if (it != images_.end()) return it->second.get();
    ModuleInfo info;
    info.path = path;
    std::unique_ptr<ElfImage> img;
    if (!path.empty() && path[0] == '/') {
        std::string err;
        auto loaded = ElfImage::load(path, &err);
        if (loaded) {
            img = std::make_unique<ElfImage>(std::move(*loaded));
            // Stripped module: look for a separate debug file by build-id.
            if (!img->has_symtab() && img->build_id().size() > 2) {
                std::string dbg = debug_file_dir() + "/.build-id/" + img->build_id().substr(0, 2) + "/" +
                                  img->build_id().substr(2) + ".debug";
                if (auto d = ElfImage::load(dbg, nullptr); d && d->has_symtab() && d->build_id() == img->build_id()) {
                    img->adopt_symbols(*d);
                    info.debug_file = dbg;
                }
            }
            info.loaded = true;
            info.has_symtab = img->has_symtab();
            info.has_debug_info = img->has_debug_info();
        } else {
            info.error = err;
        }
    } else if (path == "[vdso]") {
        // The vDSO has no file on disk, but every process on this machine maps the same
        // kernel-provided image; read it from our own address space.
        if (auto vdso = load_own_vdso()) {
            img = std::make_unique<ElfImage>(std::move(*vdso));
            info.loaded = true;
            info.has_symtab = img->symbol_count() > 0;
            info.debug_file = "(read from this process's vDSO mapping)";
        } else {
            info.error = "vDSO image not readable";
        }
    } else {
        info.error = "pseudo mapping (no file)";
    }
    module_info_[path] = info;
    auto* raw = img.get();
    images_[path] = std::move(img);
    return raw;
}

ResolvedFrame SymbolResolver::resolve(uint32_t pid, uint64_t ip) {
    std::lock_guard lk(mu_);
    auto key = std::make_pair(pid, ip);
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;

    ResolvedFrame f;
    const MappingRecord* m = find_mapping(pid, ip);
    if (!m) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "[unknown 0x%llx]", static_cast<unsigned long long>(ip));
        f.function = buf;
        f.module = "[unmapped]";
    } else {
        f.module = m->path.empty() ? "[anon]" : basename_of(m->path);
        const ElfImage* img = image_for(m->path);
        uint64_t file_off = ip - m->start + m->pgoff;
        std::optional<uint64_t> vaddr;
        if (img) vaddr = img->file_offset_to_vaddr(file_off);
        const ElfSymbol* sym = (img && vaddr) ? img->lookup(*vaddr) : nullptr;
        if (img && vaddr && img->in_plt(*vaddr)) {
            f.function = "[plt stub]";
            f.resolved = true;
        } else if (sym) {
            // Drop ELF symbol-version suffixes (e.g. "__libc_start_main@@GLIBC_2.34").
            auto at = sym->name.find('@');
            f.function = demangle(at == std::string::npos ? sym->name : sym->name.substr(0, at));
            f.resolved = true;
        } else {
            char buf[64];
            std::snprintf(buf, sizeof buf, "+0x%llx",
                          static_cast<unsigned long long>(vaddr.value_or(file_off)));
            f.function = f.module + buf;
        }
    }
    cache_.emplace(key, f);
    return f;
}

std::optional<std::pair<std::string, uint64_t>> SymbolResolver::module_vaddr(uint32_t pid, uint64_t ip) {
    std::lock_guard lk(mu_);
    const MappingRecord* m = find_mapping(pid, ip);
    if (!m) return std::nullopt;
    const ElfImage* img = image_for(m->path);
    if (!img) return std::nullopt;
    auto v = img->file_offset_to_vaddr(ip - m->start + m->pgoff);
    if (!v) return std::nullopt;
    return std::make_pair(m->path, *v);
}

std::optional<uint64_t> SymbolResolver::function_start_vaddr(uint32_t pid, uint64_t ip) {
    std::lock_guard lk(mu_);
    const MappingRecord* m = find_mapping(pid, ip);
    if (!m) return std::nullopt;
    const ElfImage* img = image_for(m->path);
    if (!img) return std::nullopt;
    auto v = img->file_offset_to_vaddr(ip - m->start + m->pgoff);
    if (!v) return std::nullopt;
    const ElfSymbol* sym = img->lookup(*v);
    if (!sym) return std::nullopt;
    return sym->value;
}

bool SymbolResolver::is_return_address(uint32_t pid, uint64_t addr) {
    std::lock_guard lk(mu_);
    const MappingRecord* m = find_mapping(pid, addr);
    if (!m) return false;
    const ElfImage* img = image_for(m->path);
    if (!img) return false;
    uint64_t off = addr - m->start + m->pgoff;
    if (off < 7) return false;
    uint8_t b[7];
    if (!img->read_bytes(off - 7, 7, b)) return false;
    // b[6] is the byte just before addr.
    if (b[2] == 0xE8) return true;                                   // call rel32
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) return true;          // call *%reg
    if (b[1] == 0xFF && b[2] == 0x15) return true;                   // call *rel32(%rip)
    if (b[4] == 0xFF && ((b[5] >> 3) & 7) == 2 && (b[5] >> 6) == 1) return true;  // call *disp8(%reg)
    if (b[3] == 0xFF && ((b[4] >> 3) & 7) == 2 && (b[4] >> 6) == 1) return true;  // call *disp8(%reg,sib)
    if (b[1] == 0xFF && ((b[2] >> 3) & 7) == 2 && (b[2] >> 6) == 2) return true;  // call *disp32(%reg)
    if (b[0] == 0xFF && ((b[1] >> 3) & 7) == 2 && (b[1] >> 6) == 2) return true;  // call *disp32(%reg,sib)
    if (b[5] == 0xFF && ((b[6] >> 3) & 7) == 2 && (b[6] >> 6) == 0) return true;  // call *(%reg)
    return false;
}

std::optional<uint64_t> SymbolResolver::direct_call_target(uint32_t pid, uint64_t ret_addr) {
    std::lock_guard lk(mu_);
    const MappingRecord* m = find_mapping(pid, ret_addr);
    if (!m) return std::nullopt;
    const ElfImage* img = image_for(m->path);
    if (!img) return std::nullopt;
    uint64_t off = ret_addr - m->start + m->pgoff;
    if (off < 5) return std::nullopt;
    uint8_t b[5];
    if (!img->read_bytes(off - 5, 5, b) || b[0] != 0xE8) return std::nullopt;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return ret_addr + static_cast<uint64_t>(static_cast<int64_t>(rel));
}

std::vector<ModuleInfo> SymbolResolver::modules() const {
    std::lock_guard lk(mu_);
    std::vector<ModuleInfo> out;
    for (const auto& [k, v] : module_info_) out.push_back(v);
    std::sort(out.begin(), out.end(), [](const ModuleInfo& a, const ModuleInfo& b) { return a.path < b.path; });
    return out;
}

std::map<std::pair<std::string, uint64_t>, std::pair<std::string, uint32_t>>
lookup_source_locations(const std::map<std::string, std::vector<uint64_t>>& requests) {
    std::map<std::pair<std::string, uint64_t>, std::pair<std::string, uint32_t>> out;
    static const char* candidates[] = {"/usr/bin/addr2line", "/bin/addr2line"};
    std::string tool;
    for (const char* c : candidates) {
        if (access(c, X_OK) == 0) { tool = c; break; }
    }
    if (tool.empty()) return out;

    for (const auto& [path, addrs] : requests) {
        if (addrs.empty() || path.empty() || path[0] != '/') continue;
        for (size_t base = 0; base < addrs.size(); base += 200) {
            std::vector<std::string> argv = {tool, "-e", path};
            size_t end = std::min(addrs.size(), base + 200);
            for (size_t i = base; i < end; ++i) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(addrs[i]));
                argv.emplace_back(buf);
            }
            auto text = run_and_capture(argv, 1 << 20, 10'000);
            if (!text) break;
            std::istringstream ss(*text);
            std::string line;
            size_t i = base;
            while (std::getline(ss, line) && i < end) {
                auto colon = line.rfind(':');
                if (colon != std::string::npos && line.rfind("??", 0) != 0) {
                    std::string file = line.substr(0, colon);
                    uint32_t ln = static_cast<uint32_t>(std::strtoul(line.c_str() + colon + 1, nullptr, 10));
                    if (ln > 0) out[{path, addrs[i]}] = {file, ln};
                }
                ++i;
            }
        }
    }
    return out;
}

}  // namespace detective
