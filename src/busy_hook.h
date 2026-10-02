#pragma once
// -----------------------------------------------------------------------------
// busy_hook.h — "still working" ticks from long-running loaders (#694).
//
// Some loading stages decode hundreds of files in one go (audio clips, ship
// atlas PNGs) and can run for tens of seconds. Leaf loaders call tick() once
// per file; the host installs a hook only while a blocking load is on screen,
// so it can keep the loading bar moving and the window responsive. With no
// hook installed tick() is a single null check. Header-only so loaders and
// the headless tests that link them pick up no new dependency.
// -----------------------------------------------------------------------------

namespace busy_hook {

using Fn = void (*)();

inline Fn g_hook = nullptr;

inline void set(Fn fn) { g_hook = fn; }

inline void tick() {
    if (g_hook) g_hook();
}

} // namespace busy_hook
