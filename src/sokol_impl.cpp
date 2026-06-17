// -----------------------------------------------------------------------------
// sokol_impl.cpp — non-Apple twin of sokol_impl.mm.
//
// Same SOKOL_IMPL expansion, but compiled as plain C++ because none of the
// non-Apple sokol backends (D3D11 on Windows, GLCORE on Linux) pull in
// Objective-C headers. The .mm version is mac-only because of the Metal /
// Foundation framework headers' ObjC syntax.
//
// CMakeLists picks ONE of the two based on platform — never both — so the
// SOKOL_IMPL one-definition rule holds.
// -----------------------------------------------------------------------------

#define SOKOL_IMPL

// Backend is selected by CMake via a -DSOKOL_D3D11 (Win) / -DSOKOL_GLCORE
// (Linux) compile definition.

#include "sokol_log.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_time.h"
#include "sokol_debugtext.h"
#include "sokol_audio.h"
