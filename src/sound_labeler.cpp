// -----------------------------------------------------------------------------
// sound_labeler.cpp — F7 audition + hand-label tool. See header for the why.
//
// This file is deliberately self-contained: a lazy clip scan, a tiny WAV
// header peek for durations, a hardcoded reverse-lookup table (current
// engine bindings, so the user can SEE what's wrong), and a hand-emitted
// JSON writer / json:: reader for the committable labels file. No new deps.
// -----------------------------------------------------------------------------

#include "sound_labeler.h"

#include "audio.h"
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

namespace sound_labeler {

namespace {

// Where the extracted originals live (gitignored, local-only — the same
// bytes sfx.cpp prefers). PAK order, named sfx_NN.wav.
constexpr const char* k_sfx_dir   = "gog_extracted/sfx_wav";
// Where the labels are written — TEXT, committable, sibling of the other
// derived-facts docs. NOT under any gitignored tree.
constexpr const char* k_labels_path = "docs/sound_labels.json";

// Label buffers are fixed-size so ImGui::InputText can write into them
// directly each frame without per-keystroke allocation. 128 is plenty for
// "afterburner / cruise spool" style descriptions.
constexpr int k_label_cap = 128;

// ---- one auditionable clip --------------------------------------------------
struct Clip {
    int         index = -1;            // NN from sfx_NN.wav
    std::string filename;              // "sfx_NN.wav"
    std::string path;                  // full relative path
    SampleId    sample      = 0;       // 0 = failed to load
    float       duration_s  = 0.0f;    // from the WAV header (0 if unknown)
    char        label[k_label_cap] = {0};
    const char* current_use = "(unused)";  // reverse lookup, never null
};

// ---- module-static state ----------------------------------------------------
//
// Hidden by default (dev affordance — shouldn't hijack input on boot). F7
// reveals it; the first reveal triggers the lazy scan+load.
bool             g_visible      = false;
bool             g_loaded       = false;   // lazy scan done?
bool             g_scan_ok      = false;   // did we find the dir + any clips?
std::vector<Clip> g_clips;
VoiceId          g_last_voice   = 0;       // cut the previous one-shot on replay
float            g_play_gain    = 0.8f;    // audition gain (slider)
bool             g_auto_advance = false;   // Enter -> focus+play next row
int              g_focus_row    = -1;      // row to grab keyboard focus next frame
int              g_dirty_saves  = 0;       // edits since last save (HUD only)

// ---- reverse lookup: extracted index -> CURRENT engine binding --------------
//
// Derived from sfx.cpp's load_all() + docs/sfx_gun_mapping.md (the originals
// in assets/sfx/original/ are byte-for-byte copies of these extracted clips,
// so the mapping is exact). This is intentionally a hand-kept table rather
// than a runtime byte-match: it's the ONE place that encodes "what the engine
// believes today", so the user can eyeball which clips are mis-bound. When a
// binding changes in sfx.cpp, update the matching row here. Indices not listed
// are currently unreferenced by any event or gun.
//
// NOTE: this is exactly the heuristic we're trying to REPLACE with the user's
// labels — showing it lets the user confirm/refute each guess by ear.
struct UseRow { int index; const char* use; };
constexpr UseRow k_current_use[] = {
    {  0, "particle_cannon gun" },
    {  2, "meson_blaster gun" },
    {  3, "impact_shield event" },
    {  4, "laser_fire event + Laser/Steltek guns" },
    {  5, "ionic_pulse_cannon gun + lock_seeking event" },
    {  6, "neutron_gun gun" },
    {  8, "plasma_gun gun" },
    {  7, "impact_armor event" },
    { 20, "lock_acquired event" },
    { 22, "engine_hum loop" },
    { 24, "mass_driver gun" },
    { 27, "explosion_big event" },
    { 30, "explosion_small event" },
    { 34, "ui_click event" },
    { 38, "tachyon_cannon gun" },
    { 40, "missile_fire event" },
    { 41, "cruise_windup event (+ jump sting)" },
};

const char* lookup_use(int index) {
    for (const UseRow& r : k_current_use)
        if (r.index == index) return r.use;
    return "(unused)";
}

// ---- tiny WAV header peek for duration --------------------------------------
//
// audio.h doesn't expose a sample's frame count, so read it straight off the
// RIFF header here. Cheap (we only touch the first ~64 bytes of each of the
// ~43 files, once, at load). Returns 0.0 if we can't make sense of it — a
// missing duration is cosmetic, never fatal. Matches audio.cpp's PCM16-only
// world so we don't need to handle exotic encodings.
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
// Read docs/sound_labels.json (if present) and copy any saved labels into the
// matching clips, so a labeling session resumes where it left off. Tolerant
// of a missing/partial file — that's the normal first-run case.
void load_existing_labels() {
    json::Value root = json::parse_file(k_labels_path);
    if (!root.is_object()) return;   // absent or malformed — start fresh
    int restored = 0;
    for (Clip& c : g_clips) {
        char key[16];
        std::snprintf(key, sizeof key, "sfx_%02d", c.index);
        const json::Value* entry = root.find(key);
        if (!entry || !entry->is_object()) continue;
        if (const json::Value* lbl = entry->find("label"); lbl && lbl->is_string()) {
            std::snprintf(c.label, sizeof c.label, "%s", lbl->as_string().c_str());
            if (c.label[0]) ++restored;
        }
    }
    if (restored)
        std::printf("[sound_labeler] restored %d saved label(s) from %s\n",
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

// Hand-emit the committable labels JSON. Flat object keyed by "sfx_NN", each
// value { "label", "current_use" }. We write EVERY clip (even unlabeled ones,
// label="") so the file doubles as a checklist of what's left to do and the
// key set is stable across saves for a clean git diff.
bool save_labels() {
    std::error_code ec;
    fs::create_directories(fs::path(k_labels_path).parent_path(), ec);

    std::string out;
    out += "{\n";
    out += "  \"_comment\": \"Human-labeled SOUNDFX.PAK clips (ground truth). "
           "Generated by the F7 in-game sound labeler. 'current_use' is the "
           "engine's CURRENT (heuristic, possibly wrong) binding for context.\",\n";
    for (size_t i = 0; i < g_clips.size(); ++i) {
        const Clip& c = g_clips[i];
        char key[16];
        std::snprintf(key, sizeof key, "sfx_%02d", c.index);
        out += "  \"";
        out += key;
        out += "\": { \"label\": \"";
        out += json_escape(c.label);
        out += "\", \"current_use\": \"";
        out += json_escape(c.current_use);
        out += "\" }";
        out += (i + 1 < g_clips.size()) ? ",\n" : "\n";
    }
    out += "}\n";

    // Atomic-ish: write a temp then rename, so an interrupted save can't
    // truncate an existing good file.
    const std::string tmp = std::string(k_labels_path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[sound_labeler] could not open %s for write\n",
                         tmp.c_str());
            return false;
        }
        f << out;
    }
    fs::rename(tmp, k_labels_path, ec);
    if (ec) {
        std::fprintf(stderr, "[sound_labeler] rename %s -> %s failed: %s\n",
                     tmp.c_str(), k_labels_path, ec.message().c_str());
        return false;
    }

    int labeled = 0;
    for (const Clip& c : g_clips) if (c.label[0]) ++labeled;
    std::printf("[sound_labeler] saved %d labels to %s (%d of %zu named)\n",
                (int)g_clips.size(), k_labels_path, labeled, g_clips.size());
    g_dirty_saves = 0;
    return true;
}

// ---- lazy clip scan + load --------------------------------------------------
//
// First-open only. Scans k_sfx_dir for sfx_*.wav, sorts by the NN index (NOT
// lexically — sfx_2 vs sfx_10 — though the zero-padded names happen to sort
// right anyway), loads each into a SampleId, peeks its duration, and resolves
// its current-use tag. Missing dir / no clips leaves g_scan_ok false so build()
// shows a friendly "run the extraction first" note instead of crashing.
void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;   // mark first so a failed scan doesn't retry every frame

