// -----------------------------------------------------------------------------
// music_labeler.cpp — F8 audition + hand-label tool for the rendered AdLib
// music. See header for the why. Mirrors src/sound_labeler.cpp.
//
// This file is deliberately self-contained: a lazy track scan, a tiny WAV
// header peek for durations, a hand-kept reverse-lookup table (current
// engine state->track use, so the user can confirm/refute it), and a
// hand-emitted JSON writer / json:: reader for the committable labels file.
// No new deps.
//
// The one thing this tool reaches OUT to is the live music layer (music.h):
// while previewing a track it DUCKS the dynamic in-game music so the long
// preview isn't fighting the BASETUNE/COMBAT bed, and it RESTORES that mute
// state the moment the preview stops / the window closes. That handoff is
// the only coupling — everything else is local to this file.
// -----------------------------------------------------------------------------

#include "music_labeler.h"

#include "audio.h"
#include "music.h"
#include "json.h"

#include "imgui.h"
#include "sokol_gfx.h"      // must precede sokol_imgui.h (pipeline types)
#include "sokol_imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace music_labeler {

namespace {

// Where the rendered music lives (gitignored, local-only — the WAVs
// render_music.py writes). One file per track, named <track>.wav.
constexpr const char* k_music_dir   = "gog_extracted/music_wav";
// Where the labels are written — TEXT, committable, sibling of the F7
// sound labels. NOT under any gitignored tree.
constexpr const char* k_labels_path = "docs/music_labels.json";

// Label buffers are fixed-size so ImGui::InputText can write into them
// directly each frame without per-keystroke allocation. 128 is plenty for
// "calm exploration bed / drives the combat snap" style descriptions.
constexpr int k_label_cap = 128;

// ---- one auditionable track -------------------------------------------------
struct Track {
    std::string name;                  // "basetune" (filename stem) — the key
    std::string filename;              // "basetune.wav"
    std::string path;                  // full relative path
    SampleId    sample      = 0;       // 0 = failed to load
    float       duration_s  = 0.0f;    // from the WAV header (0 if unknown)
    char        label[k_label_cap] = {0};
    const char* current_use = "(unused)";  // reverse lookup, never null
    int         order       = 1000;    // canonical sort key (see k_order)
};

// ---- module-static state ----------------------------------------------------
//
// Hidden by default (dev affordance — shouldn't hijack input on boot). F8
// reveals it; the first reveal triggers the lazy scan+load.
bool              g_visible      = false;
bool              g_was_visible  = false;  // close-transition detector (X / F8)
bool              g_loaded       = false;  // lazy scan done?
bool              g_scan_ok      = false;  // did we find the dir + any tracks?
std::vector<Track> g_tracks;
VoiceId           g_preview_voice = 0;     // the single live preview voice
int               g_playing_row   = -1;    // which row is previewing (HUD)
float             g_play_gain      = 0.8f;  // audition gain (slider)
bool              g_auto_advance   = false; // Enter -> focus+play next row
int               g_focus_row      = -1;    // row to grab keyboard focus next frame
int               g_dirty_saves    = 0;     // edits since last save (HUD only)

// ---- live-music ducking handoff ---------------------------------------------
//
// While previewing we mute the dynamic music layer so the long preview track
// doesn't fight the in-game BASETUNE/COMBAT bed. We capture the layer's mute
// state on the FIRST duck and restore it on un-duck so we never clobber the
// debug-panel's own mute toggle or leave the live music silenced behind us.
bool g_ducked      = false;
bool g_prev_muted  = false;

void duck_music() {
    if (g_ducked) return;
    g_prev_muted = music::muted();
    music::set_muted(true);
    g_ducked = true;
}

void unduck_music() {
    if (!g_ducked) return;
    music::set_muted(g_prev_muted);
    g_ducked = false;
}

// ---- reverse lookup: track name -> CURRENT engine state->track use -----------
//
// Derived from music.cpp's update() policy: Flight picks Basetune (calm) or
// Combat (hostiles near, w/ hysteresis); Landed reuses Basetune as the base
// ambience; Opening/Victory/Credits are rendered but NOT yet wired to any
// GameMode (menu/sting/credits hooks). This is the heuristic the user's
// labels are meant to confirm or correct — showing it lets them eyeball it
// by ear. When music.cpp's policy changes, update the matching row here.
// Entries starting with "(unused" render dimmed, like the F7 tool.
struct UseRow { const char* name; const char* use; };
constexpr UseRow k_current_use[] = {
    { "basetune", "Flight ambient + Landed base ambience" },
    { "combat",   "Flight combat (hostiles near)" },
    { "opening",  "(unused — title/menu hook)" },
    { "victory",  "(unused — victory sting hook)" },
    { "credits",  "(unused — credits roll hook)" },
};

const char* lookup_use(const std::string& name) {
    for (const UseRow& r : k_current_use)
        if (name == r.name) return r.use;
    return "(unused)";
}

// Canonical display order (mirrors music.cpp's Track enum). Tracks not in
// this list sort to the end, alphabetically among themselves.
const char* const k_order[] = { "basetune", "combat", "opening",
                                "victory", "credits" };

int order_of(const std::string& name) {
    for (int i = 0; i < (int)(sizeof k_order / sizeof k_order[0]); ++i)
        if (name == k_order[i]) return i;
    return 1000;
}

// ---- tiny WAV header peek for duration --------------------------------------
//
// audio.h doesn't expose a sample's frame count, so read it straight off the
// RIFF header here (same helper shape as the F7 tool). Cheap: we only touch
// the first chunks of each of the handful of files, once, at load. Returns
// 0.0 if we can't make sense of it — a missing duration is cosmetic, never
// fatal. Matches audio.cpp's PCM16-only world.
float wav_duration_seconds(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0.0f;
    char hdr[12] = {0};
    f.read(hdr, 12);
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return 0.0f;

    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
    uint32_t data_bytes = 0;

    // Walk the chunk list: each chunk is [4-byte id][4-byte LE size][payload].
    char id[4];
    while (f.read(id, 4)) {
        uint32_t sz = 0;
        if (!f.read(reinterpret_cast<char*>(&sz), 4)) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            char fmt[16] = {0};
            const uint32_t want = sz < 16 ? sz : 16;
            f.read(fmt, want);
            std::memcpy(&channels,    fmt + 2, 2);
            std::memcpy(&sample_rate, fmt + 4, 4);
            std::memcpy(&bits,        fmt + 14, 2);
            if (sz > want) f.seekg(sz - want, std::ios::cur);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_bytes = sz;
            break;   // got what we need; payload is the rest
        } else {
            f.seekg(sz, std::ios::cur);   // skip unknown chunk
        }
        if (sz & 1) f.seekg(1, std::ios::cur);   // RIFF chunks are word-aligned
    }

