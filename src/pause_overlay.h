#pragma once
// Pure visibility rule for the Flight pause overlay.

namespace pause_overlay {

inline bool visible(bool paused, bool flight_mode, bool show_title,
                    bool cinematic_active) {
    return paused && flight_mode && !show_title && !cinematic_active;
}

} // namespace pause_overlay
