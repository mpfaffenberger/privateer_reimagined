// -----------------------------------------------------------------------------
// fixers.cpp — fixer registry, gating, actions + the Bar screen body.
// See fixers.h for the model; assets/data/fixers.json for the content.
// -----------------------------------------------------------------------------

#include "fixers.h"

#include "json.h"
#include "player.h"
#include "plot.h"

#include <algorithm>
#include <cstdio>

#ifndef FIXERS_HEADLESS
#include "audio.h"        // audio::play_file / stop (per-paragraph voice)
#include "base_screens.h"
#include "music.h"        // request_bar_track (per-scene music direction)
#include "material.h"     // TextureSlot + load_texture_png (portrait art)
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"    // must precede sokol_imgui.h
#include "sokol_imgui.h"  // simgui_imtextureid
#include <map>
#endif

namespace fixers {
namespace {

std::vector<FixerDef> g_fixers;
std::function<void(const std::string&)> g_observer;

void notify(const std::string& what) {
    if (g_observer) g_observer(what);
}

void read_string_array(const json::Value& obj, const char* key,
                       std::vector<std::string>& out) {
    if (const json::Value* a = obj.find(key); a && a->is_array()) {
        for (const json::Value& v : a->as_array())
            if (v.is_string()) out.push_back(v.as_string());
    }
}

} // namespace

int load(const std::string& path) {
    g_fixers.clear();
    const json::Value root = json::parse_file(path);
    if (!root.is_object() || !root.contains("fixers") ||
        !root["fixers"].is_array()) {
        std::fprintf(stderr, "[fixers] no usable table at %s (registry empty)\n",
                     path.c_str());
        return 0;
    }
    for (const json::Value& e : root["fixers"].as_array()) {
        if (!e.is_object()) continue;
        FixerDef f;
        f.id         = e.contains("id")   ? e["id"].string_or("")   : "";
        f.name       = e.contains("name") ? e["name"].string_or("") : "";
        f.base_id    = e.contains("base") ? e["base"].string_or("") : "";
        f.screen     = e.contains("screen") ? e["screen"].string_or("Bar") : "Bar";
        read_string_array(e, "archetypes",     f.archetypes);
        read_string_array(e, "exclude_bases",  f.exclude_bases);
        read_string_array(e, "requires_flags", f.requires_flags);
        read_string_array(e, "forbids_flags",  f.forbids_flags);
        read_string_array(e, "dialogue",       f.dialogue);
        read_string_array(e, "speaker",        f.speaker);
        if (const json::Value* cast = e.find("cast"); cast && cast->is_object()) {
            for (const auto& [key, value] : cast->as_object()) {
                if (!value.is_object()) continue;
                CastMember member;
                member.name = value.contains("name")
                            ? value["name"].string_or(key) : key;
                member.portrait = value.contains("portrait")
                                ? value["portrait"].string_or("") : "";
                f.cast.emplace(key, std::move(member));
            }
        }
        read_string_array(e, "prop",           f.prop);
        read_string_array(e, "accept_dialogue", f.accept_dialogue);
        read_string_array(e, "accept_speaker",  f.accept_speaker);
        read_string_array(e, "accept_voice",    f.accept_voice);
        read_string_array(e, "accept_prop",     f.accept_prop);
        read_string_array(e, "refuse_dialogue", f.refuse_dialogue);
        read_string_array(e, "refuse_speaker",  f.refuse_speaker);
        read_string_array(e, "refuse_voice",    f.refuse_voice);
        f.offer_text = e.contains("offer") ? e["offer"].string_or("") : "";
        f.portrait   = e.contains("portrait") ? e["portrait"].string_or("") : "";
        f.portrait_pc = e.contains("portrait_pc")
                        ? e["portrait_pc"].string_or("") : "";
        read_string_array(e, "voice", f.voice);
        f.voice_offer = e.contains("voice_offer")
                            ? e["voice_offer"].string_or("") : "";
        f.music = e.contains("music")
                            ? (int)e["music"].number_or(0.0) : 0;
        read_string_array(e, "accept_actions", f.accept_actions);
        read_string_array(e, "refuse_actions", f.refuse_actions);
        read_string_array(e, "done_actions",   f.done_actions);
        if (f.id.empty()) {
            std::fprintf(stderr, "[fixers] skipping entry with no id\n");
            continue;
        }
        g_fixers.push_back(std::move(f));
    }
    std::printf("[fixers] loaded %zu fixer entries from %s\n",
                g_fixers.size(), path.c_str());
    return (int)g_fixers.size();
}

std::vector<const FixerDef*> present_at(const std::string& base_id,
                                        const std::string& archetype,
                                        const PlayerState& player) {
    std::vector<const FixerDef*> out;
    for (const FixerDef& f : g_fixers) {
        // ---- placement: exact base OR archetype-minus-exclusions ----
        // Base match tolerates the nav-data type suffix: an entry authored
        // "new_detroit" also matches "new_detroit_industrial" (same rule
        // as base_screens' folder resolver, prefix + '_').
        bool placed = !f.base_id.empty() &&
                      (f.base_id == base_id ||
                       (base_id.size() > f.base_id.size() &&
                        base_id.rfind(f.base_id + "_", 0) == 0));
        if (!placed && !f.archetypes.empty()) {
            const bool arch_ok = std::find(f.archetypes.begin(),
                                           f.archetypes.end(),
                                           archetype) != f.archetypes.end();
            const bool excluded = std::find(f.exclude_bases.begin(),
                                            f.exclude_bases.end(),
                                            base_id) != f.exclude_bases.end();
            placed = arch_ok && !excluded;
        }
        if (!placed) continue;

        // ---- plot gate ----
        bool ok = true;
        for (const std::string& fl : f.requires_flags)
            if (!plot::has_flag(player, fl)) { ok = false; break; }
        if (ok)
            for (const std::string& fl : f.forbids_flags)
                if (plot::has_flag(player, fl)) { ok = false; break; }
        if (ok) out.push_back(&f);
    }
    return out;
}

const FixerDef* find(const std::string& id) {
    for (const FixerDef& f : g_fixers)
        if (f.id == id) return &f;
    return nullptr;
}

// The player-character is fixed (Grayson Burrows, assets/data/characters.json),
// so these are constants rather than save-state. If a player-chosen name is
// ever added, this is the single seam that needs to read it.
std::string expand_tokens(const std::string& text) {
    static const std::string k_surname  = "Burrows";
    static const std::string k_callsign = "Grayson";
    const bool has_token = text.find('$') != std::string::npos;
    const bool has_pause = text.find("<#") != std::string::npos;
    if (!has_token && !has_pause) return text;  // fast path

    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ) {
        if (text.compare(i, 2, "<#") == 0) {
            const size_t end = text.find("#>", i + 2);
            if (end != std::string::npos) {
                i = end + 2;  // TTS pause markup is audio-only, never subtitle text
                continue;
            }
        }
        if (text[i] == '$' && text.compare(i, 3, "$NM") == 0) {
            out += k_surname;  i += 3;
        } else if (text[i] == '$' && text.compare(i, 3, "$CS") == 0) {
            out += k_callsign; i += 3;
        } else {
            out += text[i++];
        }
    }
    return out;
}

