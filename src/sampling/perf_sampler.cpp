#include "detective/sampler.hpp"

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace detective {

namespace {

long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return ::syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int read_int_file(const char* path, int fallback) {
    std::ifstream in(path);
    int v;
    if (in >> v) return v;
    return fallback;
}

perf_event_attr make_attr(uint64_t period_ns, int max_stack, bool hw) {
    perf_event_attr a;
    std::memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = hw ? PERF_TYPE_HARDWARE : PERF_TYPE_SOFTWARE;
    a.config = hw ? static_cast<uint64_t>(PERF_COUNT_HW_CPU_CYCLES) : static_cast<uint64_t>(PERF_COUNT_SW_CPU_CLOCK);
    a.sample_period = period_ns;
    a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_PERIOD |
                    PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_STACK_USER;
    a.sample_stack_user = 8;  // just the word at %rsp (return address of a frameless leaf)
    a.disabled = 1;
    a.enable_on_exec = 1;
    a.inherit = 1;
    a.mmap = 1;
    a.comm = 1;
    a.comm_exec = 1;
    a.task = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.exclude_callchain_kernel = 1;
    a.watermark = 1;
    a.wakeup_watermark = 16 * 4096;
    a.sample_max_stack = static_cast<uint16_t>(max_stack);
    return a;
}

}  // namespace

SamplerCapabilities probe_sampler_capabilities() {
    SamplerCapabilities c;
    c.perf_event_paranoid = read_int_file("/proc/sys/kernel/perf_event_paranoid", -100);
    c.max_stack = read_int_file("/proc/sys/kernel/perf_event_max_stack", 127);
    c.running_as_root = ::geteuid() == 0;
    for (int hw = 1; hw >= 0; --hw) {
        perf_event_attr a;
        std::memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = hw ? PERF_TYPE_HARDWARE : PERF_TYPE_SOFTWARE;
        a.config = hw ? static_cast<uint64_t>(PERF_COUNT_HW_CPU_CYCLES) : static_cast<uint64_t>(PERF_COUNT_SW_CPU_CLOCK);
        a.disabled = 1;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        long fd = perf_event_open(&a, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
        if (fd >= 0) {
            ::close(static_cast<int>(fd));
            (hw ? c.hardware_counters : c.software_cpu_clock) = true;
        }
    }
    c.perf_event_open_available = c.hardware_counters || c.software_cpu_clock;
    std::ostringstream s;
    s << "perf_event_open: " << (c.perf_event_open_available ? "available" : "UNAVAILABLE")
      << "; software cpu-clock: " << (c.software_cpu_clock ? "yes" : "no")
      << "; hardware counters: " << (c.hardware_counters ? "yes" : "no (no PMU / not permitted)")
      << "; perf_event_paranoid=" << c.perf_event_paranoid << "; max_stack=" << c.max_stack;
    c.summary = s.str();
    return c;
}

PerfSampler::PerfSampler(SamplerConfig cfg) : cfg_(cfg) {
    cfg_.frequency_hz = std::clamp(cfg_.frequency_hz, 1, 10'000);
    size_t p = 1;
    while (p < cfg_.ring_pages) p <<= 1;
    cfg_.ring_pages = std::clamp<size_t>(p, 8, 4096);
    period_ns_ = 1'000'000'000ULL / static_cast<uint64_t>(cfg_.frequency_hz);
    max_stack_ = std::clamp(read_int_file("/proc/sys/kernel/perf_event_max_stack", 127), 1, 1024);
    profile_.sample_period_ns = period_ns_;
    profile_.event_name = "cpu-clock";
}

PerfSampler::~PerfSampler() { close_all(); }

void PerfSampler::close_all() {
    for (auto& r : rings_) {
        if (r.base) ::munmap(r.base, r.map_size);
        if (r.fd >= 0) ::close(r.fd);
        r.base = nullptr;
        r.fd = -1;
    }
    rings_.clear();
}

bool PerfSampler::on_child_created(pid_t pid, std::string* error) {
    pid_ = pid;
    long page = ::sysconf(_SC_PAGESIZE);
    size_t map_size = (cfg_.ring_pages + 1) * static_cast<size_t>(page);
    int ncpu = static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));
    perf_event_attr attr = make_attr(period_ns_, max_stack_, false);

    auto open_ring = [&](int cpu) -> bool {
        long fd = perf_event_open(&attr, pid, cpu, -1, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0) return false;
        void* base = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<int>(fd), 0);
        if (base == MAP_FAILED) {
            ::close(static_cast<int>(fd));
            return false;
        }
        rings_.push_back({static_cast<int>(fd), base, map_size});
        return true;
    };

    // Preferred: one inherited event per CPU (covers every thread / child process).
    if (cfg_.simulate_unavailable) {
        profile_.collected = false;
        profile_.collection_error = "perf_event_open unavailable (simulated for testing)";
        return true;
    }
    bool ok = ncpu > 0;
    for (int cpu = 0; ok && cpu < ncpu; ++cpu) ok = open_ring(cpu);
    if (!ok) {
        int saved = errno;
        close_all();
        // Fallback: one per-task event on any CPU.
        if (!open_ring(-1)) {
            profile_.collected = false;
            profile_.collection_error = std::string("perf_event_open failed: ") + std::strerror(saved) +
                                        " (see docs/linux-permissions.md)";
            // Do not abort the run: the process still executes and the session reports
            // honestly that no samples were collected.
            (void)error;
            return true;
        }
        profile_.warnings.push_back("per-CPU events unavailable; using a single per-task ring buffer");
    }
    profile_.collected = true;
    return true;
}

