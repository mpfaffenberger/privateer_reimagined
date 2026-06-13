// -----------------------------------------------------------------------------
// audio.cpp — voice pool, WAV loader, stream-callback mixer.
//
// Read the THREADING CONTRACT block in audio.h first; every design
// decision below follows from "the callback never locks". Layout:
//
//   1. WAV loader — RIFF/WAVE PCM16 parser + linear resampler.
//   2. Voice pool + spatialization math (main thread).
//   3. stream_cb mixer (audio thread).
// -----------------------------------------------------------------------------

#include "audio.h"

#include "sokol_audio.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <vector>

namespace {

// ---- samples ----------------------------------------------------------------

// Interleaved stereo float frames at the DEVICE rate. Mono WAVs are
// duplicated to both channels at load so the mixer has exactly one
// format to deal with — branch-free inner loop beats saved memory at
// the few-MB scale of an SFX set.
struct Sample {
    std::vector<float> frames;     // L R L R ...; size = frame_count * 2
    size_t             frame_count = 0;
    std::string        path;       // for log lines
};

// Loaded samples. Grows only (no unload in v1). A std::deque, NOT a
// vector: the audio thread holds `const Sample& s = g_samples[idx]`
// across a whole mix buffer while the main thread's load() may
// push_back concurrently. A vector push_back can REALLOCATE the outer
// array, dangling that reference mid-mix (a real data race); deque
// growth never relocates existing elements, so the held reference
// stays valid. (Index addressing is unchanged — same stable scheme as
// placed_ship_sprites in main.cpp.)
std::deque<Sample> g_samples;

// ---- voices -----------------------------------------------------------------

constexpr int k_voice_count = 24;

struct Voice {
    // Payload — written by main thread BEFORE alive goes true; read-only
    // on the audio thread while alive. uint32 sample index (not pointer)
    // into g_samples (a deque — stable element addresses, so the mixer's
    // held Sample& survives a concurrent load() push_back).
    uint32_t sample_idx = 0;
    bool     loop       = false;

    // Mix params — atomically updated by the main thread any time
    // (spatialization, set_voice_gain); read by the callback per buffer.
    // Plain last-write-wins float stores, relaxed ordering (header note).
    std::atomic<float> gain_l{ 0.0f };
    std::atomic<float> gain_r{ 0.0f };

    // Playback cursor in frames — audio thread ONLY (after publish).
    float cursor = 0.0f;

    // Spatialization source data — main thread only. Re-read by
    // set_listener each frame for world voices.
    bool     is_world = false;
    HMM_Vec3 world_pos{ 0.0f, 0.0f, 0.0f };
    float    ref_dist  = 100.0f;
    float    max_dist  = 10000.0f;
    float    base_gain = 1.0f;

    // Steal policy input: saudio-callback-agnostic monotonic counter
    // stamped at play() so the oldest non-loop voice can be found.
    uint64_t started_at_seq = 0;

    // Stale-id protection: bumped every time the slot is (re)used.
    uint32_t generation = 1;

    // ---- SPSC flags (the contract) -----------------------------------
    std::atomic<bool> alive{ false };   // publish: main 0->1, retire: audio 1->0
    std::atomic<bool> kill{ false };    // main raises; audio consumes
};

Voice    g_voices[k_voice_count];
uint64_t g_play_seq = 0;          // main thread only

bool g_ready = false;

// Listener pose — main thread only (spatialize() runs on main).
HMM_Vec3 g_listener_pos{ 0.0f, 0.0f, 0.0f };
HMM_Vec3 g_listener_right{ 1.0f, 0.0f, 0.0f };

// VoiceId encoding: generation << 8 | slot. 24 slots fit in a byte;
// 24 bits of generation wraps after 16M reuses of one slot, which at
// gameplay rates is "never" for stale-id purposes.
VoiceId make_voice_id(int slot, uint32_t gen) {
    return (gen << 8) | (uint32_t)slot;
}
Voice* resolve_voice(VoiceId v, int* out_slot = nullptr) {
    if (v == 0) return nullptr;
    const int      slot = (int)(v & 0xff);
    const uint32_t gen  = v >> 8;
    if (slot >= k_voice_count) return nullptr;
    Voice& vox = g_voices[slot];
    if (vox.generation != gen) return nullptr;   // slot moved on; id is stale
    if (out_slot) *out_slot = slot;
    return &vox;
}

// ---- WAV loader ---------------------------------------------------------------
// Minimal RIFF/WAVE reader: 'RIFF'+'WAVE' signature, then chunk-walk to
// 'fmt ' and 'data'. PCM16 (format tag 1) mono/stereo only — everything
// else is rejected with a loud log naming the file and the reason.
// Chunk-walking (rather than assuming fmt-then-data at fixed offsets)
// survives the LIST/INFO metadata chunks most DAWs insert.

bool load_wav_pcm16(const std::string& path,
                    std::vector<int16_t>& out_pcm,
                    int& out_channels, int& out_rate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[audio] cannot open '%s'\n", path.c_str());
        return false;
    }

