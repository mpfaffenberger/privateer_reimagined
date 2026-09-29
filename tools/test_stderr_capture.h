// -----------------------------------------------------------------------------
// test_stderr_capture.h — record what a call writes to the C stderr stream.
//
// Log text is part of a failure path's contract: a message that names the
// wrong thing (or garbage, #239) is a bug even when the return value is right.
// capture() swaps fd 2 for a scratch file around the call, so it sees
// fprintf(stderr, ...) from any linked translation unit.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define NP_TEST_DUP   _dup
#define NP_TEST_DUP2  _dup2
#define NP_TEST_CLOSE _close
#define NP_TEST_FILENO _fileno
#else
#include <unistd.h>
#define NP_TEST_DUP   dup
#define NP_TEST_DUP2  dup2
#define NP_TEST_CLOSE close
#define NP_TEST_FILENO fileno
#endif

namespace test_stderr {

// Runs fn() with stderr redirected into `scratch` and returns the bytes
// written. Returns "<capture failed>" rather than a silent "" if the
// redirection itself can't be set up, so a check on the text still fails loud.
template <class Fn>
std::string capture(const std::filesystem::path& scratch, Fn&& fn) {
    std::fflush(stderr);
    const int err_fd = NP_TEST_FILENO(stderr);
    const int saved  = NP_TEST_DUP(err_fd);
    FILE* sink = std::fopen(scratch.string().c_str(), "wb");
    if (saved < 0 || !sink || NP_TEST_DUP2(NP_TEST_FILENO(sink), err_fd) < 0) {
        if (sink) std::fclose(sink);
        if (saved >= 0) NP_TEST_CLOSE(saved);
        fn();
        return "<capture failed>";
    }
    fn();
    std::fflush(stderr);
    NP_TEST_DUP2(saved, err_fd);
    NP_TEST_CLOSE(saved);
    std::fclose(sink);

    std::ifstream in(scratch, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace test_stderr

#undef NP_TEST_DUP
#undef NP_TEST_DUP2
#undef NP_TEST_CLOSE
#undef NP_TEST_FILENO
