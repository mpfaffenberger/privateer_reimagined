// -----------------------------------------------------------------------------
// tracelog.cpp — buffered logger + scoped-timing implementation.
//
// See tracelog.h for the design (SPSC ring buffer, RAII scope timer, drop-old
// on overflow, never-block the producer). What lives here:
//
//   1. Ring-buffer plumbing (lock-protected on the producer side; the
//      consumer is the dedicated flush thread that drains & writes the
//      file).
//   2. Steady-clock time helpers for both the wall-clock header and the
//      scope-timer duration (microseconds).
//   3. Formatter that packs "<wall_clock_ns>  <scope>  <text>\n" into the
//      ring buffer.
//   4. The ScopedTimer RAII class — label + start time on entry, log a
//      single line on destruction.
//
// The flush thread writes one line per buffered record to "tracelog.log"
// in the cwd (overwritten on init), so dev runs accumulate a single
// rolling file that can be diffed between runs.
// -----------------------------------------------------------------------------

#include "tracelog.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if TRACELOG_ENABLE

namespace {

// ---- ring buffer -----------------------------------------------------------
// SPSC intent (main = producer, flush thread = consumer). For safety against
// a logging site being called from another thread unexpectedly we still take
// a short mutex — the cost is one atomic op + a lock acquire; the body of
// the lock is just a couple of byte copies and a std::deque push. A deque
// rather than a fixed array because we want unbounded history without
// pre-sizing for the worst case (a 5-minute fight might legitimately need
// 100k+ lines).
constexpr size_t kDropThreshold = 8192;   // trim oldest when we exceed this

std::mutex              g_mtx;
std::deque<std::string> g_queue;
std::atomic<bool>       g_alive{ false };
std::atomic<bool>       g_profiling{ false };   // user toggle for dev builds

// ---- flush thread ----------------------------------------------------------
// Drains g_queue to the log file. A dedicated thread (rather than flushing
// from the game loop) keeps the writer off the critical path; if the disk
// hiccups the queue grows but the game keeps moving.
std::thread     g_thread;
FILE*           g_file = nullptr;
std::string     g_path = "tracelog.log";

void flush_thread_main() {
    std::vector<std::string> local;
    local.reserve(256);
    while (g_alive.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            while (!g_queue.empty()) {
                local.emplace_back(std::move(g_queue.front()));
                g_queue.pop_front();
            }
        }
        if (!local.empty()) {
            if (g_file) {
                for (const std::string& s : local) {
                    std::fputs(s.c_str(), g_file);
                    if (std::fputc('\n', g_file) == EOF) break;
                }
                std::fflush(g_file);
            }
            local.clear();
        }
        // Yield to the OS so we don't peg a core while idle.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    // Drain anything left on shutdown so we don't lose the tail.
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_file) {
        for (const std::string& s : g_queue) {
            std::fputs(s.c_str(), g_file);
            std::fputc('\n', g_file);
        }
        std::fflush(g_file);
    }
    g_queue.clear();
}

// ---- time helpers ----------------------------------------------------------
// std::chrono::steady_clock::now() is monotonic and sub-microsecond on every
// platform we target. We snapshot in nanoseconds for the wall clock header
// (so multi-second traces don't overflow) and microseconds for the
// scope-timer duration (so a sub-millisecond scope reads as "23us" not
// "0ms" in the log). Both use the same source so they're comparable.
int64_t now_ns() {
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

namespace tracelog {

void init() {
    if (g_alive.load()) return;
    g_alive.store(true);
    g_profiling.store(true);
    g_file = std::fopen(g_path.c_str(), "w");
    g_thread = std::thread(flush_thread_main);
    // Don't print here — init() may be called before stdout/stderr is ready.
    // The very first log line will get a header stamp + be written when
    // the flush thread first wakes up.
}

void shutdown() {
    if (!g_alive.load()) return;
    g_alive.store(false);
    if (g_thread.joinable()) g_thread.join();
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    g_profiling.store(false);
}

bool enabled()    { return g_alive.load(std::memory_order_acquire); }
bool profiling()  { return g_profiling.load(std::memory_order_acquire); }

void log(const char* fmt, ...) {
    if (!g_alive.load(std::memory_order_acquire)) return;
    char body[512];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(body, sizeof body, fmt, args);
    va_end(args);
    if (n < 0) return;                         // vsnprintf failed; drop
    const int64_t ts = now_ns();
    char line[640];
    const int hdr = std::snprintf(line, sizeof line,
                                  "%lld  %s", (long long)ts, body);
    if (hdr <= 0) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_queue.emplace_back(line, (size_t)hdr);
    if (g_queue.size() > kDropThreshold) {
        // Drop oldest batch to keep memory bounded; logging a stutter is
        // more useful than dropping the most recent events.
        g_queue.pop_front();
    }
}

ScopedTimer::ScopedTimer(const char* label)
    : label_(label ? label : "?"), start_ns_(now_ns()) {}

ScopedTimer::~ScopedTimer() {
    if (!g_alive.load(std::memory_order_acquire)) return;
    const int64_t dur_us = (now_ns() - start_ns_) / 1000;
    char body[512];
    std::snprintf(body, sizeof body, "[time] %s = %lld us",
                  label_, (long long)dur_us);
    const int64_t ts = now_ns();
    char line[640];
    const int n = std::snprintf(line, sizeof line,
                                "%lld  %s", (long long)ts, body);
    if (n <= 0) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_queue.emplace_back(line, (size_t)n);
    if (g_queue.size() > kDropThreshold) {
        g_queue.pop_front();
    }
}

} // namespace tracelog

#else  // TRACELOG_ENABLE == 0

// Compile-out build: every public function becomes a no-op. The macro
// shims in tracelog.h still compile, they just call the no-ops.
namespace tracelog {
void init() {}
void shutdown() {}
void log(const char*, ...) {}
bool enabled()   { return false; }
bool profiling() { return false; }
ScopedTimer::ScopedTimer(const char*) : label_(nullptr), start_ns_(0) {}
ScopedTimer::~ScopedTimer() {}
} // namespace tracelog

#endif // TRACELOG_ENABLE