    // Total file size, so chunk sizes read off disk can be bounded
    // against the bytes that actually exist (a hostile/corrupt 'data'
    // size would otherwise drive a ~4GB resize — a trivial DoS).
    in.seekg(0, std::ios::end);
    const std::streamoff file_size = in.tellg();
    in.seekg(0, std::ios::beg);

    char riff[4], wave[4];
    uint32_t riff_size = 0;
    in.read(riff, 4);
    in.read(reinterpret_cast<char*>(&riff_size), 4);
    in.read(wave, 4);
    if (!in || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        std::fprintf(stderr, "[audio] '%s' is not a RIFF/WAVE file\n", path.c_str());
        return false;
    }

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false, have_data = false;

    while (in && !(have_fmt && have_data)) {
        char     tag[4];
        uint32_t size = 0;
        in.read(tag, 4);
        in.read(reinterpret_cast<char*>(&size), 4);
        if (!in) break;

        if (std::memcmp(tag, "fmt ", 4) == 0) {
            // The canonical fmt chunk is >=16 bytes; a smaller one is
            // malformed and reading the 16 fields below would walk into
            // the next chunk (and size-16 would underflow). Reject loud.
            if (size < 16) {
                std::fprintf(stderr, "[audio] '%s': fmt chunk too small (%u bytes)\n",
                             path.c_str(), size);
                return false;
            }
            // Read the first 16 canonical bytes; skip any extension.
            uint16_t block_align = 0; uint32_t byte_rate = 0;
            in.read(reinterpret_cast<char*>(&format),      2);
            in.read(reinterpret_cast<char*>(&channels),    2);
            in.read(reinterpret_cast<char*>(&rate),        4);
            in.read(reinterpret_cast<char*>(&byte_rate),   4);
            in.read(reinterpret_cast<char*>(&block_align), 2);
            in.read(reinterpret_cast<char*>(&bits),        2);
            if (size > 16) in.seekg(size - 16, std::ios::cur);
            have_fmt = true;
        } else if (std::memcmp(tag, "data", 4) == 0) {
            // Bound the declared data size against the bytes left in the
            // file: a corrupt/hostile size must never drive a multi-GB
            // allocation. Reject if it overruns rather than truncating
            // silently — a lying header means a broken asset.
            const std::streamoff here      = in.tellg();
            const std::streamoff remaining = (file_size >= here) ? file_size - here : 0;
            if ((std::streamoff)size > remaining) {
                std::fprintf(stderr, "[audio] '%s': data chunk size %u overruns file "
                             "(%lld bytes left) — corrupt\n",
                             path.c_str(), size, (long long)remaining);
                return false;
            }
            out_pcm.resize(size / 2);
            in.read(reinterpret_cast<char*>(out_pcm.data()), (std::streamsize)(out_pcm.size() * 2));
            have_data = true;
        } else {
            // LIST, INFO, fact, cue — skip. Chunks are word-aligned;
            // odd sizes carry a pad byte.
            in.seekg(size + (size & 1), std::ios::cur);
        }
    }

    if (!have_fmt || !have_data) {
        std::fprintf(stderr, "[audio] '%s': missing fmt/data chunk\n", path.c_str());
        return false;
    }
    if (format != 1 || bits != 16 || (channels != 1 && channels != 2)) {
        std::fprintf(stderr, "[audio] '%s': unsupported format (tag=%u bits=%u ch=%u) — "
                             "PCM16 mono/stereo only, re-export\n",
                     path.c_str(), format, bits, channels);
        return false;
    }
    out_channels = channels;
    out_rate     = (int)rate;
    return true;
}

