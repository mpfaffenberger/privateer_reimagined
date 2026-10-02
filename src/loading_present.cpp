// loading_present.cpp — see loading_present.h.

#include "loading_present.h"

#if defined(SOKOL_D3D11)

// WIN32_LEAN_AND_MEAN / NOMINMAX come from the MSVC command line (CMakeLists).
#include <windows.h>
#include <dxgi.h>

#include "sokol_app.h"

namespace loading_present {

void flush() {
    auto* swap_chain = static_cast<IDXGISwapChain*>(
        const_cast<void*>(sapp_d3d11_get_swap_chain()));
    if (swap_chain) {
        // Sync interval 0: queue the frame for the next vblank without
        // stalling the load on it. Stages are far apart, so nothing drops.
        swap_chain->Present(0, 0);
    }
    // Peek without removing anything. That is enough for Windows to see the
    // thread checking its queue, so the window isn't ghosted as "Not
    // Responding" during a long stage. Dispatching here would be unsafe:
    // a posted WM_TIMER (window drag) re-enters frame_cb, and input would
    // reach event_cb mid-load. sokol's own loop dispatches it all later.
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
}

bool needs_heartbeat() { return true; }

} // namespace loading_present

#else

namespace loading_present {

void flush() {}

bool needs_heartbeat() { return false; }

} // namespace loading_present

#endif