    if (sample_rate == 0 || channels == 0 || bits == 0 || data_bytes == 0)
        return 0.0f;
    const uint32_t frame_bytes = channels * (bits / 8);
    if (frame_bytes == 0) return 0.0f;
    const double frames = double(data_bytes) / double(frame_bytes);
    return float(frames / double(sample_rate));
}

// ---- persistence ------------------------------------------------------------
//
// Read docs/music_labels.json (if present) and copy any saved labels into the
// matching tracks (keyed by track name), so a labeling session resumes where
// it left off. Tolerant of a missing/partial file — the normal first run.
void load_existing_labels() {
    json::Value root = json::parse_file(k_labels_path);
    if (!root.is_object()) return;   // absent or malformed — start fresh
    int restored = 0;
    for (Track& t : g_tracks) {
        const json::Value* entry = root.find(t.name);
        if (!entry || !entry->is_object()) continue;
        if (const json::Value* lbl = entry->find("label"); lbl && lbl->is_string()) {
            std::snprintf(t.label, sizeof t.label, "%s", lbl->as_string().c_str());
            if (t.label[0]) ++restored;
        }
    }
    if (restored)
        std::printf("[music_labeler] restored %d saved label(s) from %s\n",
                    restored, k_labels_path);
}

std::string json_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\t': r += "\\t";  break;
            case '\r': r += "\\r";  break;
            default:   r += ch;     break;
        }
    }
    return r;
}