// int16 interleaved (any of mono/stereo, any rate) -> stereo float at
// device rate. Linear interpolation — fine for SFX; nobody A/Bs a
// windowed-sinc resampler on a laser zap.
void convert_to_device(const std::vector<int16_t>& pcm, int channels, int src_rate,
                       int dst_rate, std::vector<float>& out_frames) {
    const size_t src_frames = pcm.size() / (size_t)channels;
    if (src_frames == 0) return;
    const double ratio      = (double)src_rate / (double)dst_rate;
    const size_t dst_frames = (size_t)((double)src_frames / ratio);

    out_frames.resize(dst_frames * 2);
    constexpr float k_inv = 1.0f / 32768.0f;
    for (size_t i = 0; i < dst_frames; ++i) {
        const double pos = (double)i * ratio;
        const size_t i0  = (size_t)pos;
        const size_t i1  = (i0 + 1 < src_frames) ? i0 + 1 : i0;
        const float  t   = (float)(pos - (double)i0);
        for (int c = 0; c < 2; ++c) {
            const int sc = (channels == 2) ? c : 0;   // mono: same sample both ears
            const float a = (float)pcm[i0 * channels + sc] * k_inv;
            const float b = (float)pcm[i1 * channels + sc] * k_inv;
            out_frames[i * 2 + c] = a + (b - a) * t;
        }
    }
}

// ---- spatialization (main thread) --------------------------------------------

// Compute L/R gains for a world voice against the current listener.
// Distance: inverse-clamped with a 15% linear fade band before
// max_dist so sources slip out instead of popping. Pan: constant-power.
void spatialize(Voice& v) {
    const HMM_Vec3 d    = HMM_SubV3(v.world_pos, g_listener_pos);
    const float    dist = HMM_LenV3(d);

    float att = 0.0f;
    if (dist < v.max_dist) {
        att = v.ref_dist / std::fmax(dist, v.ref_dist);
        const float fade_start = v.max_dist * 0.85f;
        if (dist > fade_start) {
            att *= 1.0f - (dist - fade_start) / (v.max_dist - fade_start);
        }
    }

    // Pan in [-1, 1] from the projection onto listener-right. At zero
    // distance the direction is meaningless — center it.
    float pan = 0.0f;
    if (dist > 1e-3f) {
        pan = HMM_DotV3(HMM_DivV3F(d, dist), g_listener_right);
        pan = std::fmax(-1.0f, std::fmin(1.0f, pan));
    }
    // Constant-power law: theta sweeps 0 (hard left) to pi/2 (hard
    // right); L = cos, R = sin keeps L^2+R^2 = 1 across the arc.
    constexpr float k_quarter_pi = 0.78539816339f;
    const float theta = (pan + 1.0f) * k_quarter_pi;
    const float g     = v.base_gain * att;
    v.gain_l.store(g * std::cos(theta), std::memory_order_relaxed);
    v.gain_r.store(g * std::sin(theta), std::memory_order_relaxed);
}

// ---- the mixer (AUDIO THREAD) -------------------------------------------------
// Straight-line per contract: scan voices, acquire-check alive, mix,
// retire finished ones. No locks, no allocation, no logging (stdio on
// the audio thread risks priority-inversion hiccups).

void stream_cb(float* buffer, int num_frames, int num_channels) {
    std::memset(buffer, 0, (size_t)num_frames * (size_t)num_channels * sizeof(float));

    for (Voice& v : g_voices) {
        if (!v.alive.load(std::memory_order_acquire)) continue;
        if (v.kill.load(std::memory_order_relaxed)) {
            v.kill.store(false, std::memory_order_relaxed);
            v.alive.store(false, std::memory_order_release);
            continue;
        }
        const Sample& s = g_samples[v.sample_idx];
        if (s.frame_count == 0) {   // defensive; load() never publishes empties
            v.alive.store(false, std::memory_order_release);
            continue;
        }

        const float gl = v.gain_l.load(std::memory_order_relaxed);
        const float gr = v.gain_r.load(std::memory_order_relaxed);

        float cursor = v.cursor;
        bool  done   = false;
        for (int f = 0; f < num_frames; ++f) {
            if (cursor >= (float)s.frame_count) {
                if (v.loop) {
                    cursor = 0.0f;
                } else {
                    done = true;
                    break;
                }
            }
            const size_t fi = (size_t)cursor;
            buffer[f * 2 + 0] += s.frames[fi * 2 + 0] * gl;
            buffer[f * 2 + 1] += s.frames[fi * 2 + 1] * gr;
            cursor += 1.0f;   // sample is already at device rate (resampled on load)
        }
        v.cursor = cursor;
        if (done) v.alive.store(false, std::memory_order_release);
    }

    // Soft clamp. tanh flattens the worst-case 24-voice pileup into
    // gentle saturation instead of digital wrap-harshness; for the
    // common single-digit-voice case tanh(x) ~ x and it's transparent.
    for (int i = 0; i < num_frames * num_channels; ++i) {
        buffer[i] = std::tanh(buffer[i]);
    }
}

