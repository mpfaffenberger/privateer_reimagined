// -----------------------------------------------------------------------------
// win32_launch_dialog.h — Windows pre-launch resolution picker.
//
// Pops a tiny Win32 dialog before sokol creates the window. Lets the user pick
// a display mode from a curated list. If the user cancels, the caller should
// fall back to its defaults.
// -----------------------------------------------------------------------------

#pragma once

#ifdef _WIN32

namespace win32 {

// Show the picker. On success fills *out_w, *out_h, *out_fullscreen.
// Returns false if the dialog was cancelled or creation failed.
bool pick_resolution(int* out_w, int* out_h, bool* out_fullscreen);

} // namespace win32

#endif // _WIN32
