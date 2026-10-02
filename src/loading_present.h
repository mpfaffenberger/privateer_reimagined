#pragma once
// -----------------------------------------------------------------------------
// loading_present.h — put a loading-bar frame on screen mid-callback (#694).
//
// Blocking loads (init_cb, the save-menu pending_goto path) render progress
// frames from INSIDE a sokol_app callback. Whether those frames reach the
// window depends on the backend:
//   * Metal: sokol_gfx presents the drawable inside sg_commit(). Nothing to do.
//   * D3D11: sokol_app calls IDXGISwapChain::Present only after the callback
//     returns, so every mid-load frame was drawn and then thrown away.
// flush() closes that gap without touching the vendored sokol headers.
// -----------------------------------------------------------------------------

namespace loading_present {

// Call right after sg_commit() for a frame drawn during a blocking load.
// D3D11: presents the frame immediately and checks the message queue so
// Windows keeps treating the window as responsive. Other backends: no-op.
void flush();

// True where flush() is what puts frames on screen (D3D11). There, a long
// stage needs periodic heartbeat redraws to keep the bar moving and the
// window out of "Not Responding". Elsewhere (Metal) the loading screen keeps
// its existing stage-only presentation, unchanged.
bool needs_heartbeat();

} // namespace loading_present