// ---- voice allocation (main thread) -------------------------------------------

// Find a slot: free first, else steal the oldest non-looping voice.
// Returns -1 only when every slot is a loop (refuse rather than cut
// ambience — see header).
int alloc_slot() {
    for (int i = 0; i < k_voice_count; ++i) {
        if (!g_voices[i].alive.load(std::memory_order_acquire)) return i;
    }
    int      oldest     = -1;
    uint64_t oldest_seq = UINT64_MAX;
    for (int i = 0; i < k_voice_count; ++i) {
        if (g_voices[i].loop) continue;
        if (g_voices[i].started_at_seq < oldest_seq) {
            oldest_seq = g_voices[i].started_at_seq;
            oldest     = i;
        }
    }
    if (oldest >= 0) {
        std::printf("[audio] voice pool full — stealing slot %d\n", oldest);
        // Raise kill and wait for nothing: we overwrite the payload
        // below only after alive flips false. One callback period
        // (~5ms at 256 frames) of spin is the worst case; in practice
        // the next buffer retires it. Busy-wait is acceptable on this
        // rare, logged path; a lock in the callback is not.
        g_voices[oldest].kill.store(true, std::memory_order_relaxed);
        // Bounded spin: normally the next callback (~5ms) retires it.
        // If the audio thread has stalled we must NOT hang the main
        // thread forever — cap the wait at a few ms and refuse the steal
        // (the kill flag stays raised; the voice retires whenever the
        // callback next runs). Refusing one rare SFX beats a freeze.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        while (g_voices[oldest].alive.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::printf("[audio] steal spin timed out — refusing voice\n");
                return -1;
            }
        }
    }
    return oldest;
}

// Publish a configured voice (contract: payload first, alive last).
VoiceId publish(int slot, uint32_t sample_idx, bool loop, bool is_world,
                HMM_Vec3 pos, float ref_d, float max_d, float gain) {
    Voice& v = g_voices[slot];
    v.sample_idx     = sample_idx;
    v.loop           = loop;
    v.cursor         = 0.0f;
    v.is_world       = is_world;
    v.world_pos      = pos;
    v.ref_dist       = std::fmax(ref_d, 1.0f);
    v.max_dist       = std::fmax(max_d, v.ref_dist + 1.0f);
    v.base_gain      = gain;
    v.started_at_seq = ++g_play_seq;
    v.generation++;
    // Clear any stale kill from this slot's PREVIOUS occupant before we
    // publish: otherwise an old stop() that raced in could be consumed
    // against this fresh voice and silence it on its very first buffer.
    v.kill.store(false, std::memory_order_relaxed);

    if (is_world) {
        spatialize(v);   // initial gains from current listener pose
    } else {
        // Centered constant-power: cos(pi/4) both sides.
        constexpr float k_center = 0.70710678f;
        v.gain_l.store(gain * k_center, std::memory_order_relaxed);
        v.gain_r.store(gain * k_center, std::memory_order_relaxed);
    }

    v.alive.store(true, std::memory_order_release);   // publish — LAST write
    return make_voice_id(slot, v.generation);
}

} // namespace

