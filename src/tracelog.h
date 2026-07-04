#pragma once
// -----------------------------------------------------------------------------
// tracelog.h — buffered logger + scoped-timing instrumentation.
//
// Issue #26: a buffered logger and scoped-timing helpers to find combat input
// lag. The idea is to give the runtime a single, fast, thread-safe place to
// record what happened (an event log) AND how long it took (a scope timer)
// so we can correlate "press TAB at T+0" with "engine responded at T+23ms"
// in a postmortem analysis pass.
//
// Why buffered + not fprintf:
//   * fprintf to stderr is a syscall; on Windows it's a few microseconds,
//     on Linux under load it can hit milliseconds. In a 60Hz render loop
//     that's a perceptible stutter every time we log. The buffered logger
//     pushes a tiny struct (or short formatted line) into a lock-protected
//     SPSC queue and lets the background flush thread do the syscall at
//     its leisure.
//   * Buffered also makes it cheap to instrument code paths that fire
//     hundreds of times per frame — a ScopedTimer destructor at the end
//     of a per-frame function still only does one log per frame.
//
// Why scoped-timing RAII and not manual start/stop:
//   * Easy to forget to call "stop". A ScopedTimer at function entry prints
//     on exit no matter how the function returns — RAII is the contract.
//   * Stackable: a function can take a scoped timer at its top while one
//     of its callees does the same. Total time includes the children; the
//     caller doesn't need to know what they are.
//
// Why a separate module (and not a global sprintf+stderr):
//   * So call sites can stay terse:  TRACELOG("event %s", name);
//   * So the formatter can stamp every line with a wall-clock timestamp
//     and the source label.
//   * So we can disable it at runtime (TRACELOG_ENABLE=0 build flag) for
//     a release build without touching call sites.
//
// ============================ THREADING CONTRACT ============================
// Single-producer / single-consumer ring buffer (the game thread is the only
// producer; a dedicated flush thread or a tick-driven flush is the only
// consumer). Lines are <512 bytes; the buffer holds up to kBufferCap lines.
// On overflow we drop the OLDEST line so the producer never blocks — a real
// game never wants logging to stall the render loop.
//
// Per-line format (one entry per line on the log file):
//   <wall_clock_ns>  <scope_or_event>  <text>\n
// The ScopedTimer destructor logs the duration in MICROSECONDS so we can
// spot a 500us hiccup without a sub-microsecond clock. Wall clock is
// std::chrono::steady_clock so a log line never goes backwards.
// =============================================================================
//
// ============================ DISABLING THE LOG =============================
// Set TRACELOG_ENABLE to 0 (or simply don't call init()) and every public
// function in this header becomes a cheap no-op. Use that for the
// release build path — same call sites, zero overhead.
// =============================================================================

#include <cstdarg>
#include <cstdint>

// Master switch — set to 0 to compile the call sites out entirely. Default
// is 1 (logger is live) for the dev build. Profiling toggle at the bottom
// of tracelog.cpp lets the user turn it back off at runtime.
#ifndef TRACELOG_ENABLE
#define TRACELOG_ENABLE 1
#endif

namespace tracelog {

// Bring up the background flush thread + open the log file. Idempotent;
// call once near startup. Safe to call from main BEFORE any logging — the
// early lines get queued and written once init() returns.
void init();

// Tear down: flush remaining lines, join the flush thread, close the file.
// Safe to call multiple times.
void shutdown();

// Append one formatted line to the log buffer. Thread-safe; never blocks
// the caller (drops on overflow rather than waiting). Mirrors printf
// semantics: format string + varargs. Lines are truncated at ~512 bytes so
// a runaway format can't blow the buffer.
void log(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ---- scoped timing ---------------------------------------------------------

// RAII timer that records duration on destruction. Declare at the TOP of a
// scope (function body, loop, block) and the destructor logs the wall-clock
// microseconds spent inside the scope. Never throws; safe to leave in a
// hot path.
class ScopedTimer {
public:
    explicit ScopedTimer(const char* label);
    ~ScopedTimer();
    // Non-copyable, non-movable: a ScopedTimer owns a stack frame's timing.
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
private:
    const char* label_;
    int64_t     start_ns_;
};

// True if the runtime logging is currently active (init called, not yet
// shut down). Cheap to test from anywhere; lets you gate expensive
// formatters in hot paths.
bool enabled();

// True if the dev profiler is currently capturing (init'd and active). Use
// to gate instrumentation that's worth running in a profile but not in
// steady state.
bool profiling();

// ---- macros ---------------------------------------------------------------

// Token-pasting on a __LINE__-based unique identifier so two SCOPED_TIMER
// calls in the same function don't clash. The optional _FMT form lets the
// caller pass extra printf args (e.g. an iteration count) when the label
// alone isn't enough to disambiguate the instance.
#define TRACELOG_SCOPED_TIMER(label) \
    ::tracelog::ScopedTimer tracelog_scope_##__LINE__(label)
#define TRACELOG_SCOPED_TIMER_FMT(label, ...) \
    ::tracelog::ScopedTimer tracelog_scope_##__LINE__(label)
#define TRACELOG_SCOPED_FUNC() \
    TRACELOG_SCOPED_TIMER(__func__)

// A plain log line. Wraps the variadic form so it compiles out cleanly when
// TRACELOG_ENABLE is 0.
#define TRACELOG(...) \
    do { if (::tracelog::enabled()) ::tracelog::log(__VA_ARGS__); } while (0)

} // namespace tracelog