bool accept(const FixerDef& f, PlayerState& player) {
    std::printf("[fixers] ACCEPT '%s'\n", f.id.c_str());
    if (!plot::run_actions(player, f.accept_actions)) {
        notify("accept_rejected: " + f.id);
        return false;
    }
    notify("accepted: " + f.id);
    return true;
}

void refuse(const FixerDef& f, PlayerState& player) {
    std::printf("[fixers] REFUSE '%s'\n", f.id.c_str());
    plot::run_actions(player, f.refuse_actions);
    notify("refused: " + f.id);
}

void dialogue_done(const FixerDef& f, PlayerState& player) {
    std::printf("[fixers] dialogue done '%s'\n", f.id.c_str());
    plot::run_actions(player, f.done_actions);
    notify("dialogue_done: " + f.id);
}

void set_observer(std::function<void(const std::string&)> fn) {
    g_observer = std::move(fn);
}

void note_offered(const FixerDef& f, const std::string& base_id) {
    notify("offered: " + f.id + " @ " + base_id);
}

// ---------------------------------------------------------------------------
// Bar screen body (live build only).
// ---------------------------------------------------------------------------
#ifndef FIXERS_HEADLESS
namespace {

// Conversation state — which fixer the player is talking to and how far
// along. Reset when the bar is entered for a different base (or after the
// conversation closes). `g_noted` de-dupes the "offered" observer line per
// bar visit.
// Which stretch of dialogue we're playing. MAIN runs up to the offer; ACCEPTED
// and REFUSED are the short exchanges AFTER the player commits, so a scene
// ends on a line rather than on a button press. Gameplay actions fire at the
// decision itself; the epilogue is presentation only. g_actions_ran prevents
// abandoned/finished epilogues from applying the decision twice.
enum class Phase { Main, Accepted, Refused };
Phase                    g_phase = Phase::Main;
bool                     g_actions_ran = false;
std::string              g_decision_error;

std::string              g_talking_to;     // fixer id, "" = browsing the bar
size_t                   g_paragraph = 0;
std::string              g_last_base;      // bar-visit change detector
std::vector<std::string> g_noted;          // fixers already logged this visit

constexpr ImU32 kAmber    = IM_COL32(255, 176,  48, 255);
constexpr ImU32 kWhite    = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim      = IM_COL32(160, 150, 130, 255);
constexpr ImU32 kBackdrop = IM_COL32(8, 10, 14, 222);

// Portrait art lives under assets/cinematics/ so fixer entries and cinematic
// `line` cues can share the exact same PNGs (portraits/<char>/_ref.png).
const std::string k_portrait_root = "assets/cinematics/";

// Portrait size in the conversation panel. 4:5 to match the pinned 512x640
// source art, scaled to sit comfortably beside the text column.
constexpr float kPortraitW = 192.0f;
constexpr float kPortraitH = 240.0f;

// Lazy-loaded portrait textures, keyed by relative path. A failed load is
// negative-cached as an invalid slot so we log once and never retry per-frame
// (same contract as cinematic.cpp's portrait_texture).
std::map<std::string, TextureSlot> g_portrait_tex;

// The voice clip currently playing for a paragraph, so advancing (or leaving)
// cuts it rather than letting lines stack on top of each other.
VoiceId g_voice = 0;

// ---- auto-advance --------------------------------------------------------
// Conversations play themselves: a paragraph holds until its voice clip
// finishes, then the next one starts. No clicking through a performance.
//
// "Finished" is detected via audio::voice_gains, which returns false once the
// voice slot retires -- no new audio API needed. Unvoiced paragraphs (and the
// whole no-key case) fall back to a READ-TIME estimate so text-only content
// still paces sensibly instead of freezing or flashing past.
float g_para_elapsed = 0.0f;   // seconds shown
float g_para_hold    = 0.0f;   // read-time estimate for the current paragraph
bool  g_auto_advance = true;   // player can pin a paragraph (see controls)

// A BEAT is a silent reaction line, authored as "...": the character says
// nothing but the moment needs to land (being handed an alien artifact; being
// shown a dead colleague's gun-camera footage). Rendered as a held pause with
// the speaker's portrait still up, rather than printing literal dots.
bool is_beat(const std::string& text) {
    for (char c : text)
        if (c != '.' && c != ' ') return false;
    return !text.empty();
}

// Breathing room after a voice clip ends, before the next line starts. Long
// enough that delivery doesn't run together, short enough to keep the scene
// moving.
constexpr float k_post_clip_beat = 0.45f;

// ~14 chars/sec is a comfortable subtitle rate; clamp so one-liners still
// linger and long paragraphs don't overstay if their clip is missing.
// Silent beats get a short fixed hold -- long enough to read as a reaction,
// short enough not to stall the scene.
float read_time_for(const std::string& text) {
    if (is_beat(text)) return 1.1f;
    return std::clamp(1.6f + (float)text.size() / 14.0f, 2.2f, 11.0f);
}

// True once the current paragraph has had its say.
bool paragraph_done() {
    float l = 0.0f, r = 0.0f;
    if (g_voice) {
        if (audio::voice_gains(g_voice, &l, &r))
            return false;                   // clip still playing
        // The clip has finished. Retire the id NOW and restart the clock, so
        // the trailing pause is measured from the end of the audio rather
        // than from the start of the line (g_para_elapsed is already several
        // seconds by this point). Without this the beat is zero-length and
        // lines snap past each other the instant the voice stops.
        g_voice = 0;
        g_para_elapsed = 0.0f;
        g_para_hold = k_post_clip_beat;
        return false;
    }
    return g_para_elapsed >= g_para_hold;   // unvoiced (or post-clip beat)
}

const TextureSlot& portrait_texture(const std::string& rel_path) {
    auto it = g_portrait_tex.find(rel_path);
    if (it != g_portrait_tex.end()) return it->second;
    TextureSlot slot;
    const std::string full = k_portrait_root + rel_path;
    if (!load_texture_png(full, slot)) {
        std::printf("[fixers] portrait missing/undecodable: %s (text only)\n",
                    full.c_str());
        slot.valid = false;   // negative-cache
    }
    auto [ins, ok] = g_portrait_tex.emplace(rel_path, slot);
    (void)ok;
    return ins->second;
}

void stop_voice() {
    if (g_voice) { audio::stop(g_voice); g_voice = 0; }
}

// ---- phase-aware accessors ----------------------------------------------
// Each phase has its own parallel arrays; everything below reads through
// these so the drawing code never branches on the phase itself.
const std::vector<std::string>& phase_dialogue(const FixerDef& f) {
    if (g_phase == Phase::Accepted) return f.accept_dialogue;
    if (g_phase == Phase::Refused)  return f.refuse_dialogue;
    return f.dialogue;
}

const std::vector<std::string>& phase_speaker(const FixerDef& f) {
    if (g_phase == Phase::Accepted) return f.accept_speaker;
    if (g_phase == Phase::Refused)  return f.refuse_speaker;
    return f.speaker;
}

const std::vector<std::string>& phase_voice(const FixerDef& f) {
    if (g_phase == Phase::Accepted) return f.accept_voice;
    if (g_phase == Phase::Refused)  return f.refuse_voice;
    return f.voice;
}

const std::vector<std::string>& phase_prop(const FixerDef& f) {
    static const std::vector<std::string> k_none;
    if (g_phase == Phase::Accepted) return f.accept_prop;
    if (g_phase == Phase::Refused)  return k_none;
    return f.prop;
}

// Play the clip for paragraph `idx` (no-op when the entry has no voice array,
// the index is past its end, or the entry is blank -- all normal for
// partially-voiced content).
void play_paragraph_voice(const FixerDef& f, size_t idx) {
    stop_voice();
    const std::vector<std::string>& dlg = phase_dialogue(f);
    const std::vector<std::string>& vox = phase_voice(f);
    g_para_elapsed = 0.0f;
    g_para_hold = read_time_for(idx < dlg.size() ? dlg[idx] : "");
    if (idx >= vox.size() || vox[idx].empty()) return;
    g_voice = audio::play_file(k_portrait_root + vox[idx], 1.0f);
}

// Enter a post-decision exchange. Returns false when the fixer has no lines
// authored for that outcome, so the caller can close immediately.
bool begin_phase(const FixerDef& f, Phase p) {
    g_phase = p;
    if (phase_dialogue(f).empty()) { g_phase = Phase::Main; return false; }
    g_paragraph = 0;
    g_auto_advance = true;
    play_paragraph_voice(f, 0);
    return true;
}

void reset_conversation() {
    stop_voice();
    g_talking_to.clear();
    g_paragraph = 0;
    g_para_elapsed = 0.0f;
    g_auto_advance = true;
    g_phase = Phase::Main;
    g_actions_ran = false;
    g_decision_error.clear();
}

bool run_decision_actions(const FixerDef& f, PlayerState& player, Phase phase) {
    if (g_actions_ran) return true;
    if (phase == Phase::Main) return false;
    if (phase == Phase::Accepted && !accept(f, player)) return false;
    if (phase == Phase::Refused) refuse(f, player);
    g_actions_ran = true;
    return true;
}

// Who speaks paragraph i -- "" (or past the end of the array) means the fixer.
bool is_pc_line(const FixerDef& f, size_t idx) {
    const std::vector<std::string>& sp = phase_speaker(f);
    return idx < sp.size() && sp[idx] == "pc";
}

void draw_fixer_body(BaseContext& ctx, const std::string& screen) {
    const std::string visit_key = ctx.base_id + ":" + screen;
    if (visit_key != g_last_base) {             // new room visit
        // Safety net for legacy/injected conversation state. Normal button
        // decisions already commit before their epilogue starts.
        if ((g_phase == Phase::Accepted || g_phase == Phase::Refused) &&
            !g_talking_to.empty() && ctx.player) {
            if (const FixerDef* f = find(g_talking_to))
                run_decision_actions(*f, *ctx.player, g_phase);
        }
        g_last_base = visit_key;
        reset_conversation();
        g_noted.clear();
    }
    if (!ctx.player) return;
    PlayerState& player = *ctx.player;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const std::vector<const FixerDef*> base_present =
        present_at(ctx.base_id, ctx.archetype, player);
    std::vector<const FixerDef*> present;
    for (const FixerDef* f : base_present)
        if (f->screen == screen) present.push_back(f);
    for (const FixerDef* f : present) {
        if (std::find(g_noted.begin(), g_noted.end(), f->id) == g_noted.end()) {
            g_noted.push_back(f->id);
            note_offered(*f, ctx.base_id);
        }
    }

    // Music direction is GAME STATE, not conversation state (issue #265):
    // the first present (plot-gated) fixer with an authored track holds the
    // bar's music for the whole visit -- before, during, and after the
    // conversation panel. Only non-zero requests are made (issue #267): when
    // an accept/refuse/done empties the roster mid-visit, the established
    // mood keeps playing; music.cpp clears the soft request when the player
    // actually leaves the Bar screen.
    if (screen == "Bar") {
        for (const FixerDef* f : present) {
            if (f->music > 0) { music::request_bar_track(f->music); break; }
        }
    }

    // ---- browsing: bartender flavor + one talk button per fixer ----------
    if (g_talking_to.empty()) {
        const float bx = 28.0f, bw = 300.0f;
        float by = disp.y - 96.0f - 40.0f * (float)present.size();
        const char* empty_text = screen == "Library"
            ? "The archive terminal waits in disciplined silence."
            : "The bartender polishes a glass. Nothing new.";
        dl->AddText(ImVec2(bx, by - 26.0f), kDim,
                    present.empty() ? empty_text
                                    : "Someone here wants a word...");
        for (const FixerDef* f : present) {
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            char label[96];
            std::snprintf(label, sizeof(label), "Talk to %s", f->name.c_str());
            if (ImGui::Button(label, ImVec2(bw, 32))) {
                sfx::ui_click();
                talk_to(*f);   // same seam as POST /fixer talk (music + voice)
            }
            by += 40.0f;
        }
        return;
    }

    // ---- conversation panel ----------------------------------------------
    const FixerDef* f = find(g_talking_to);
    if (!f) { reset_conversation(); return; }

    // Advance the clock and roll to the next paragraph when the current one
    // has finished speaking. Done BEFORE drawing so a completed paragraph
    // never renders a stale frame.
    const std::vector<std::string>& dlg = phase_dialogue(*f);
    const bool has_dialogue_now = !dlg.empty();
    const bool at_last = !has_dialogue_now || g_paragraph + 1 >= dlg.size();
    g_para_elapsed += ImGui::GetIO().DeltaTime;
    if (g_auto_advance && !at_last && paragraph_done()) {
        ++g_paragraph;
        play_paragraph_voice(*f, g_paragraph);
    }

    const std::vector<std::string>& props = phase_prop(*f);
    const bool has_prop = g_paragraph < props.size() &&
                          !props[g_paragraph].empty();

    const std::vector<std::string>& speakers = phase_speaker(*f);
    const std::string speaker_key = g_paragraph < speakers.size()
                                  ? speakers[g_paragraph] : "";
    const bool pc_line = speaker_key == "pc";
    const auto cast_it = f->cast.find(speaker_key);
    const bool cast_line = cast_it != f->cast.end();
    const std::string art_rel = has_prop
        ? props[g_paragraph]
        : pc_line && !f->portrait_pc.empty() ? f->portrait_pc
        : cast_line ? cast_it->second.portrait
        : f->portrait;
    const TextureSlot* art = nullptr;
    if (!art_rel.empty()) {
        const TextureSlot& slot = portrait_texture(art_rel);
        if (slot.valid) art = &slot;
    }
    const float art_gutter = art ? (kPortraitW + 18.0f) : 0.0f;

    const float pw = std::min(disp.x * 0.62f, 760.0f) + art_gutter;
    const float ph = art ? std::max(240.0f, kPortraitH + 32.0f) : 240.0f;
    const float px = (disp.x - pw) * 0.5f;
    const float py = disp.y - ph - 84.0f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), kBackdrop, 8.0f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), kAmber, 8.0f);

    if (art) {
        const ImVec2 pmin(px + 12, py + 12);
        const ImVec2 pmax(pmin.x + kPortraitW, pmin.y + kPortraitH);
        dl->AddImage(simgui_imtextureid(art->view), pmin, pmax);
        dl->AddRect(pmin, pmax, kDim, 4.0f);
    }

    const float tx = px + 18.0f + art_gutter;   // text column origin
    const char* who = pc_line ? "Grayson Burrows"
                    : cast_line ? cast_it->second.name.c_str()
                    : f->name.c_str();
    dl->AddText(ImVec2(tx, py + 14), pc_line ? kWhite : kAmber, who);
    dl->AddLine(ImVec2(tx, py + 36), ImVec2(px + pw - 18, py + 36), kDim);

    const bool has_dialogue = !dlg.empty();
    const bool last_para    = !has_dialogue || g_paragraph + 1 >= dlg.size();
    if (has_dialogue) {
        const std::string raw = dlg[std::min(g_paragraph, dlg.size() - 1)];
        const std::string text = is_beat(raw) ? std::string() : expand_tokens(raw);
        ImGui::SetCursorScreenPos(ImVec2(tx, py + 48));
        ImGui::PushTextWrapPos(px + pw - 18);
        ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    // Offer line, shown with the final paragraph. Its clip fires once on
    // arrival (tracked separately from paragraph advance, since the offer
    // shares the screen with the last paragraph rather than replacing it).
    // The offer only belongs to the MAIN phase -- once the player has decided,
    // the epilogue plays out without re-asking the question.
    const bool offering = g_phase == Phase::Main && last_para &&
                          !f->offer_text.empty();
    static std::string g_offer_voiced;   // fixer id whose offer clip has played
    if (offering && !f->voice_offer.empty() && g_offer_voiced != f->id) {
        g_offer_voiced = f->id;
        stop_voice();
        g_voice = audio::play_file(k_portrait_root + f->voice_offer, 1.0f);
    } else if (!offering && g_offer_voiced == f->id) {
        g_offer_voiced.clear();          // re-arm if they back out and return
    }
    if (offering) {
        const std::string text = g_decision_error.empty()
                               ? expand_tokens(f->offer_text)
                               : g_decision_error;
        ImGui::SetCursorScreenPos(ImVec2(tx, py + ph - 92));
        ImGui::PushTextWrapPos(px + pw - 18);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              g_decision_error.empty()
                                  ? ImGui::ColorConvertU32ToFloat4(kAmber)
                                  : ImVec4(1.0f, 0.38f, 0.28f, 1.0f));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    // ---- controls: NEXT / ACCEPT+REFUSE / LEAVE ---------------------------
    const float byy = py + ph - 44.0f;
    if (!last_para) {
        // Conversations play themselves. SKIP jumps the rest of the current
        // line for anyone who reads faster than the VO; PAUSE pins the
        // paragraph so a player can sit with it.
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 262, byy));
        if (ImGui::Button(g_auto_advance ? "PAUSE" : "RESUME", ImVec2(120, 30))) {
            sfx::ui_click();
            g_auto_advance = !g_auto_advance;
        }
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("SKIP >", ImVec2(112, 30))) {
            sfx::ui_click();
            ++g_paragraph;
            play_paragraph_voice(*f, g_paragraph);
        }
        // Progress pips: how far through the conversation we are.
        const float dot_y = byy + 14.0f;
        float dot_x = tx;
        for (size_t i = 0; i < dlg.size() && i < 24; ++i) {
            dl->AddCircleFilled(ImVec2(dot_x, dot_y), 3.0f,
                                i == g_paragraph ? kAmber : kDim);
            dot_x += 11.0f;
        }
    } else if (offering) {
        // Commit gameplay state immediately, then roll into the post-decision
        // exchange. Launching mid-epilogue must not leave an accepted offer
        // active while omitting its mission from the player's mission list.
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 262, byy));
        if (ImGui::Button("ACCEPT", ImVec2(120, 30))) {
            sfx::ui_click();
            if (run_decision_actions(*f, player, Phase::Accepted)) {
                g_decision_error.clear();
                if (!begin_phase(*f, Phase::Accepted)) reset_conversation();
            } else {
                g_decision_error =
                    "CAN'T ACCEPT: free enough cargo space for the consignment.";
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("REFUSE", ImVec2(112, 30))) {
            sfx::ui_click();
            run_decision_actions(*f, player, Phase::Refused);
            if (!begin_phase(*f, Phase::Refused)) reset_conversation();
        }
    } else if (g_phase != Phase::Main) {
        // End of a post-decision exchange. Run the actions exactly once, then
        // close. g_actions_ran guards against a double-fire if the player
        // clicks during the same frame the phase ends.
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("DONE", ImVec2(112, 30))) {
            sfx::ui_click();
            run_decision_actions(*f, player, g_phase);
            reset_conversation();
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("LEAVE", ImVec2(112, 30))) {
            sfx::ui_click();
            run_decision_actions(*f, player, g_phase);
            dialogue_done(*f, player);
            reset_conversation();
        }
    }
}

} // namespace

void talk_to(const FixerDef& f) {
    g_talking_to = f.id;
    g_paragraph  = 0;
    g_auto_advance = true;
    g_phase = Phase::Main;
    g_actions_ran = false;
    g_decision_error.clear();
    play_paragraph_voice(f, 0);
}

void register_bar_screen() {
    base_screens::register_screen(BaseScreen::Bar,
        [](BaseContext& ctx) { draw_fixer_body(ctx, "Bar"); });
    base_screens::register_screen(BaseScreen::Library,
        [](BaseContext& ctx) { draw_fixer_body(ctx, "Library"); });
    base_screens::register_screen(BaseScreen::ResearchComputer,
        [](BaseContext& ctx) { draw_fixer_body(ctx, "ResearchComputer"); });
    std::printf("[fixers] Bar + Library + Research Computer bodies registered\n");
}
#else
void talk_to(const FixerDef&) {}
void register_bar_screen() {}
#endif // FIXERS_HEADLESS

} // namespace fixers