namespace audio {

void init() {
    saudio_desc d{};
    d.sample_rate        = 44100;
    d.num_channels       = 2;
    d.stream_cb          = stream_cb;
    d.logger.func        = nullptr;   // sokol default
    saudio_setup(&d);

    g_ready = saudio_isvalid();
    if (!g_ready) {
        std::fprintf(stderr, "[audio] device init FAILED — running silent\n");
        return;
    }
    // Reserve sample index 0 as the "invalid" sentinel so SampleId 0
    // can mean "no sample" without an offset dance at every lookup.
    g_samples.clear();
    g_samples.push_back(Sample{});
    std::printf("[audio] device %dHz %dch, %d voices, buffer %d frames\n",
                saudio_sample_rate(), saudio_channels(),
                k_voice_count, saudio_buffer_frames());
}

void shutdown() {
    if (!g_ready) return;
    saudio_shutdown();   // joins the audio thread; voices can't race us after
    g_ready = false;
}

SampleId load(const std::string& path) {
    if (!g_ready) return 0;

    std::vector<int16_t> pcm;
    int channels = 0, rate = 0;
    if (!load_wav_pcm16(path, pcm, channels, rate)) return 0;

    Sample s;
    s.path = path;
    convert_to_device(pcm, channels, rate, saudio_sample_rate(), s.frames);
    s.frame_count = s.frames.size() / 2;
    if (s.frame_count == 0) {
        std::fprintf(stderr, "[audio] '%s': empty after conversion\n", path.c_str());
        return 0;
    }

    g_samples.push_back(std::move(s));
    const SampleId id = (SampleId)(g_samples.size() - 1);
    std::printf("[audio] loaded '%s' (%zu frames @ %dHz, src %dHz %dch)\n",
                path.c_str(), g_samples[id].frame_count,
                saudio_sample_rate(), rate, channels);
    return id;
}

VoiceId play(SampleId s, float gain) {
    if (!g_ready || s == 0 || s >= g_samples.size()) return 0;
    const int slot = alloc_slot();
    if (slot < 0) return 0;
    return publish(slot, s, /*loop=*/false, /*world=*/false,
                   HMM_V3(0, 0, 0), 1.0f, 2.0f, gain);
}

VoiceId play_loop(SampleId s, float gain) {
    if (!g_ready || s == 0 || s >= g_samples.size()) return 0;
    const int slot = alloc_slot();
    if (slot < 0) return 0;
    return publish(slot, s, /*loop=*/true, /*world=*/false,
                   HMM_V3(0, 0, 0), 1.0f, 2.0f, gain);
}

VoiceId play_world(SampleId s, HMM_Vec3 world_pos,
                   float ref_dist, float max_dist, bool loop) {
    if (!g_ready || s == 0 || s >= g_samples.size()) return 0;

    // Audibility cull: a one-shot spawned beyond (or effectively at)
    // max_dist can never be heard — attenuation is ~0 and one-shots
    // don't live long enough for the listener to close the gap. Refuse
    // the voice instead of burning a pool slot; a 17-ship furball at
    // 5km+ otherwise floods the pool with silence and steals slots
    // from sounds the player can actually hear. Loops are exempt: a
    // looping source persists, so the listener CAN fly into range.
    if (!loop) {
        const float dist = HMM_LenV3(HMM_SubV3(world_pos, g_listener_pos));
        const float att  = (dist < max_dist) ? ref_dist / std::fmax(dist, ref_dist) : 0.0f;
        constexpr float k_audible_floor = 0.01f;   // < -40dB: nobody hears it
        if (att < k_audible_floor) return 0;
    }

    const int slot = alloc_slot();
    if (slot < 0) return 0;
    return publish(slot, s, loop, /*world=*/true,
                   world_pos, ref_dist, max_dist, 1.0f);
}

void set_listener(HMM_Vec3 pos, HMM_Vec3 right) {
    if (!g_ready) return;
    g_listener_pos   = pos;
    g_listener_right = right;
    for (Voice& v : g_voices) {
        if (!v.alive.load(std::memory_order_acquire)) continue;
        if (v.is_world) spatialize(v);
    }
}

void stop(VoiceId id) {
    if (Voice* v = resolve_voice(id); v) {
        v->kill.store(true, std::memory_order_relaxed);
    }
}

void set_voice_gain(VoiceId id, float gain) {
    if (Voice* v = resolve_voice(id); v) {
        v->base_gain = gain;
        if (v->is_world) {
            spatialize(*v);
        } else {
            constexpr float k_center = 0.70710678f;
            v->gain_l.store(gain * k_center, std::memory_order_relaxed);
            v->gain_r.store(gain * k_center, std::memory_order_relaxed);
        }
    }
}

bool voice_gains(VoiceId id, float* out_l, float* out_r) {
    Voice* v = resolve_voice(id);
    if (!v || !v->alive.load(std::memory_order_acquire)) return false;
    if (out_l) *out_l = v->gain_l.load(std::memory_order_relaxed);
    if (out_r) *out_r = v->gain_r.load(std::memory_order_relaxed);
    return true;
}

int voices_active() {
    int n = 0;
    for (Voice& v : g_voices) {
        if (v.alive.load(std::memory_order_acquire)) ++n;
    }
    return n;
}

bool ready() { return g_ready; }

} // namespace audio