    std::error_code ec;
    if (!fs::is_directory(k_sfx_dir, ec)) {
        std::printf("[sound_labeler] no extracted sounds at %s/ — "
                    "run tools/extract_soundfx_pak.py first\n", k_sfx_dir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(k_sfx_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        // Match sfx_<digits>.wav and pull the index.
        if (name.rfind("sfx_", 0) != 0) continue;
        if (name.size() < 5 || entry.path().extension() != ".wav") continue;
        int idx = -1;
        if (std::sscanf(name.c_str(), "sfx_%d.wav", &idx) != 1 || idx < 0) continue;

        Clip c;
        c.index       = idx;
        c.filename    = name;
        c.path        = entry.path().string();
        c.sample      = audio::load(c.path);
        c.duration_s  = wav_duration_seconds(c.path);
        c.current_use = lookup_use(idx);
        g_clips.push_back(std::move(c));
    }

    std::sort(g_clips.begin(), g_clips.end(),
              [](const Clip& a, const Clip& b) { return a.index < b.index; });

    g_scan_ok = !g_clips.empty();
    int ok = 0;
    for (const Clip& c : g_clips) if (c.sample != 0) ++ok;
    std::printf("[sound_labeler] loaded %d/%zu extracted clips from %s\n",
                ok, g_clips.size(), k_sfx_dir);

    load_existing_labels();
}

// Play one clip, cutting whatever audition was already going (we only ever
// want to hear one at a time while labeling).
void audition(const Clip& c) {
    if (c.sample == 0) return;
    if (g_last_voice) audio::stop(g_last_voice);
    g_last_voice = audio::play(c.sample, g_play_gain);
}

void stop_all() {
    if (g_last_voice) audio::stop(g_last_voice);
    g_last_voice = 0;
}

} // namespace

// ---- lifecycle --------------------------------------------------------------

void init() {
    std::printf("[sound_labeler] press F7 to open the sound labeler "
                "(labels -> %s)\n", k_labels_path);
}

void shutdown() {}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F7) {
        g_visible = !g_visible;
        if (g_visible) ensure_loaded();   // lazy: pay the scan only on first open
        return true;   // consumed — don't leak F7 to the game
    }
    return false;
}