// Hand-emit the committable labels JSON. Flat object keyed by track name,
// each value { "label", "current_use" }. We write EVERY track (even
// unlabeled ones, label="") so the file doubles as a checklist of what's
// left to do and the key set is stable across saves for a clean git diff.
bool save_labels() {
    std::error_code ec;
    fs::create_directories(fs::path(k_labels_path).parent_path(), ec);

    std::string out;
    out += "{\n";
    out += "  \"_comment\": \"Human-labeled rendered AdLib music tracks "
           "(ground truth). Generated by the F8 in-game music labeler. "
           "'current_use' is the engine's CURRENT music.cpp state->track "
           "binding for context.\",\n";
    for (size_t i = 0; i < g_tracks.size(); ++i) {
        const Track& t = g_tracks[i];
        out += "  \"";
        out += json_escape(t.name);
        out += "\": { \"label\": \"";
        out += json_escape(t.label);
        out += "\", \"current_use\": \"";
        out += json_escape(t.current_use);
        out += "\" }";
        out += (i + 1 < g_tracks.size()) ? ",\n" : "\n";
    }
    out += "}\n";

    // Atomic-ish: write a temp then rename, so an interrupted save can't
    // truncate an existing good file.
    const std::string tmp = std::string(k_labels_path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[music_labeler] could not open %s for write\n",
                         tmp.c_str());
            return false;
        }
        f << out;
    }
    fs::rename(tmp, k_labels_path, ec);
    if (ec) {
        std::fprintf(stderr, "[music_labeler] rename %s -> %s failed: %s\n",
                     tmp.c_str(), k_labels_path, ec.message().c_str());
        return false;
    }

    int labeled = 0;
    for (const Track& t : g_tracks) if (t.label[0]) ++labeled;
    std::printf("[music_labeler] saved %d labels to %s (%d of %zu named)\n",
                (int)g_tracks.size(), k_labels_path, labeled, g_tracks.size());
    g_dirty_saves = 0;
    return true;
}

// ---- lazy track scan + load -------------------------------------------------
//
// First-open only. Scans k_music_dir for *.wav, loads each into a SampleId,
// peeks its duration, resolves its current-use tag, and sorts by the
// canonical music.cpp order. Missing dir / no tracks leaves g_scan_ok false
// so build() shows a friendly "run render_music.py first" note instead of
// crashing. Count is NOT hardcoded — whatever WAVs are present get listed.
void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;   // mark first so a failed scan doesn't retry every frame

    std::error_code ec;
    if (!fs::is_directory(k_music_dir, ec)) {
        std::printf("[music_labeler] no rendered music at %s/ — "
                    "run tools/render_music.py first\n", k_music_dir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(k_music_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".wav") continue;

        Track t;
        t.name        = entry.path().stem().string();
        t.filename    = entry.path().filename().string();
        t.path        = entry.path().string();
        t.sample      = audio::load(t.path);
        t.duration_s  = wav_duration_seconds(t.path);
        t.current_use = lookup_use(t.name);
        t.order       = order_of(t.name);
        g_tracks.push_back(std::move(t));
    }

    std::sort(g_tracks.begin(), g_tracks.end(),
              [](const Track& a, const Track& b) {
                  if (a.order != b.order) return a.order < b.order;
                  return a.name < b.name;
              });

    g_scan_ok = !g_tracks.empty();
    int ok = 0;
    for (const Track& t : g_tracks) if (t.sample != 0) ++ok;
    std::printf("[music_labeler] loaded %d/%zu rendered tracks from %s\n",
                ok, g_tracks.size(), k_music_dir);

    load_existing_labels();
}

// Stop whatever preview is going and restore the live music layer. Idempotent
// — safe to call when nothing is playing (the close/teardown path leans on it).
void stop_preview() {
    if (g_preview_voice) audio::stop(g_preview_voice);
    g_preview_voice = 0;
    g_playing_row   = -1;
    unduck_music();   // hand the dynamic music bed back to the game
}

// Preview one track, cutting whatever was already auditioning (only one at a
// time — these tracks are long). Ducks the live music for the duration.
void preview(int row) {
    if (row < 0 || row >= (int)g_tracks.size()) return;
    Track& t = g_tracks[row];
    if (t.sample == 0) return;
    if (g_preview_voice) audio::stop(g_preview_voice);   // cut the previous
    duck_music();                                        // hush the live bed
    g_preview_voice = audio::play(t.sample, g_play_gain);
    g_playing_row   = row;
}

} // namespace