std::vector<int> PerfSampler::poll_fds() {
    std::vector<int> fds;
    for (auto& r : rings_) fds.push_back(r.fd);
    return fds;
}

void PerfSampler::on_poll() {
    drain_all();
    if (cfg_.scan_proc_maps && (poll_count_++ % 25) == 0) scan_maps();
}

void PerfSampler::on_child_exited() {
    drain_all();
    close_all();
    if (dropped_by_limit_ > 0) {
        profile_.warnings.push_back("sample limit reached; " + std::to_string(dropped_by_limit_) +
                                    " samples were discarded");
    }
    if (truncated_stacks_ > 0) {
        profile_.warnings.push_back(std::to_string(truncated_stacks_) +
                                    " stacks hit the kernel max_stack limit (" + std::to_string(max_stack_) +
                                    " frames) and are truncated");
    }
}

RawProfile PerfSampler::take_profile() { return std::move(profile_); }

void PerfSampler::scan_maps() {
    if (pid_ <= 0) return;
    std::ifstream in("/proc/" + std::to_string(pid_) + "/maps");
    std::string line;
    while (std::getline(in, line)) {
        // start-end perms offset dev inode path
        std::istringstream ss(line);
        std::string range, perms, offset, dev, inode, path;
        ss >> range >> perms >> offset >> dev >> inode;
        std::getline(ss, path);
        auto first = path.find_first_not_of(' ');
        path = first == std::string::npos ? "" : path.substr(first);
        if (perms.size() < 3 || perms[2] != 'x') continue;
        auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        MappingRecord m;
        m.pid = static_cast<uint32_t>(pid_);
        m.start = std::stoull(range.substr(0, dash), nullptr, 16);
        uint64_t end = std::stoull(range.substr(dash + 1), nullptr, 16);
        m.length = end - m.start;
        m.pgoff = std::stoull(offset, nullptr, 16);
        m.path = path;
        bool dup = false;
        for (const auto& e : profile_.mappings) {
            if (e.start == m.start && e.length == m.length && e.path == m.path && e.pid == m.pid) { dup = true; break; }
        }
        if (!dup) profile_.mappings.push_back(std::move(m));
    }
}

void PerfSampler::drain_all() {
    for (auto& r : rings_) drain(r);
}

