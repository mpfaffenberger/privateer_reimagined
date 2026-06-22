#pragma once
// -----------------------------------------------------------------------------
// navmap_projection.h — shared 3D-world -> 2D-system-map projection helpers.
// -----------------------------------------------------------------------------
//
// Keep this boring on purpose: the navmap projection is engine X/Z -> map X/Y.
// If a WCNews/wiki coordinate has a missing minus sign, fix the underlying
// wiki-derived coordinate in the auditor instead of hiding it behind projection
// magic. Authored `map_position` remains supported as an explicit override, but
// the normal fallback path is now a plain Cartesian projection.
// -----------------------------------------------------------------------------

#include "system_def.h"

inline HMM_Vec2 navmap_project_world(HMM_Vec3 p) {
    return HMM_V2(p.X, p.Z);
}

inline HMM_Vec2 navmap_project_nav(const NavPointDef& nav) {
    return nav.has_map_position ? nav.map_position : navmap_project_world(nav.position);
}