// ---- lifecycle --------------------------------------------------------------

void init() {
    std::printf("[music_labeler] press F8 to open the music labeler "
                "(labels -> %s)\n", k_labels_path);
}

void shutdown() { stop_preview(); }

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F8) {
        g_visible = !g_visible;
        if (g_visible) ensure_loaded();   // lazy: pay the scan only on first open
        else           stop_preview();    // toggling off: hush + hand back music
        return true;   // consumed — don't leak F8 to the game
    }
    return false;
}

// ---- build ------------------------------------------------------------------

void build() {
    // Catch the close transition (X button or F8-off): tear down a stray
    // preview and restore the live music exactly once.
    if (!g_visible) {
        if (g_was_visible) { stop_preview(); g_was_visible = false; }
        return;
    }
    g_was_visible = true;

    ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Music Labeler (F8)", &g_visible)) {
        ImGui::End();
        return;
    }

    if (!g_scan_ok) {
        ImGui::TextWrapped("No rendered music found at %s/.", k_music_dir);
        ImGui::Spacing();
        ImGui::TextWrapped("Render the tracks first:");
        ImGui::TextDisabled("  python tools/render_music.py "
                            "<SOUND_DIR> <adlmidiplay> %s", k_music_dir);
        ImGui::End();
        return;
    }

    // ---- toolbar -------------------------------------------------------------
    if (ImGui::Button("Save")) save_labels();
    ImGui::SameLine();
    if (ImGui::Button("Stop")) stop_preview();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    // Live slider: re-gain the in-flight preview voice so the user can ride
    // the level while a long track plays.
    if (ImGui::SliderFloat("gain", &g_play_gain, 0.0f, 1.5f, "%.2f") &&
        g_preview_voice)
        audio::set_voice_gain(g_preview_voice, g_play_gain);
    ImGui::SameLine();
    ImGui::Checkbox("auto-advance", &g_auto_advance);

    int labeled = 0;
    for (const Track& t : g_tracks) if (t.label[0]) ++labeled;
    ImGui::Text("%d / %zu labeled%s", labeled, g_tracks.size(),
                g_dirty_saves ? "   (unsaved edits)" : "");
    ImGui::TextDisabled("labels save to %s (committable); "
                        "live music ducked while previewing", k_labels_path);
    ImGui::Separator();

    // ---- the list ------------------------------------------------------------
    ImGui::BeginChild("track_list", ImVec2(0, 0), false);
    for (int i = 0; i < (int)g_tracks.size(); ++i) {
        Track& t = g_tracks[i];
        ImGui::PushID(i);

        // name / filename, fixed-width so the columns line up. Mark the row
        // that's currently previewing with a leading '>'.
        ImGui::Text("%c %-16s", (i == g_playing_row) ? '>' : ' ',
                    t.filename.c_str());
        ImGui::SameLine(190.0f);

        const bool dead = (t.sample == 0);
        if (dead) ImGui::BeginDisabled();
        if (ImGui::Button("Play")) preview(i);
        if (dead) ImGui::EndDisabled();

        ImGui::SameLine();
        if (t.duration_s > 0.0f) ImGui::Text("%6.1fs", t.duration_s);
        else                     ImGui::TextDisabled("   ?  ");

        // The label box. Enter commits + (optionally) advances to the next row.
        ImGui::SameLine(330.0f);
        ImGui::SetNextItemWidth(240.0f);
        if (g_focus_row == i) {
            ImGui::SetKeyboardFocusHere();
            g_focus_row = -1;
        }
        const bool entered = ImGui::InputText(
            "##label", t.label, sizeof t.label, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivatedAfterEdit() || entered) ++g_dirty_saves;
        if (entered && g_auto_advance && i + 1 < (int)g_tracks.size()) {
            g_focus_row = i + 1;
            preview(i + 1);   // hear the next one as focus lands
        }

        // current engine state->track tag — the thing we're confirming.
        ImGui::SameLine(580.0f);
        if (std::strncmp(t.current_use, "(unused", 7) == 0)
            ImGui::TextDisabled("%s", t.current_use);
        else
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                               "used by: %s", t.current_use);

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace music_labeler
