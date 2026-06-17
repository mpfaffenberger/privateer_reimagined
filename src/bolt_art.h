// bolt_art.h — per-GunType animated bolt sprite textures.
//
// Each GunType (Laser, Mass Driver, Plasma Gun, ...) has 1-9 bolt frames
// loaded from assets/bolts/<prefix>_NN.png. The bolt renderer cycles frames
// at a fixed fps for the spinning/flicker animation. 7 of 8 gun bolts are
// extracted from the original Privateer DATA/APPEARNC/ sprite sets; the Laser
// bolt is procedurally generated (see tools/generate_laser_bolt.py) because
// vanilla drew it procedurally as a red ray.
//
// Lifetime: one BoltArtSet per app. Loaded once at startup; textures live
// until destroy(). Frame lookup is a simple modulo by frame count — no
// allocations after load.

#pragma once

#include "sokol_gfx.h"
#include "gun.h"

#include <string>
#include <vector>

// A texture slot wrapper matching what SpriteRenderer expects (sg_image +
// valid flag). Kept lightweight so bolt textures drop into the same billboard
// draw path as sprite art.
struct BoltTexture {
    sg_image img  = {0};
    sg_view  view = {0};
    bool     valid = false;
    int      w = 0, h = 0;
};

// All bolt frames for one GunType.
struct BoltFrames {
    std::vector<BoltTexture> frames;
    bool elongated = false;   // laser: long axis should align with velocity
};

// The full set — one entry per GunType. index by (int)GunType.
// kGunTypeCount entries; SteltekGun has no bolt yet and falls back to Plasma.
class BoltArtSet {
public:
    // Load every assets/bolts/<prefix>_NN.png matching the gun prefix map.
    // Missing guns are silently skipped (the renderer falls back to the
    // procedural glow). SteltekGun aliases PlasmaGun's bolts.
    void load(const std::string& dir);

    // GPU cleanup — call on shutdown or system reload.
    void destroy();

    // Pick a frame for this gun type at time t (seconds). Animation cycles
    // at k_bolt_fps through the frame set. Returns nullptr if no frames
    // loaded for this type (caller should skip / fall back to glow).
    const BoltTexture* frame(GunType type, float t) const;

    bool has_bolts(GunType type) const;

    // Flatten all textures into one vector for the renderer, writing the
    // start index for each gun type into offsets[]. Call once after load.
    // texture_id for a bolt = offsets[type] + frame_index(type, t).
    void flatten(std::vector<sg_view>& out, int offsets[kGunTypeCount]) const;

    // Animation frame index for this gun type at time t.
    int frame_index(GunType type, float t) const;

    // Texture aspect (w/h) for this gun type — elongated bolts use this
    // to stretch the billboard.
    float aspect(GunType type) const;

private:
    BoltFrames m_guns[kGunTypeCount];

    // GunType -> file prefix. Must match the extraction script output.
    static const char* prefix_for(GunType type);
};
