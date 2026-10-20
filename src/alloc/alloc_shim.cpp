// libdetective_alloc.so — allocation tracker injected with LD_PRELOAD.
//
// Interposes malloc/calloc/realloc/free/posix_memalign/aligned_alloc/memalign and
// operator new/delete. Counters and the call-site table are lock-free atomics.
// At unload, writes a JSON report to $DETECTIVE_ALLOC_OUT together with a snapshot of
// the executable mappings of /proc/self/maps so the engine can symbolize call sites.
//
// Metric definitions (kept distinct on purpose):
//   bytes_requested : total allocation volume (sum of requested sizes)
//   live / peak     : malloc_usable_size accounting of currently allocated blocks
// This is NOT resident memory (RSS), which the engine reports separately via getrusage.
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

using MallocFn = void* (*)(size_t);
using FreeFn = void (*)(void*);
using CallocFn = void* (*)(size_t, size_t);
using ReallocFn = void* (*)(void*, size_t);
using PosixMemalignFn = int (*)(void**, size_t, size_t);
using AlignedAllocFn = void* (*)(size_t, size_t);

MallocFn real_malloc = nullptr;
FreeFn real_free = nullptr;
CallocFn real_calloc = nullptr;
ReallocFn real_realloc = nullptr;
PosixMemalignFn real_posix_memalign = nullptr;
AlignedAllocFn real_aligned_alloc = nullptr;
AlignedAllocFn real_memalign = nullptr;

// Bootstrap arena used while dlsym itself allocates.
alignas(64) char g_boot[256 * 1024];
std::atomic<size_t> g_boot_used{0};
std::atomic<int> g_init_state{0};  // 0 = not started, 1 = in progress, 2 = done
std::atomic<bool> g_reporting{false};
pthread_key_t g_key;
std::atomic<bool> g_key_ready{false};
void thread_exit_flush(void*);

bool in_boot(const void* p) {
    return p >= static_cast<const void*>(g_boot) && p < static_cast<const void*>(g_boot + sizeof g_boot);
}

void* boot_alloc(size_t n) {
    n = (n + 15) & ~size_t{15};
    size_t off = g_boot_used.fetch_add(n);
    if (off + n > sizeof g_boot) return nullptr;
    return g_boot + off;
}

void init() {
    int expected = 0;
    if (!g_init_state.compare_exchange_strong(expected, 1)) return;
    real_malloc = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
    real_free = reinterpret_cast<FreeFn>(dlsym(RTLD_NEXT, "free"));
    real_calloc = reinterpret_cast<CallocFn>(dlsym(RTLD_NEXT, "calloc"));
    real_realloc = reinterpret_cast<ReallocFn>(dlsym(RTLD_NEXT, "realloc"));
    real_posix_memalign = reinterpret_cast<PosixMemalignFn>(dlsym(RTLD_NEXT, "posix_memalign"));
    real_aligned_alloc = reinterpret_cast<AlignedAllocFn>(dlsym(RTLD_NEXT, "aligned_alloc"));
    real_memalign = reinterpret_cast<AlignedAllocFn>(dlsym(RTLD_NEXT, "memalign"));
    g_init_state.store(2);
    if (pthread_key_create(&g_key, thread_exit_flush) == 0) g_key_ready.store(true, std::memory_order_release);
}

