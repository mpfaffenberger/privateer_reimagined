#pragma once
// -----------------------------------------------------------------------------
// audio.h — sokol_audio init + N-voice mixer with 3D attenuation/pan.
//
// The sound substrate for np-3gw: a fixed pool of voices mixed down to
// 44.1kHz stereo in the sokol_audio stream callback. Two playback
// flavors: play() for UI/2D sounds (fixed gain, centered), and
// play_world() for positional sounds — per-frame distance attenuation
// and stereo pan computed against the listener (the camera), updated by
// set_listener() once per frame.
//
// Distance model: inverse-distance clamped —
//     gain = ref_dist / max(dist, ref_dist), 0 beyond max_dist
// i.e. full volume inside ref_dist, 1/d falloff past it, hard silence
// past max_dist (with a short linear fade-out band so sources don't
// pop at the boundary). Pan: constant-power (cos/sin) law driven by
// dot(dir_to_source, listener_right) — equal perceived loudness while
// a source swings ear to ear, the textbook fix for the "hole in the
// middle" of naive linear pan.
//
// ============================ THREADING CONTRACT ============================
// The saudio stream callback runs on a DEDICATED AUDIO THREAD. Everything
// here is built around one rule: the callback NEVER blocks and NEVER
// takes a lock. Coordination is per-voice SPSC via two atomic flags:
//
//   * `alive`  — owned by the MAIN thread for 0->1 (publish), owned by
//                the AUDIO thread for 1->0 (sample finished). The main
//                thread writes EVERY other voice field FIRST and sets
//                `alive` LAST with release ordering; the callback reads
//                `alive` with acquire ordering before touching anything
//                else. Voice slots are only recycled by the main thread
//                when `alive` is false, so the two sides never race on
//                a slot's payload.
//   * `kill`   — main-thread stop/despawn requests. stop() can't clear
//                `alive` directly (the callback might be mid-slot), so
//                it raises `kill` and the callback clears both at the
//                top of its next pass.
//   * `reload` — INSTANT voice-steal handoff (np-3va). The game thread
//                must NEVER block, so it can't wait for a live victim's
//                `alive` to fall before reusing the slot. Instead it
//                writes the new sound's {sample,loop} into the slot's
//                `pending_*` fields, updates the MAIN-OWNED bookkeeping
//                (world_pos/dists/base_gain/gains/started_seq/gen) in
//                place, then raises `reload` LAST (release). The callback
//                consumes `reload` (acquire) at the top of its next pass:
//                it copies pending->active, rewinds the cursor to 0, and
//                the NEW sound begins that same buffer. Only the callback
//                ever writes `sample_idx`/`loop`/`cursor` while a slot is
//                live, so the steal is race free with no wait. (Residual:
//                if the steal lands in the exact buffer the victim
//                one-shot finishes, the retire may win and strand the
//                newcomer — the next alloc sees the slot free and
//                recovers; net effect is at most one dropped SFX, never
//                corruption or a stuck slot.)
//
// Per-frame parameter updates (listener movement, set_voice_gain) write
// the voice's atomic gain fields directly — last-write-wins float
// stores, no ordering requirement beyond atomicity, worst case the
// callback mixes one buffer at a one-frame-stale gain. Inaudible.
//
// Sample PCM data is immutable after load() returns and samples are
// never unloaded, so the callback can read sample buffers with no
// synchronization at all. (If hot-reload ever lands, it must allocate
// NEW sample slots rather than mutating old ones.)
// =============================================================================
//
// Voice stealing (np-3va, non-blocking): on a full pool alloc picks the
// WEAKEST live non-looping voice (lowest current L/R gain, oldest on a
// tie) and steals it INSTANTLY via the `reload` handoff above — the game
// thread never busy-waits. Loops (engine hum) are NEVER eligible: the
// ambience must survive saturation. A steal only happens if the incoming
// sound is at least as loud as that weakest incumbent; a quieter newcomer
// (a distant tiny impact) is DROPPED instead of evicting something the
// player can actually hear. The mixer keeps the full 24-voice pool —
// more pool means less thrash — but two extra valves cap furball load:
// play_world() drops one-shots whose attenuated gain is inaudible before
// they ever take a slot, and a per-frame budget (k_max_world_starts_per_
// frame in audio.cpp) caps NEW world one-shot starts so a single furball
// frame can't flood the mixer.
//
// WAV loading: hand-rolled RIFF/WAVE PCM16 parser (audio.cpp), mono or
// stereo, resampled to the device rate at load time via linear
// interpolation. Same no-third-party-deps ethos as the project's JSON
// and TOML readers. Float32/ADPCM/24-bit files are rejected loudly —
// re-export as PCM16, it's 2026, every tool can.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>
#include <cstdint>
#include <string>

// Opaque-ish ids. 0 = invalid for both (slot 0 is real but ids are
// 1-based to keep the zero-init "no sound" default safe everywhere).
using SampleId = uint32_t;
using VoiceId  = uint32_t;

namespace audio {

// Bring up the device (44.1kHz stereo, stream callback model) and the
// voice pool. Logs `[audio] device <rate>Hz <n>ch`. Safe to call once;
// failure (no audio device, CI box) logs and leaves the system inert —
// every other call becomes a cheap no-op, the game runs silent.
void init();
void shutdown();

// Load a WAV from disk, resampling to the device rate if needed.
// Returns 0 on failure (logged). Sample data lives until shutdown —
// no unload in v1 (the whole SFX set is a few MB).
SampleId load(const std::string& path);

// 2D playback: fixed gain, centered. For UI clicks, comm chatter,
// anything without a position. Returns 0 if the pool had to refuse
// (init failed) — callers may ignore the id entirely.
VoiceId play(SampleId s, float gain = 1.0f);

// 3D playback: gain/pan recomputed every set_listener() call from the
// source position. ref_dist = "full volume inside this radius",
// max_dist = "inaudible past this". A `loop`ed world voice keeps
// playing until stop() — that's the engine-hum path (np-3gw.2).
VoiceId play_world(SampleId s, HMM_Vec3 world_pos,
                   float ref_dist, float max_dist, bool loop = false);

// 2D looping voice for non-positional ambience; pair with
// set_voice_gain to fade (e.g. throttle-driven engine pitch beds).
VoiceId play_loop(SampleId s, float gain);

// Update the listener pose. Call once per frame (Flight mode) BEFORE
// the frame's play_world calls so new voices spatialize against the
// current pose; existing world voices re-spatialize here too.
void set_listener(HMM_Vec3 pos, HMM_Vec3 right);

// Manual control. Both tolerate stale/finished ids (no-op) — a VoiceId
// may outlive its voice when the sample ends or the slot is stolen;
// generation tags make stale ids miss instead of poking a stranger.
void stop(VoiceId v);
void set_voice_gain(VoiceId v, float gain);

// Introspection for the debug panel / logs. voice_gains reads back the
// CURRENT computed L/R gains (post attenuation + pan) — lets trigger
// sites log the real mixer numbers instead of re-deriving them.
// Returns false on stale/finished ids.
int  voices_active();
bool voice_gains(VoiceId v, float* out_l, float* out_r);
bool ready();   // true when the device came up

} // namespace audio
