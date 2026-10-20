// Performance Detective instrumentation API (Mode B).
//
// Link the target against `detective_instr` and wrap regions of interest:
//
//     void query() {
//         DETECTIVE_FUNCTION();              // scope named after the function
//         { DETECTIVE_SCOPE("db.query"); }   // named scope
//         DETECTIVE_MARK("cache.lookup");    // counter, no timing
//     }
//
// Timings are WALL-CLOCK (std::chrono::steady_clock): they include time the thread
// spends blocked. Events are aggregated in per-thread buffers (no lock per event)
// and merged once per thread at thread exit. At process exit the report is written
// to the path in the DETECTIVE_INSTR_OUT environment variable (if set).
#pragma once

#include <atomic>
#include <cstdint>

namespace detective::instr {

// One static instance per DETECTIVE_SCOPE call site.
struct Site {
    const char* name;
    std::atomic<int32_t> id{-1};  // assigned on first use
};

int32_t register_site(Site& site) noexcept;

class Scope {
public:
    explicit Scope(Site& site) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int32_t id_ = -1;
    int64_t start_ns_ = 0;
};

void mark(Site& site) noexcept;

// Writes the report now (normally automatic at exit). Returns false on I/O error.
bool write_report(const char* path) noexcept;

// Testing hook: merge the calling thread's buffers into the global registry now.
void flush_current_thread() noexcept;
// Testing hook: drop all collected data.
void reset_for_testing() noexcept;

}  // namespace detective::instr

#define DETECTIVE_CONCAT_INNER(a, b) a##b
#define DETECTIVE_CONCAT(a, b) DETECTIVE_CONCAT_INNER(a, b)

#define DETECTIVE_SCOPE(name_literal)                                                       \
    static ::detective::instr::Site DETECTIVE_CONCAT(detective_site_, __LINE__){name_literal}; \
    ::detective::instr::Scope DETECTIVE_CONCAT(detective_scope_, __LINE__)(DETECTIVE_CONCAT(detective_site_, __LINE__))

#define DETECTIVE_FUNCTION() DETECTIVE_SCOPE(__func__)

#define DETECTIVE_MARK(name_literal)                                                        \
    do {                                                                                    \
        static ::detective::instr::Site detective_mark_site_{name_literal};                 \
        ::detective::instr::mark(detective_mark_site_);                                     \
    } while (0)