// ---- counters
// Hot path: thread-local plain counters and a thread-local call-site cache; merged into the
// global atomics when a cache slot is evicted, when the thread exits, and at report time.
enum Kind { kMalloc, kCalloc, kRealloc, kMemalign, kNew, kKinds };
std::atomic<uint64_t> g_calls[kKinds];
std::atomic<uint64_t> g_frees{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<int64_t> g_live{0};
std::atomic<int64_t> g_peak{0};
std::atomic<uint64_t> g_hist[48];

// ---- global call-site table (lock-free open addressing)
constexpr size_t kSites = 8192;
struct SiteSlot {
    std::atomic<uintptr_t> addr;
    std::atomic<uint64_t> count;
    std::atomic<uint64_t> bytes;
};
SiteSlot g_sites[kSites];
std::atomic<uint64_t> g_site_overflow{0};

void record_site_global(uintptr_t addr, uint64_t count, uint64_t bytes) {
    size_t h = static_cast<size_t>((addr >> 2) * 0x9E3779B97F4A7C15ULL) & (kSites - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
        SiteSlot& s = g_sites[(h + probe) & (kSites - 1)];
        uintptr_t cur = s.addr.load(std::memory_order_relaxed);
        if (cur == 0) {
            uintptr_t zero = 0;
            if (s.addr.compare_exchange_strong(zero, addr) || zero == addr) cur = addr;
            else cur = zero;
        }
        if (cur == addr) {
            s.count.fetch_add(count, std::memory_order_relaxed);
            s.bytes.fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
    }
    g_site_overflow.fetch_add(count, std::memory_order_relaxed);
}

constexpr size_t kLocalSites = 256;  // direct-mapped per-thread cache
// Live-heap deltas are published once they exceed this many bytes (bounds peak error per thread).
constexpr int64_t kLiveFlushBytes = 8 * 1024;

struct LocalSite { uintptr_t addr; uint64_t count; uint64_t bytes; };
struct ThreadCounters {
    bool registered;
    uint64_t calls[kKinds];
    uint64_t frees;
    uint64_t bytes;
    int64_t live_delta;
    uint64_t hist[48];
    LocalSite sites[kLocalSites];
};

__attribute__((tls_model("initial-exec"))) thread_local ThreadCounters t_counters;  // POD, zero-initialized
void publish_live(ThreadCounters& t) {
    int64_t live = g_live.fetch_add(t.live_delta, std::memory_order_relaxed) + t.live_delta;
    t.live_delta = 0;
    int64_t peak = g_peak.load(std::memory_order_relaxed);
    while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
}

void flush_thread(ThreadCounters& t) {
    for (int k = 0; k < kKinds; ++k) {
        if (t.calls[k]) g_calls[k].fetch_add(t.calls[k], std::memory_order_relaxed);
        t.calls[k] = 0;
    }
    g_frees.fetch_add(t.frees, std::memory_order_relaxed);
    g_bytes.fetch_add(t.bytes, std::memory_order_relaxed);
    t.frees = t.bytes = 0;
    publish_live(t);
    for (auto& ls : t.sites) {
        if (ls.addr) record_site_global(ls.addr, ls.count, ls.bytes);
        ls = LocalSite{0, 0, 0};
    }
}

void flush_histogram(ThreadCounters& t) {
    for (int i = 0; i < 48; ++i) {
        if (t.hist[i]) g_hist[i].fetch_add(t.hist[i], std::memory_order_relaxed);
        t.hist[i] = 0;
    }
}

void thread_exit_flush(void*) {
    flush_histogram(t_counters);
    flush_thread(t_counters);
}

ThreadCounters& counters() {
    ThreadCounters& t = t_counters;
    if (!t.registered && g_key_ready.load(std::memory_order_acquire)) {
        t.registered = true;
        pthread_setspecific(g_key, &t);  // non-null value so the destructor runs at thread exit
    }
    return t;
}

void on_alloc(void* p, size_t req, Kind k, void* site) {
    if (!p || g_reporting.load(std::memory_order_relaxed)) return;
    ThreadCounters& t = counters();
    t.calls[k]++;
    t.bytes += req;
    int b = req ? 63 - __builtin_clzll(static_cast<unsigned long long>(req)) : 0;
    t.hist[b < 48 ? b : 47]++;
    t.live_delta += static_cast<int64_t>(malloc_usable_size(p));
    if (t.live_delta > kLiveFlushBytes) publish_live(t);
    auto addr = reinterpret_cast<uintptr_t>(site);
    LocalSite& ls = t.sites[(addr >> 2) & (kLocalSites - 1)];
    if (ls.addr != addr) {
        if (ls.addr) record_site_global(ls.addr, ls.count, ls.bytes);
        ls = LocalSite{addr, 0, 0};
    }
    ls.count++;
    ls.bytes += req;
}

void on_free(void* p) {
    if (!p || g_reporting.load(std::memory_order_relaxed)) return;
    ThreadCounters& t = counters();
    t.frees++;
    t.live_delta -= static_cast<int64_t>(malloc_usable_size(p));
    if (t.live_delta < -kLiveFlushBytes) publish_live(t);
}

void* do_malloc(size_t n, Kind k, void* site) {
    if (g_init_state.load() != 2) {
        init();
        if (g_init_state.load() != 2) return boot_alloc(n);
    }
    void* p = real_malloc(n);
    on_alloc(p, n, k, site);
    return p;
}

void do_free(void* p) {
    if (!p || in_boot(p)) return;
    if (g_init_state.load() != 2) init();
    on_free(p);
    real_free(p);
}

void* do_aligned(size_t align, size_t n, void* site) {
    if (g_init_state.load() != 2) init();
    void* p = real_memalign ? real_memalign(align, n) : nullptr;
    on_alloc(p, n, kMemalign, site);
    return p;
}

// ---- report
void append(char* buf, size_t cap, size_t* len, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void append(char* buf, size_t cap, size_t* len, const char* fmt, ...) {
    if (*len >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n > 0) *len += static_cast<size_t>(n);
}

void write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

void write_json_string(int fd, const char* s, size_t n) {
    char out[1024];
    size_t o = 0;
    out[o++] = '"';
    for (size_t i = 0; i < n && o < sizeof out - 8; ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (static_cast<unsigned char>(c) >= 0x20) out[o++] = c;
    }
    out[o++] = '"';
    write_all(fd, out, o);
}

__attribute__((destructor)) void write_report() {
    const char* path = std::getenv("DETECTIVE_ALLOC_OUT");
    if (!path) return;
    g_reporting.store(true);
    flush_histogram(t_counters);
    flush_thread(t_counters);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    static char buf[1 << 16];
    size_t len = 0;
    append(buf, sizeof buf, &len,
           "{\"version\":1,\"malloc\":%llu,\"calloc\":%llu,\"realloc\":%llu,\"memalign\":%llu,\"new\":%llu,"
           "\"free\":%llu,\"live_flush_bytes\":%lld,\"bytes_requested\":%llu,\"peak_live\":%lld,\"live_at_exit\":%lld,\"site_overflow\":%llu,"
           "\"histogram\":[",
           static_cast<unsigned long long>(g_calls[kMalloc].load()), static_cast<unsigned long long>(g_calls[kCalloc].load()),
           static_cast<unsigned long long>(g_calls[kRealloc].load()), static_cast<unsigned long long>(g_calls[kMemalign].load()),
           static_cast<unsigned long long>(g_calls[kNew].load()), static_cast<unsigned long long>(g_frees.load()),
           static_cast<long long>(kLiveFlushBytes),
           static_cast<unsigned long long>(g_bytes.load()), static_cast<long long>(g_peak.load()),
           static_cast<long long>(g_live.load()), static_cast<unsigned long long>(g_site_overflow.load()));
    for (int i = 0; i < 48; ++i)
        append(buf, sizeof buf, &len, "%s%llu", i ? "," : "", static_cast<unsigned long long>(g_hist[i].load()));
    append(buf, sizeof buf, &len, "],\"sites\":[");
    write_all(fd, buf, len);
    len = 0;
    bool first = true;
    for (auto& s : g_sites) {
        uintptr_t a = s.addr.load();
        if (!a) continue;
        append(buf, sizeof buf, &len, "%s[%llu,%llu,%llu]", first ? "" : ",", static_cast<unsigned long long>(a),
               static_cast<unsigned long long>(s.count.load()), static_cast<unsigned long long>(s.bytes.load()));
        first = false;
        if (len > sizeof buf - 128) { write_all(fd, buf, len); len = 0; }
    }
    append(buf, sizeof buf, &len, "],\"maps\":[");
    write_all(fd, buf, len);
    len = 0;
    // Executable mappings, for symbolizing call sites.
    int mfd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    first = true;
    if (mfd >= 0) {
        static char mbuf[1 << 20];
        size_t used = 0;
        ssize_t r;
        while (used < sizeof mbuf && (r = ::read(mfd, mbuf + used, sizeof mbuf - used)) > 0) used += static_cast<size_t>(r);
        ::close(mfd);
        size_t start = 0;
        for (size_t i = 0; i < used; ++i) {
            if (mbuf[i] != '\n') continue;
            const char* line = mbuf + start;
            size_t n = i - start;
            // perms field follows the first space: "start-end r-xp ..."
            const char* sp = static_cast<const char*>(std::memchr(line, ' ', n));
            if (sp && static_cast<size_t>(sp - line) + 3 < n && sp[3] == 'x') {
                if (!first) write_all(fd, ",", 1);
                write_json_string(fd, line, n);
                first = false;
            }
            start = i + 1;
        }
    }
    write_all(fd, "]}\n", 3);
    ::close(fd);
}

}  // namespace

// ---------------------------------------------------------------- C interface
extern "C" {

__attribute__((visibility("default"))) void* malloc(size_t n) {
    return do_malloc(n, kMalloc, __builtin_return_address(0));
}

__attribute__((visibility("default"))) void free(void* p) { do_free(p); }

__attribute__((visibility("default"))) void* calloc(size_t a, size_t b) {
    if (g_init_state.load() != 2) {
        init();
        if (g_init_state.load() != 2) {
            void* p = boot_alloc(a * b);
            if (p) std::memset(p, 0, a * b);
            return p;
        }
    }
    void* p = real_calloc(a, b);
    on_alloc(p, a * b, kCalloc, __builtin_return_address(0));
    return p;
}

__attribute__((visibility("default"))) void* realloc(void* p, size_t n) {
    if (g_init_state.load() != 2) init();
    if (in_boot(p)) {
        void* q = real_malloc(n);
        if (q) std::memcpy(q, p, n);  // bootstrap blocks are small; over-read stays inside g_boot
        on_alloc(q, n, kRealloc, __builtin_return_address(0));
        return q;
    }
    size_t old_usable = p ? malloc_usable_size(p) : 0;
    void* q = real_realloc(p, n);
    if (q && !g_reporting.load(std::memory_order_relaxed)) {
        counters().live_delta -= static_cast<int64_t>(old_usable);  // old block released (not a user free)
        on_alloc(q, n, kRealloc, __builtin_return_address(0));
    }
    return q;
}

__attribute__((visibility("default"))) int posix_memalign(void** out, size_t align, size_t n) {
    if (g_init_state.load() != 2) init();
    int rc = real_posix_memalign(out, align, n);
    if (rc == 0) on_alloc(*out, n, kMemalign, __builtin_return_address(0));
    return rc;
}

__attribute__((visibility("default"))) void* aligned_alloc(size_t align, size_t n) {
    if (g_init_state.load() != 2) init();
    void* p = real_aligned_alloc(align, n);
    on_alloc(p, n, kMemalign, __builtin_return_address(0));
    return p;
}

__attribute__((visibility("default"))) void* memalign(size_t align, size_t n) {
    return do_aligned(align, n, __builtin_return_address(0));
}

}  // extern "C"

// ---------------------------------------------------------------- C++ operators
namespace {
void* new_impl(size_t n, void* site) {
    if (n == 0) n = 1;
    for (;;) {
        void* p = do_malloc(n, kNew, site);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
void* new_aligned_impl(size_t n, size_t align, void* site) {
    if (n == 0) n = 1;
    void* p = do_aligned(align, n, site);
    if (!p) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(size_t n) { return new_impl(n, __builtin_return_address(0)); }
void* operator new[](size_t n) { return new_impl(n, __builtin_return_address(0)); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    return do_malloc(n ? n : 1, kNew, __builtin_return_address(0));
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    return do_malloc(n ? n : 1, kNew, __builtin_return_address(0));
}
void* operator new(size_t n, std::align_val_t a) { return new_aligned_impl(n, static_cast<size_t>(a), __builtin_return_address(0)); }
void* operator new[](size_t n, std::align_val_t a) { return new_aligned_impl(n, static_cast<size_t>(a), __builtin_return_address(0)); }
void operator delete(void* p) noexcept { do_free(p); }
void operator delete[](void* p) noexcept { do_free(p); }
void operator delete(void* p, size_t) noexcept { do_free(p); }
void operator delete[](void* p, size_t) noexcept { do_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { do_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { do_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { do_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { do_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { do_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { do_free(p); }