// ---- build ------------------------------------------------------------------

void build() {
    if (!g_visible) return;

    ImGui::SetNextWindowSize(ImVec2(720.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sound Labeler (F7)", &g_visible)) {
        ImGui::End();
        return;
    }

    if (!g_scan_ok) {
        ImGui::TextWrapped("No extracted sounds found at %s/.", k_sfx_dir);
        ImGui::Spacing();
        ImGui::TextWrapped("Run the extraction first:");
        ImGui::TextDisabled("  python tools/extract_soundfx_pak.py SOUNDFX.PAK gog_extracted/sfx_voc");
        ImGui::TextDisabled("  (then convert the VOCs to gog_extracted/sfx_wav/sfx_NN.wav)");
        ImGui::End();
        return;
    }

    // ---- toolbar -------------------------------------------------------------
    if (ImGui::Button("Save")) save_labels();
    ImGui::SameLine();
    if (ImGui::Button("Stop all")) stop_all();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("gain", &g_play_gain, 0.0f, 1.5f, "%.2f");
    ImGui::SameLine();
    ImGui::Checkbox("auto-advance", &g_auto_advance);

    int labeled = 0;
    for (const Clip& c : g_clips) if (c.label[0]) ++labeled;
    ImGui::Text("%d / %zu labeled%s", labeled, g_clips.size(),
                g_dirty_saves ? "   (unsaved edits)" : "");
    ImGui::TextDisabled("labels save to %s (committable)", k_labels_path);
    ImGui::Separator();

    // ---- the list ------------------------------------------------------------
    ImGui::BeginChild("clip_list", ImVec2(0, 0), false);
    for (int i = 0; i < (int)g_clips.size(); ++i) {
        Clip& c = g_clips[i];
        ImGui::PushID(i);

        // index + filename, fixed-width so the columns line up.
        ImGui::Text("%-12s", c.filename.c_str());
        ImGui::SameLine(120.0f);

        const bool dead = (c.sample == 0);
        if (dead) ImGui::BeginDisabled();
        if (ImGui::Button("Play")) audition(c);
        if (dead) ImGui::EndDisabled();

        ImGui::SameLine();
        if (c.duration_s > 0.0f) ImGui::Text("%5.2fs", c.duration_s);
        else                     ImGui::TextDisabled("  ?  ");

        // The label box. Enter commits + (optionally) advances to the next row.
        ImGui::SameLine(250.0f);
        ImGui::SetNextItemWidth(240.0f);
        if (g_focus_row == i) {
            ImGui::SetKeyboardFocusHere();
            g_focus_row = -1;
        }
        const bool entered = ImGui::InputText(
            "##label", c.label, sizeof c.label, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivatedAfterEdit() || entered) ++g_dirty_saves;
        if (entered && g_auto_advance && i + 1 < (int)g_clips.size()) {
            g_focus_row = i + 1;
            audition(g_clips[i + 1]);   // hear the next one as focus lands
        }

        // current-engine-binding tag — the thing we're trying to verify/replace.
        ImGui::SameLine(500.0f);
        if (std::strcmp(c.current_use, "(unused)") == 0)
            ImGui::TextDisabled("(unused)");
        else
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                               "used by: %s", c.current_use);

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace sound_labeler