void PerfSampler::drain(Ring& r) {
    auto* meta = static_cast<perf_event_mmap_page*>(r.base);
    long page = ::sysconf(_SC_PAGESIZE);
    auto* data = static_cast<uint8_t*>(r.base) + page;
    const uint64_t size = cfg_.ring_pages * static_cast<uint64_t>(page);
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = meta->data_tail;
    while (tail < head) {
        perf_event_header hdr;
        uint64_t off = tail % size;
        // Header may wrap around the end of the buffer.
        for (size_t i = 0; i < sizeof hdr; ++i)
            reinterpret_cast<uint8_t*>(&hdr)[i] = data[(off + i) % size];
        if (hdr.size < sizeof hdr || hdr.size > head - tail) break;  // corrupt / partial
        scratch_.resize(hdr.size);
        if (off + hdr.size <= size) {
            std::memcpy(scratch_.data(), data + off, hdr.size);
        } else {
            size_t first = size - off;
            std::memcpy(scratch_.data(), data + off, first);
            std::memcpy(scratch_.data() + first, data, hdr.size - first);
        }
        handle_record(scratch_.data(), hdr.size);
        tail += hdr.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

void PerfSampler::handle_record(const uint8_t* rec, size_t size) {
    perf_event_header hdr;
    std::memcpy(&hdr, rec, sizeof hdr);
    const uint8_t* p = rec + sizeof hdr;
    const uint8_t* end = rec + size;
    auto take64 = [&](uint64_t* v) {
        if (end - p < 8) return false;
        std::memcpy(v, p, 8);
        p += 8;
        return true;
    };
    auto take32 = [&](uint32_t* v) {
        if (end - p < 4) return false;
        std::memcpy(v, p, 4);
        p += 4;
        return true;
    };

    switch (hdr.type) {
        case PERF_RECORD_SAMPLE: {
            RawSample s;
            uint64_t ip = 0, nr = 0;
            if (!take64(&ip) || !take32(&s.pid) || !take32(&s.tid) || !take64(&s.time_ns) ||
                !take64(&s.weight_ns) || !take64(&nr))
                return;
            if (nr > static_cast<uint64_t>(end - p) / 8) return;
            s.ips.reserve(nr);
            for (uint64_t i = 0; i < nr; ++i) {
                uint64_t a = 0;
                take64(&a);
                if (a >= static_cast<uint64_t>(PERF_CONTEXT_MAX)) continue;  // context marker
                s.ips.push_back(a);
            }
            if (s.ips.empty()) s.ips.push_back(ip);
            // PERF_SAMPLE_STACK_USER: u64 size; u8 data[size]; u64 dyn_size (if size > 0)
            uint64_t stack_size = 0;
            if (take64(&stack_size) && stack_size >= 8 && static_cast<uint64_t>(end - p) >= stack_size + 8) {
                uint64_t top = 0;
                std::memcpy(&top, p, 8);
                p += stack_size;
                uint64_t dyn = 0;
                take64(&dyn);
                if (dyn >= 8) s.stack_top = top;
            }
            if (nr >= static_cast<uint64_t>(max_stack_)) ++truncated_stacks_;
            if (profile_.samples.size() >= cfg_.max_samples) {
                ++dropped_by_limit_;
                return;
            }
            profile_.samples.push_back(std::move(s));
            break;
        }
        case PERF_RECORD_MMAP: {
            MappingRecord m;
            uint32_t tid;
            if (!take32(&m.pid) || !take32(&tid) || !take64(&m.start) || !take64(&m.length) || !take64(&m.pgoff))
                return;
            size_t maxlen = static_cast<size_t>(end - p);
            m.path.assign(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), maxlen));
            profile_.mappings.push_back(std::move(m));
            break;
        }
        case PERF_RECORD_COMM: {
            uint32_t pid, tid;
            if (!take32(&pid) || !take32(&tid)) return;
            size_t maxlen = static_cast<size_t>(end - p);
            profile_.thread_names[tid] =
                std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), maxlen));
            break;
        }
        case PERF_RECORD_LOST: {
            uint64_t id, lost;
            if (take64(&id) && take64(&lost)) profile_.lost_records += lost;
            break;
        }
        default:
            break;
    }
}

}  // namespace detective
