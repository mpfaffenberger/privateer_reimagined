#pragma once
// -----------------------------------------------------------------------------
// render_config.h — tiny shared header of pixel-format + MSAA constants.
//
// Every scene-drawing pipeline (skybox, sun, dust, ...) needs to declare
// the attachment format it will write into. Concentrating those constants
// here keeps things DRY and makes "change HDR format" a one-line edit.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"

// HDR scene (#715). On: the scene + bloom targets are RGBA16F, so additive
// effects (tracers, explosions, the sun, shield flashes) keep their energy
// above 1.0 instead of clipping to flat white, and the composite pass
// tonemaps it back down (see PostProcess::tonemap). Off: the old RGBA8 LDR
// path. Costs twice the scene-target bandwidth of RGBA8.
constexpr bool kHdrScene = true;

// Offscreen scene color attachment. Every scene pipeline declares this
// format, so flipping kHdrScene is the whole switch.
constexpr sg_pixel_format kSceneColorFormat =
    kHdrScene ? SG_PIXELFORMAT_RGBA16F : SG_PIXELFORMAT_RGBA8;
constexpr sg_pixel_format kSceneDepthFormat = SG_PIXELFORMAT_DEPTH;

// Bloom mip chain (#724): level 0 is half res, each next level halves
// again. Same format as the scene so the bright-pass sees real HDR
// energy. More levels = wider glow; each is a tiny extra pass.
constexpr sg_pixel_format kBloomColorFormat = kSceneColorFormat;
constexpr int kBloomLevels = 6;

// Final composite targets the swapchain. On Metal/macOS that's BGRA8;
// sokol's swapchain auto-detection gives us whatever the platform wants.
// We pin it here so pipeline descs can declare it.
constexpr sg_pixel_format kSwapchainColorFormat = SG_PIXELFORMAT_BGRA8;

// MSAA-off everywhere keeps the offscreen path simple (no resolve step).
// Bloom + scene shading tend to hide aliasing anyway. Revisit if shimmer
// bothers us.
constexpr int kSceneSampleCount = 1;
