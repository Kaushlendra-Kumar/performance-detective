// Linux perf_event_open based sampling collector.
//
// Uses the software CPU-clock event (works in VMs without a hardware PMU) with
// kernel frame-pointer callchains. One event per CPU with `inherit` so that all
// threads and child processes of the target are sampled.
#pragma once

#include "detective/process_runner.hpp"
#include "detective/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace detective {

struct SamplerConfig {
    int frequency_hz = 999;          // clamped to [1, 10000]
    size_t ring_pages = 128;         // data pages per ring buffer (power of two)
    size_t max_samples = 2'000'000;  // hard cap on retained samples
    bool scan_proc_maps = true;      // supplement PERF_RECORD_MMAP with /proc/<pid>/maps
    bool simulate_unavailable = false;  // testing: behave as if perf_event_open failed
};

struct SamplerCapabilities {
    bool perf_event_open_available = false;
    bool hardware_counters = false;
    bool software_cpu_clock = false;
    int perf_event_paranoid = -100;
    int max_stack = 0;
    bool running_as_root = false;
    std::string summary;
};

SamplerCapabilities probe_sampler_capabilities();

class PerfSampler : public ProcessObserver {
public:
    explicit PerfSampler(SamplerConfig cfg);
    ~PerfSampler() override;
    PerfSampler(const PerfSampler&) = delete;
    PerfSampler& operator=(const PerfSampler&) = delete;

    bool on_child_created(pid_t pid, std::string* error) override;
    std::vector<int> poll_fds() override;
    void on_poll() override;
    void on_child_exited() override;

    // Moves the collected data out. Valid after the process has exited.
    RawProfile take_profile();

    uint64_t period_ns() const { return period_ns_; }

private:
    struct Ring {
        int fd = -1;
        void* base = nullptr;
        size_t map_size = 0;
    };
    void drain_all();
    void drain(Ring& r);
    void handle_record(const uint8_t* rec, size_t size);
    void scan_maps();
    void close_all();

    SamplerConfig cfg_;
    uint64_t period_ns_ = 0;
    pid_t pid_ = -1;
    std::vector<Ring> rings_;
    RawProfile profile_;
    uint64_t dropped_by_limit_ = 0;
    uint64_t truncated_stacks_ = 0;
    int max_stack_ = 127;
    int poll_count_ = 0;
    std::vector<uint8_t> scratch_;
};

}  // namespace detective
