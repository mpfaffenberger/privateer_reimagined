// -----------------------------------------------------------------------------
// win32_desktop_res.h — detect primary monitor resolution on Windows.
//
// Header-only helper so sokol_main can size the initial window to the
// desktop without a pre-launch dialog. On non-Windows builds this file
// is simply not included.
// -----------------------------------------------------------------------------

#pragma once

#ifdef _WIN32

// WIN32_LEAN_AND_MEAN is defined once, on the MSVC command line in
// CMakeLists.txt (if(MSVC) block). Do not re-#define it here or MSVC
// warns C4005 about the duplicate definition.
#include <windows.h>

namespace win32 {

inline bool desktop_resolution(int* out_w, int* out_h) {
    if (!out_w || !out_h) return false;
    *out_w = GetSystemMetrics(SM_CXSCREEN);
    *out_h = GetSystemMetrics(SM_CYSCREEN);
    return *out_w > 0 && *out_h > 0;
}

} // namespace win32

#endif // _WIN32
