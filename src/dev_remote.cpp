// -----------------------------------------------------------------------------
// dev_remote.cpp — HTTP server + command queue for the dev remote.
//
// The HTTP server is deliberately tiny (~one-at-a-time connections, HTTP/1.0
// style, no keep-alive). This is a dev tool, not a production service.
// The code here avoids any third-party deps — it's just BSD sockets and
// std::thread. Platform-specific bits (window screenshot) live in
// dev_remote_macos.mm.
// -----------------------------------------------------------------------------

#include "dev_remote.h"
#include "camera.h"
#include "comm.h"
#include "faction.h"
#include "voice.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>   // std::clamp for the quat→euler arcsin guard
#include <atomic>
#include <cmath>        // std::asin / std::atan2 for telemetry conversion
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Declared in dev_remote_macos.mm — captures the game's NSWindow via
// screencapture -l<wid>. Returns true on success. Uses C linkage so
// we don't have to coordinate C++ mangling with the .mm TU.
extern "C" bool dev_remote_capture_window(const char* path);

namespace dev_remote {
namespace {

// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------
struct ScreenshotWaiter {
    std::string path;
    std::mutex mu;
    std::condition_variable cv;
    bool done    = false;
    bool success = false;
};

struct Command {
    enum class Kind { SetCamera, Screenshot, VoiceSay, CommBark, CargoGive, Spawn,
                      Kill, SetTarget, TractorPull, InventorySell, InventoryGive,
                      InventoryInstall, InventoryEquip, SetPanel, CommsSelect,
                      Rumor };
    Kind kind;

    // SetCamera — any optional field is encoded with `has_*`.
    bool has_pos = false, has_euler = false;
    HMM_Vec3 pos{};
    HMM_Vec3 euler{};

    // Screenshot — the waiter is owned by the HTTP thread; the main
    // thread only sets fields on it and notifies.
    ScreenshotWaiter* waiter = nullptr;

    // VoiceSay / CommBark / Spawn — parsed + validated on the HTTP thread,
    // the actual game call runs on the main thread in drain_commands
    // (game code never touches the HTTP thread).
    Faction         faction   = Faction::Count;
    voice::Category category  = voice::Category::Greeting;
    bool            to_player = true;

    // CargoGive — commodity id + unit count, applied via the cargo hook.
    // Spawn — ship class name + spawn distance, applied via the spawn hook
    // (faction reuses the field above).
    std::string     str_arg;          // commodity id (CargoGive) / class (Spawn)
    std::string     faction_name;     // raw faction string (Spawn)
    int             int_arg = 0;      // units (CargoGive)
    float           dist    = 0.0f;   // spawn distance (Spawn)
    uint32_t        kill_id = 0;      // target ship id (Kill; 0 = nearest)
    uint32_t        target_id = 0;    // target ship id (SetTarget; 0 = nearest)
    int             sell_index = 0;   // unified-hold item index (InventorySell/Install/Equip)
    int             mount_index = -1;  // gun mount index (InventoryEquip; <0 = first empty)
    // InventoryGive — item id reuses str_arg, qty reuses int_arg; the
    // kind/rarity strings ride on these dedicated fields.
    std::string     give_kind;        // "weapon"|"upgrade"|"salvage"|"commodity"
    std::string     give_rarity;      // "basic"|"rare"|"legendary"
};

std::mutex          g_queue_mu;
std::deque<Command> g_queue;

// Published state for /state responses. Plain atomics for the cheap
// scalars; a short mutex for the string.
std::atomic<int>    g_last_fps{0};
std::mutex          g_system_mu;
std::string         g_system_name;

// Shared between main thread (publishes camera) and HTTP thread (reads
// for /state). The main thread writes this in drain_commands so the
// state endpoint always reports the *current* camera even when no new
// commands have been issued.
std::mutex g_cam_snapshot_mu;
HMM_Vec3   g_cam_pos{};
HMM_Vec3   g_cam_euler{};

// Render-matrix snapshot for /project. Published once per frame by the
// main thread (publish_render_matrices); read by the HTTP thread inside
// handle_project. Guarded by its own mutex so a /project request never
// races a mid-frame matrix write.
std::mutex g_proj_mu;
HMM_Mat4   g_view_proj{};
HMM_Mat4   g_model{};
HMM_Vec3   g_proj_cam_pos{};
bool       g_proj_ready = false;   // false until first publish

// Latest near-player ship snapshot for GET /ships. Published once per
// Flight frame by the main thread (publish_ships); read by the HTTP thread
// in handle_ships. Guarded so a serialise never races a mid-frame rebuild.
std::mutex             g_ships_mu;
std::vector<ShipInfo>  g_ships;

// Latest near-player loot snapshot for GET /loot, published once per Flight
// frame by the main thread (publish_loot). Guarded so a serialise never
// races a mid-frame rebuild.
std::mutex             g_loot_mu;
std::vector<LootInfo>  g_loot;

// Latest leads snapshot for GET /objectives, published once per Flight
// frame by the main thread (publish_objectives). Guarded so a serialise
// never races a mid-frame rebuild.
std::mutex             g_objectives_mu;
std::vector<LeadInfo>  g_objectives;

// Latest inventory snapshot for GET /inventory, published once per Flight
// frame by the main thread (publish_inventory). The cargo usage/cap pair
// rides under the same lock so the three values stay consistent.
std::mutex             g_inventory_mu;
std::vector<ItemInfo>  g_inventory;
std::vector<ModInfo>   g_permanent_mods;   // installed permanent_mods (#92/#93)
std::vector<MountInfo> g_mounts;           // fitted gun mounts (#98)
int                    g_cargo_used = 0;
int                    g_cargo_cap  = 0;

// Latest player snapshot for GET /player, published once per frame (ALL
// modes) by the main thread (publish_player). Guarded so a serialise never
// races a mid-frame rebuild.
std::mutex  g_player_mu;
PlayerInfo  g_player;

// Latest accepted-missions snapshot for GET /missions, published once per
// frame (ALL modes) by the main thread (publish_missions).
std::mutex               g_missions_mu;
std::vector<MissionInfo> g_missions;

// Gameplay event ring buffer for GET /events. push_event may be called from
// any main-thread system (in practice: the comm feed tap + mode-transition
// detection in main.cpp); the HTTP thread reads under the same lock. Ring
// capped at k_max_events — a polling judge passing since=<last latest>
// only loses data if it sleeps through 512 events.
struct Event {
    uint64_t    seq;
    int64_t     t;          // unix seconds
    std::string category;   // "comm", "mode", ...
    std::string text;
};
constexpr size_t   k_max_events = 512;
std::mutex         g_events_mu;
std::deque<Event>  g_events;
uint64_t           g_event_seq = 0;

// Registered host hooks (the decoupling seam). Set once at startup by the
// host via set_*_hook; invoked only on the main thread inside
// drain_commands. Guarded for the (unlikely) case of a late registration
// racing the HTTP thread — though in practice both are touched only after
// start(). The HTTP thread never calls these; it only enqueues commands.
std::mutex                                                  g_hooks_mu;
std::function<void(std::string, int)>                       g_cargo_give_hook;
std::function<void(std::string, std::string, float)>        g_spawn_hook;
std::function<void(uint32_t)>                               g_kill_hook;
std::function<void(uint32_t)>                               g_set_target_hook;
std::function<void()>                                       g_tractor_pull_hook;
std::function<void()>                                       g_rumor_hook;
std::function<void(int)>                                    g_inventory_sell_hook;
std::function<void(std::string, std::string, std::string, int)> g_inventory_give_hook;
std::function<void(int)>                                    g_inventory_install_hook;
std::function<void(int, int)>                               g_inventory_equip_hook;
std::function<void(std::string)>                            g_panel_hook;
std::function<void(int)>                                    g_comms_select_hook;

std::thread       g_thread;
std::atomic<bool> g_running{false};
int               g_listen_fd = -1;

// The currently-pending screenshot request. Written only from the main
// thread in drain_commands(); read/cleared only from the main thread in
// maybe_capture_screenshot(). Single-threaded access → no lock needed.
ScreenshotWaiter* g_pending_shot = nullptr;

// ---------------------------------------------------------------------------
// Tiny JSON helpers — just enough for our 3 endpoints. We don't need a
// real parser; requests are always flat objects with known keys and
// numeric or string values. Extracting with string scans keeps the code
// dependency-free.
// ---------------------------------------------------------------------------
bool extract_float(const std::string& body, const char* key, float* out) {
    std::string needle = std::string("\"") + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    *out = std::strtof(body.c_str() + pos + 1, nullptr);
    return true;
}

// Extract a string value: "key":"value". Returns false if the key is
// absent or not a quoted string. No escape handling — our dev payloads
// are flat ASCII faction/category names.
bool extract_string(const std::string& body, const char* key, std::string* out) {
    std::string needle = std::string("\"") + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    auto open = body.find('"', pos);
    if (open == std::string::npos) return false;
    auto close = body.find('"', open + 1);
    if (close == std::string::npos) return false;
    *out = body.substr(open + 1, close - open - 1);
    return true;
}

// Extract a JSON bool: "key":true / "key":false. Returns false if the
// key is absent (caller keeps its default); on presence, *out is set.
bool extract_bool(const std::string& body, const char* key, bool* out) {
    std::string needle = std::string("\"") + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    auto t = body.find("true", pos);
    auto f = body.find("false", pos);
    // Whichever literal appears first after the colon wins.
    if (t != std::string::npos && (f == std::string::npos || t < f)) { *out = true;  return true; }
    if (f != std::string::npos) { *out = false; return true; }
    return false;
}

// Map a category name to voice::Category. Returns false on an unknown
// string so the handler can answer with a clean JSON error.
bool category_from_name(const std::string& s, voice::Category* out) {
    if      (s == "greeting") *out = voice::Category::Greeting;
    else if (s == "hostile")  *out = voice::Category::Hostile;
    else if (s == "low_hp")   *out = voice::Category::LowHp;
    else if (s == "kill")     *out = voice::Category::Kill;
    else if (s == "demand")   *out = voice::Category::Demand;
    else if (s == "rumor")    *out = voice::Category::Rumor;
    else if (s == "search")   *out = voice::Category::Search;
    else if (s == "clear")    *out = voice::Category::Clear;
    else return false;
    return true;
}

// Scan a string for every floating-point token and return them in order.
// Used by /project: the client sends a flat array of numbers and we just
// read them all sequentially (6 per point: x y z nx ny nz). Brackets,
// commas, and whitespace are skipped by strtof's own tokenising — we
// step the cursor past each parsed number and resume scanning for the
// next numeric character.
std::vector<float> extract_all_floats(const std::string& body) {
    std::vector<float> out;
    const char* p   = body.c_str();
    const char* end = p + body.size();
    while (p < end) {
        // Advance to the next character that could start a number.
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {
            char* next = nullptr;
            float v = std::strtof(p, &next);
            if (next == p) { ++p; continue; }   // not actually a number
            out.push_back(v);
            p = next;
        } else {
            ++p;
        }
    }
    return out;
}

std::string json_state() {
    HMM_Vec3 pos, euler;
    {
        std::lock_guard lk(g_cam_snapshot_mu);
        pos   = g_cam_pos;
        euler = g_cam_euler;
    }
    std::string sys;
    {
        std::lock_guard lk(g_system_mu);
        sys = g_system_name;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"pos\":[%.2f,%.2f,%.2f],"
        "\"euler\":[%.2f,%.2f,%.2f],"
        "\"fps\":%d,"
        "\"system\":\"%s\"}",
        pos.X, pos.Y, pos.Z,
        euler.X, euler.Y, euler.Z,
        g_last_fps.load(),
        sys.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
void send_all(int fd, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, data + sent, n - sent, 0);
        if (r <= 0) return;
        sent += (size_t)r;
    }
}

void send_response(int fd, int status, const char* status_text,
                   const char* content_type, const std::string& body) {
    char hdr[512];
    int n = std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body.size());
    send_all(fd, hdr, (size_t)n);
    if (!body.empty()) send_all(fd, body.data(), body.size());
}

void send_json(int fd, const std::string& body) {
    send_response(fd, 200, "OK", "application/json", body);
}

void send_404(int fd) {
    send_response(fd, 404, "Not Found", "text/plain",
                  "unknown endpoint\n");
}

// ---------------------------------------------------------------------------
// Endpoint handlers
// ---------------------------------------------------------------------------
void handle_state(int fd) {
    send_json(fd, json_state());
}

void handle_camera_set(int fd, const std::string& body) {
    Command c;
    c.kind = Command::Kind::SetCamera;

    float x, y, z;
    const bool has_x = extract_float(body, "x", &x);
    const bool has_y = extract_float(body, "y", &y);
    const bool has_z = extract_float(body, "z", &z);
    if (has_x && has_y && has_z) {
        c.has_pos = true;
        c.pos = { x, y, z };
    }
    float yaw, pitch, roll;
    // Degrees. Accepts any subset — missing fields preserve current
    // orientation when drained by the main thread.
    bool has_any_euler = false;
    HMM_Vec3 eul{};
    if (extract_float(body, "pitch", &pitch)) { eul.X = pitch; has_any_euler = true; }
    if (extract_float(body, "yaw",   &yaw))   { eul.Y = yaw;   has_any_euler = true; }
    if (extract_float(body, "roll",  &roll))  { eul.Z = roll;  has_any_euler = true; }
    if (has_any_euler) {
        c.has_euler = true;
        c.euler = eul;
    }

    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /voice/say — enqueue a voiced comm line. Body:
//   {"faction":"pirate","category":"hostile","to_player":true}
// to_player is optional (default true). Parse + validate here on the
// HTTP thread; the actual voice::say runs on the main thread.
void handle_voice_say(int fd, const std::string& body) {
    std::string fac_s, cat_s;
    if (!extract_string(body, "faction", &fac_s)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing faction\"}");
        return;
    }
    if (!extract_string(body, "category", &cat_s)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing category\"}");
        return;
    }
    const Faction fac = faction::from_name(fac_s);
    if (fac == Faction::Count) {
        send_json(fd, "{\"ok\":false,\"error\":\"unknown faction\"}");
        return;
    }
    voice::Category cat;
    if (!category_from_name(cat_s, &cat)) {
        send_json(fd, "{\"ok\":false,\"error\":\"unknown category\"}");
        return;
    }

    Command c;
    c.kind      = Command::Kind::VoiceSay;
    c.faction   = fac;
    c.category  = cat;
    c.to_player = true;
    extract_bool(body, "to_player", &c.to_player);   // optional override
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /comm/bark — enqueue a hostile comm bark. Body: {"faction":"confed"}.
void handle_comm_bark(int fd, const std::string& body) {
    std::string fac_s;
    if (!extract_string(body, "faction", &fac_s)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing faction\"}");
        return;
    }
    const Faction fac = faction::from_name(fac_s);
    if (fac == Faction::Count) {
        send_json(fd, "{\"ok\":false,\"error\":\"unknown faction\"}");
        return;
    }

    Command c;
    c.kind    = Command::Kind::CommBark;
    c.faction = fac;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /panel — set the STATUS panel sub-screen. Body:
//   {"screen":"comms|ship|damage|weapons"}
// Validated here (HTTP thread); the host's panel hook flips
// cockpit_hud::set_status_screen on the main thread.
void handle_panel(int fd, const std::string& body) {
    std::string screen;
    if (!extract_string(body, "screen", &screen)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing screen\"}");
        return;
    }
    if (screen != "comms" && screen != "ship" &&
        screen != "damage" && screen != "weapons") {
        send_json(fd, "{\"ok\":false,\"error\":\"unknown screen\"}");
        return;
    }
    Command c;
    c.kind    = Command::Kind::SetPanel;
    c.str_arg = screen;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /comms/select — pick a numbered entry in the Comms menu. Body:
//   {"n":N}
// The host hook ensures the Comms screen is active + opened, then routes to
// comms_menu::select(N) on the main thread.
void handle_comms_select(int fd, const std::string& body) {
    float n_f = 0.0f;
    if (!extract_float(body, "n", &n_f)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing n\"}");
        return;
    }
    Command c;
    c.kind    = Command::Kind::CommsSelect;
    c.int_arg = (int)n_f;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// Escape a string for embedding in a JSON literal. The snapshot strings
// (faction names, class ids) are safe by construction, but comm feed lines
// and mission titles are free text — quotes/backslashes/control chars must
// not corrupt the reply a judge is parsing.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// GET /player — serialise the latest player snapshot. Pure read of the
// published struct; the host publishes it every frame in every mode.
void handle_player(int fd) {
    PlayerInfo p;
    {
        std::lock_guard lk(g_player_mu);
        p = g_player;
    }
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"credits\":%lld,\"ship_class\":\"%s\",\"system\":\"%s\","
        "\"docked_base\":\"%s\",\"mode\":\"%s\","
        "\"merc_guild\":%s,\"merchant_guild\":%s,\"factions\":[",
        (long long)p.credits,
        json_escape(p.ship_class).c_str(),
        json_escape(p.system).c_str(),
        json_escape(p.docked_base).c_str(),
        json_escape(p.mode).c_str(),
        p.merc_guild ? "true" : "false",
        p.merchant_guild ? "true" : "false");
    out += buf;
    for (size_t i = 0; i < p.factions.size(); ++i) {
        const FactionStanding& f = p.factions[i];
        std::snprintf(buf, sizeof(buf),
            "%s{\"faction\":\"%s\",\"rep\":%d,\"kills\":%lld}",
            i ? "," : "", json_escape(f.faction).c_str(), f.rep,
            (long long)f.kills);
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

// GET /missions — serialise the latest accepted-missions snapshot. The
// status strings were pre-formatted by the host with the same
// missions::mission_status the HUD uses — one truth, two readers.
void handle_missions(int fd) {
    std::vector<MissionInfo> ms;
    {
        std::lock_guard lk(g_missions_mu);
        ms = g_missions;
    }
    std::string out = "{\"missions\":[";
    for (size_t i = 0; i < ms.size(); ++i) {
        const MissionInfo& m = ms[i];
        char buf[768];
        std::snprintf(buf, sizeof(buf),
            "%s{\"id\":\"%s\",\"type\":\"%s\",\"source\":\"%s\","
            "\"status\":\"%s\",\"reward\":%lld,\"target_system\":\"%s\","
            "\"in_current_system\":%s}",
            i ? "," : "",
            json_escape(m.id).c_str(),
            json_escape(m.type).c_str(),
            json_escape(m.source).c_str(),
            json_escape(m.status).c_str(),
            (long long)m.reward,
            json_escape(m.target_system).c_str(),
            m.in_current_system ? "true" : "false");
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

// GET /events?since=N — serialise ring-buffer events with seq > N (all of
// them when `since` is absent/0). `latest` echoes the newest seq so the
// caller can poll incrementally: next request passes since=<latest>.
void handle_events(int fd, const std::string& query) {
    uint64_t since = 0;
    if (auto p = query.find("since="); p != std::string::npos) {
        since = (uint64_t)std::strtoull(query.c_str() + p + 6, nullptr, 10);
    }
    std::string out = "{\"events\":[";
    uint64_t latest = 0;
    {
        std::lock_guard lk(g_events_mu);
        latest = g_event_seq;
        bool first = true;
        for (const Event& e : g_events) {
            if (e.seq <= since) continue;
            char buf[640];
            std::snprintf(buf, sizeof(buf),
                "%s{\"seq\":%llu,\"t\":%lld,\"category\":\"%s\","
                "\"text\":\"%s\"}",
                first ? "" : ",", (unsigned long long)e.seq, (long long)e.t,
                json_escape(e.category).c_str(), json_escape(e.text).c_str());
            out += buf;
            first = false;
        }
    }
    char tail[64];
    std::snprintf(tail, sizeof(tail), "],\"latest\":%llu}",
                  (unsigned long long)latest);
    out += tail;
    send_json(fd, out);
}

// GET /ships — serialise the latest near-player ship snapshot. Pure read
// of the published vector; no command queued, no main-thread round trip,
// because the host publishes the snapshot every Flight frame.
void handle_ships(int fd) {
    std::vector<ShipInfo> ships;
    {
        std::lock_guard lk(g_ships_mu);
        ships = g_ships;
    }
    std::string out = "{\"ships\":[";
    for (size_t i = 0; i < ships.size(); ++i) {
        const ShipInfo& s = ships[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s{\"id\":%u,\"faction\":\"%s\",\"dist\":%.1f,"
            "\"alive\":%s,\"aggro_player\":%s,\"provoked\":%s}",
            i ? "," : "", s.id, s.faction.c_str(), s.dist,
            s.alive        ? "true" : "false",
            s.aggro_player ? "true" : "false",
            s.provoked     ? "true" : "false");
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

// POST /cargo/give — add cargo to the player. Body:
//   {"commodity":"tungsten","units":10}
// Validate here on the HTTP thread; the actual player::add_cargo runs on
// the main thread via the registered hook.
void handle_cargo_give(int fd, const std::string& body) {
    std::string commodity;
    if (!extract_string(body, "commodity", &commodity)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing commodity\"}");
        return;
    }
    float units_f = 0.0f;
    if (!extract_float(body, "units", &units_f)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing units\"}");
        return;
    }
    const int units = (int)units_f;
    if (units <= 0) {
        send_json(fd, "{\"ok\":false,\"error\":\"units must be positive\"}");
        return;
    }

    Command c;
    c.kind    = Command::Kind::CargoGive;
    c.str_arg = commodity;
    c.int_arg = units;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /spawn — spawn an NPC near the player. Body:
//   {"faction":"militia","class":"talon","dist":2500}
// dist is optional (0 → host default). Faction is validated here so a bad
// name 400s cleanly before we queue anything; the host's spawn hook runs
// the real recipe on the main thread.
void handle_spawn(int fd, const std::string& body) {
    std::string fac_s, klass;
    if (!extract_string(body, "faction", &fac_s)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing faction\"}");
        return;
    }
    if (!extract_string(body, "class", &klass)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing class\"}");
        return;
    }
    if (faction::from_name(fac_s) == Faction::Count) {
        send_json(fd, "{\"ok\":false,\"error\":\"unknown faction\"}");
        return;
    }
    float dist = 0.0f;
    extract_float(body, "dist", &dist);   // optional; 0 → host default

    Command c;
    c.kind         = Command::Kind::Spawn;
    c.faction_name = fac_s;
    c.str_arg      = klass;
    c.dist         = dist;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// GET /loot — serialise the latest near-player loot snapshot. Pure read of
// the published vector; no command queued, no main-thread round trip,
// because the host publishes the snapshot every Flight frame.
void handle_loot(int fd) {
    std::vector<LootInfo> loot;
    {
        std::lock_guard lk(g_loot_mu);
        loot = g_loot;
    }
    std::string out = "{\"loot\":[";
    for (size_t i = 0; i < loot.size(); ++i) {
        const LootInfo& l = loot[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s{\"dist\":%.1f,\"id\":\"%s\",\"kind\":%d,\"rarity\":%d}",
            i ? "," : "", l.dist, l.id.c_str(), l.kind, l.rarity);
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

// GET /inventory — serialise the player's unified-hold items + cargo usage.
// Pure read of the published snapshot; the host rebuilds it each Flight
// frame from g.player.items + cargo_units_used + cargo_capacity.
void handle_inventory(int fd) {
    std::vector<ItemInfo>  items;
    std::vector<ModInfo>   mods;
    std::vector<MountInfo> mounts;
    int used, cap;
    {
        std::lock_guard lk(g_inventory_mu);
        items  = g_inventory;
        mods   = g_permanent_mods;
        mounts = g_mounts;
        used   = g_cargo_used;
        cap    = g_cargo_cap;
    }
    std::string out = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        const ItemInfo& it = items[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s{\"id\":\"%s\",\"kind\":%d,\"rarity\":%d,\"qty\":%d}",
            i ? "," : "", it.id.c_str(), it.kind, it.rarity, it.qty);
        out += buf;
    }
    out += "],\"permanent_mods\":[";
    for (size_t i = 0; i < mods.size(); ++i) {
        const ModInfo& m = mods[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s{\"id\":\"%s\",\"effect\":\"%s\",\"value\":%g}",
            i ? "," : "", m.id.c_str(), m.effect.c_str(), m.value);
        out += buf;
    }
    out += "],\"mounts\":[";
    for (size_t i = 0; i < mounts.size(); ++i) {
        const MountInfo& mt = mounts[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s{\"gun_id\":\"%s\",\"rarity\":%d}",
            i ? "," : "", mt.gun_id.c_str(), mt.rarity);
        out += buf;
    }
    char tail[64];
    std::snprintf(tail, sizeof(tail), "],\"cargo_used\":%d,\"cargo_cap\":%d}",
                  used, cap);
    out += tail;
    send_json(fd, out);
}

// POST /kill — kill a ship. Body {"id":N} is optional; a missing/zero id
// means "nearest alive non-player ship to the player". Validation is
// trivial (any uint), so we enqueue and reply optimistically with the
// requested id; the registered kill hook resolves the actual target on the
// main thread (see the cargo/give + spawn fire-and-forget precedent).
void handle_kill(int fd, const std::string& body) {
    float id_f = 0.0f;
    extract_float(body, "id", &id_f);   // optional; 0 → nearest
    const uint32_t id = (uint32_t)(id_f < 0.0f ? 0.0f : id_f);

    Command c;
    c.kind    = Command::Kind::Kill;
    c.kill_id = id;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"killed\":%u}", id);
    send_json(fd, buf);
}

// POST /target — set the player's current target. Body {"id":N} is optional;
// a missing/zero id means "nearest alive non-player ship to the player". Like
// /kill, validation is trivial (any uint), so we enqueue and reply
// optimistically with the requested id; the registered target hook resolves
// the actual target (and writes g.player_target_id) on the main thread.
void handle_target(int fd, const std::string& body) {
    float id_f = 0.0f;
    extract_float(body, "id", &id_f);   // optional; 0 → nearest
    const uint32_t id = (uint32_t)(id_f < 0.0f ? 0.0f : id_f);

    Command c;
    c.kind      = Command::Kind::SetTarget;
    c.target_id = id;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"target\":%u}", id);
    send_json(fd, buf);
}

// POST /tractor/pull — pull every in-range loot drop into the player's
// hold. No body. Enqueued and run on the main thread by the registered
// tractor hook (loot::try_pull).
void handle_tractor_pull(int fd) {
    Command c;
    c.kind = Command::Kind::TractorPull;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// GET /objectives — serialise the latest leads snapshot. Pure read of the
// published vector; the host publishes it every Flight frame from
// objectives::all().
void handle_objectives(int fd) {
    std::vector<LeadInfo> objs;
    {
        std::lock_guard lk(g_objectives_mu);
        objs = g_objectives;
    }
    std::string out = "{\"objectives\":[";
    for (size_t i = 0; i < objs.size(); ++i) {
        const LeadInfo& o = objs[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s{\"label\":\"%s\",\"dist\":%.1f}",
            i ? "," : "", o.label.c_str(), o.dist);
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

// POST /rumor — register a dynamic-objective lead in the current system.
// No body. Enqueued and run on the main thread by the registered rumor
// hook (objectives::add_lead + pick_lead_pos).
void handle_rumor(int fd) {
    Command c;
    c.kind = Command::Kind::Rumor;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /inventory/sell — sell the unified-hold item at {"index":N}. Index
// defaults to 0 if missing. Enqueued and run on the main thread by the
// registered hook (inventory::sell_item), which bounds-checks for us.
void handle_inventory_sell(int fd, const std::string& body) {
    float idx_f = 0.0f;
    extract_float(body, "index", &idx_f);   // optional; default 0

    Command c;
    c.kind       = Command::Kind::InventorySell;
    c.sell_index = (int)idx_f;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /inventory/give-item — add an item to the player's hold. Body:
//   {"id":"shield_matrix","kind":"upgrade","rarity":"basic","qty":1}
// kind/rarity default to "salvage"/"basic"; qty defaults to 1. The id is
// required. Enqueued and run on the main thread by the registered hook,
// which builds an InventoryItem + player::add_item.
void handle_inventory_give(int fd, const std::string& body) {
    std::string id;
    if (!extract_string(body, "id", &id)) {
        send_json(fd, "{\"ok\":false,\"error\":\"missing id\"}");
        return;
    }
    std::string kind = "salvage", rarity = "basic";
    extract_string(body, "kind",   &kind);     // optional
    extract_string(body, "rarity", &rarity);   // optional
    float qty_f = 1.0f;
    extract_float(body, "qty", &qty_f);        // optional; default 1
    const int qty = (qty_f >= 1.0f) ? (int)qty_f : 1;

    Command c;
    c.kind        = Command::Kind::InventoryGive;
    c.str_arg     = id;
    c.give_kind   = kind;
    c.give_rarity = rarity;
    c.int_arg     = qty;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /inventory/install — install the Upgrade-kind item at {"index":N}
// into permanent_mods. Index defaults to 0. Enqueued and run on the main
// thread by the registered hook (inventory::install_upgrade), which
// bounds-checks + refuses non-Upgrade items and duplicates.
void handle_inventory_install(int fd, const std::string& body) {
    float idx_f = 0.0f;
    extract_float(body, "index", &idx_f);   // optional; default 0

    Command c;
    c.kind       = Command::Kind::InventoryInstall;
    c.sell_index = (int)idx_f;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// POST /inventory/equip — equip the Weapon-kind item at {"item_index":N}
// into gun mount {"mount_index":N}. item_index defaults to 0; mount_index
// is OPTIONAL and defaults to -1, which the host resolves to the first
// empty mount (else 0). Enqueued + run on the main thread by the
// registered hook (inventory::equip_weapon), which bounds-checks for us.
void handle_inventory_equip(int fd, const std::string& body) {
    float item_f = 0.0f, mount_f = -1.0f;
    extract_float(body, "item_index",  &item_f);   // optional; default 0
    extract_float(body, "mount_index", &mount_f);  // optional; default -1

    Command c;
    c.kind        = Command::Kind::InventoryEquip;
    c.sell_index  = (int)item_f;
    c.mount_index = (int)mount_f;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// Project mesh-local 3D points into screen UV using the published render
// matrices. Body is a flat float array [x,y,z,nx,ny,nz, ...]; we read 6
// floats per point (position + outward normal). Pure read of the snapshot
// — no command queued, no main-thread round trip needed, because the
// matrices are already published each frame.
void handle_project(int fd, const std::string& body) {
    HMM_Mat4 vp, model;
    HMM_Vec3 cam;
    bool ready;
    {
        std::lock_guard lk(g_proj_mu);
        vp = g_view_proj; model = g_model; cam = g_proj_cam_pos;
        ready = g_proj_ready;
    }
    if (!ready) {
        send_json(fd, "{\"results\":[],\"error\":\"no render matrices yet\"}");
        return;
    }

    const std::vector<float> nums = extract_all_floats(body);
    std::string out = "{\"results\":[";
    const size_t n_pts = nums.size() / 6;
    for (size_t i = 0; i < n_pts; ++i) {
        const float px = nums[i*6+0], py = nums[i*6+1], pz = nums[i*6+2];
        const float nx = nums[i*6+3], ny = nums[i*6+4], nz = nums[i*6+5];

        // Local → world → clip. w=1 for the position (affine transform).
        const HMM_Vec4 world = HMM_MulM4V4(model, HMM_V4(px, py, pz, 1.0f));
        const HMM_Vec4 clip  = HMM_MulM4V4(vp, world);
        const bool front = clip.W > 1e-4f;
        float u = 0.0f, v = 0.0f;
        if (front) {
            const float ndc_x = clip.X / clip.W;
            const float ndc_y = clip.Y / clip.W;
            u = ndc_x * 0.5f + 0.5f;
            v = 1.0f - (ndc_y * 0.5f + 0.5f);   // image row 0 = top = +ndc.y
        }

        // Normal-facing visibility: transform the outward normal by the
        // model matrix (w=0 → rotation+scale only) and check it points
        // toward the camera. Uniform scale keeps the direction valid.
        const HMM_Vec4 nworld = HMM_MulM4V4(model, HMM_V4(nx, ny, nz, 0.0f));
        const HMM_Vec3 to_cam = HMM_SubV3(cam, HMM_V3(world.X, world.Y, world.Z));
        const float dot = nworld.X*to_cam.X + nworld.Y*to_cam.Y + nworld.Z*to_cam.Z;
        const bool facing = dot > 0.0f;

        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "%s{\"u\":%.5f,\"v\":%.5f,\"front\":%s,\"facing\":%s}",
            i ? "," : "", u, v,
            front  ? "true" : "false",
            facing ? "true" : "false");
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

void handle_screenshot(int fd) {
    ScreenshotWaiter w;
    w.path = "/tmp/np_shot.png";

    Command c;
    c.kind   = Command::Kind::Screenshot;
    c.waiter = &w;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }

    // Wait up to ~2 seconds for the main thread to take the shot.
    // Timeout means something's wrong with the render loop; we return
    // an error response rather than block the HTTP thread forever.
    std::unique_lock lk(w.mu);
    w.cv.wait_for(lk, std::chrono::seconds(2), [&]{ return w.done; });

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"path\":\"%s\"}",
        w.success ? "true" : "false",
        w.path.c_str());
    send_json(fd, buf);
}

// ---------------------------------------------------------------------------
// Request parsing and dispatch
// ---------------------------------------------------------------------------
// Read until CRLFCRLF. Returns true on success. For simplicity we cap
// the request at 64 KB — way beyond anything a dev tool should send.
bool read_request(int fd, std::string& out) {
    out.clear();
    char buf[4096];
    while (out.size() < 65536) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) return !out.empty();
        out.append(buf, buf + r);
        if (out.find("\r\n\r\n") != std::string::npos) return true;
    }
    return false;
}

void handle_connection(int fd) {
    std::string req;
    if (!read_request(fd, req)) { ::close(fd); return; }

    // Parse first line: "METHOD PATH HTTP/1.x"
    auto first_crlf = req.find("\r\n");
    std::string line = req.substr(0, first_crlf);
    std::string method, path;
    {
        auto sp1 = line.find(' ');
        auto sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            send_response(fd, 400, "Bad Request", "text/plain", "bad line\n");
            ::close(fd);
            return;
        }
        method = line.substr(0, sp1);
        path   = line.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    // Split "?query" off the path so parameterised GETs (/events?since=N)
    // route on the bare path. Query stays raw — handlers parse their own.
    std::string query;
    if (auto q = path.find('?'); q != std::string::npos) {
        query = path.substr(q + 1);
        path.resize(q);
    }

    // Extract body (after blank line).
    auto body_sep = req.find("\r\n\r\n");
    std::string body = (body_sep == std::string::npos)
                     ? std::string{}
                     : req.substr(body_sep + 4);

    // Read the Content-Length bytes if we don't already have them all.
    // This matters for POSTs that arrive in >1 TCP segment.
    size_t content_length = 0;
    if (auto p = req.find("Content-Length:"); p != std::string::npos) {
        content_length = (size_t)std::strtoul(req.c_str() + p + 15, nullptr, 10);
    }
    while (body.size() < content_length) {
        char buf[4096];
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        body.append(buf, buf + r);
    }

    if      (method == "GET"  && path == "/state")      handle_state(fd);
    else if (method == "POST" && path == "/camera/set") handle_camera_set(fd, body);
    else if (method == "POST" && path == "/screenshot") handle_screenshot(fd);
    else if (method == "POST" && path == "/project")    handle_project(fd, body);
    else if (method == "POST" && path == "/voice/say")  handle_voice_say(fd, body);
    else if (method == "POST" && path == "/comm/bark")  handle_comm_bark(fd, body);
    else if (method == "POST" && path == "/panel")       handle_panel(fd, body);
    else if (method == "POST" && path == "/comms/select") handle_comms_select(fd, body);
    else if (method == "GET"  && path == "/ships")      handle_ships(fd);
    else if (method == "POST" && path == "/cargo/give") handle_cargo_give(fd, body);
    else if (method == "POST" && path == "/spawn")      handle_spawn(fd, body);
    else if (method == "GET"  && path == "/loot")       handle_loot(fd);
    else if (method == "GET"  && path == "/objectives") handle_objectives(fd);
    else if (method == "POST" && path == "/rumor")      handle_rumor(fd);
    else if (method == "GET"  && path == "/inventory")  handle_inventory(fd);
    else if (method == "POST" && path == "/kill")       handle_kill(fd, body);
    else if (method == "POST" && path == "/target")     handle_target(fd, body);
    else if (method == "POST" && path == "/tractor/pull") handle_tractor_pull(fd);
    else if (method == "POST" && path == "/inventory/sell") handle_inventory_sell(fd, body);
    else if (method == "POST" && path == "/inventory/give-item") handle_inventory_give(fd, body);
    else if (method == "POST" && path == "/inventory/install") handle_inventory_install(fd, body);
    else if (method == "POST" && path == "/inventory/equip") handle_inventory_equip(fd, body);
    else if (method == "GET"  && path == "/player")     handle_player(fd);
    else if (method == "GET"  && path == "/missions")   handle_missions(fd);
    else if (method == "GET"  && path == "/events")     handle_events(fd, query);
    else                                                send_404(fd);

    ::close(fd);
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------
void server_loop(int port) {
    g_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        std::fprintf(stderr, "[dev_remote] socket() failed\n");
        return;
    }
    int yes = 1;
    ::setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only
    if (::bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "[dev_remote] bind(%d) failed — port in use?\n", port);
        ::close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }
    if (::listen(g_listen_fd, 4) < 0) {
        std::fprintf(stderr, "[dev_remote] listen() failed\n");
        ::close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }
    std::printf("[dev_remote] listening on 127.0.0.1:%d\n", port);

    while (g_running.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int fd = ::accept(g_listen_fd, (sockaddr*)&peer, &peer_len);
        if (fd < 0) {
            if (g_running.load()) continue;  // shutdown raced us
            break;
        }
        handle_connection(fd);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void start(int port) {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(server_loop, port);
}

void stop() {
    if (!g_running.exchange(false)) return;
    if (g_listen_fd >= 0) {
        ::shutdown(g_listen_fd, SHUT_RDWR);
        ::close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_thread.joinable()) g_thread.join();
}

void publish_fps(int fps) {
    g_last_fps.store(fps);
}

void drain_commands(Camera& cam) {
    std::deque<Command> local;
    {
        std::lock_guard lk(g_queue_mu);
        local.swap(g_queue);
    }

    for (auto& c : local) {
        switch (c.kind) {
        case Command::Kind::SetCamera:
            if (c.has_pos) {
                cam.position = c.pos;
                cam.velocity = { 0, 0, 0 };   // teleport → zero inertia
            }
            if (c.has_euler) {
                // API speaks Euler angles (degrees) for human convenience;
                // Camera now stores a quaternion. Build the equivalent
                // orientation as q = qyaw * qpitch (matches the old
                // ry(yaw) * rx(pitch) matrix order). Roll is folded in
                // for full coverage even though most callers send 0.
                const float yaw_r   = c.euler.Y * HMM_DegToRad;
                const float pitch_r = c.euler.X * HMM_DegToRad;
                const float roll_r  = c.euler.Z * HMM_DegToRad;
                const HMM_Quat qy = HMM_QFromAxisAngle_RH(HMM_V3(0,1,0), yaw_r);
                const HMM_Quat qp = HMM_QFromAxisAngle_RH(HMM_V3(1,0,0), pitch_r);
                const HMM_Quat qr = HMM_QFromAxisAngle_RH(HMM_V3(0,0,1), roll_r);
                cam.orientation = HMM_NormQ(
                    HMM_MulQ(HMM_MulQ(qy, qp), qr));
            }
            break;
        case Command::Kind::Screenshot:
            // Stash for end-of-frame hook. Only one in flight at a time;
            // if two come back-to-back the earlier one is signalled as
            // failed so its HTTP thread unblocks cleanly.
            if (g_pending_shot) {
                std::lock_guard lk(g_pending_shot->mu);
                g_pending_shot->done    = true;
                g_pending_shot->success = false;
                g_pending_shot->cv.notify_all();
            }
            g_pending_shot = c.waiter;
            break;
        case Command::Kind::VoiceSay:
            // Voiced comm line at the origin; 2D radio vs 3D ambient is
            // decided inside voice::say by to_player.
            voice::say(c.faction, c.category, HMM_Vec3{0, 0, 0}, c.to_player);
            break;
        case Command::Kind::CommBark:
            // Dev barks are always treated as aimed at the player so they
            // surface on the HUD comm feed.
            comm::npc_engage_bark(c.faction, /*target_is_player=*/true,
                                  /*speaker_id=*/0);
            break;
        case Command::Kind::CargoGive: {
            // Run the host's registered cargo hook on the main thread.
            std::function<void(std::string, int)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_cargo_give_hook;
            }
            if (hook) hook(c.str_arg, c.int_arg);
            break;
        }
        case Command::Kind::Spawn: {
            // Run the host's registered spawn hook on the main thread,
            // reusing whatever debug-spawn recipe the host wired in.
            std::function<void(std::string, std::string, float)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_spawn_hook;
            }
            if (hook) hook(c.faction_name, c.str_arg, c.dist);
            break;
        }
        case Command::Kind::Kill: {
            // Run the host's registered kill hook on the main thread; it
            // resolves the target (explicit id or nearest) and kills it.
            std::function<void(uint32_t)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_kill_hook;
            }
            if (hook) hook(c.kill_id);
            break;
        }
        case Command::Kind::SetTarget: {
            // Run the host's registered set-target hook on the main thread;
            // it resolves the target (explicit id or nearest) and writes
            // g.player_target_id (the T-key targeting field).
            std::function<void(uint32_t)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_set_target_hook;
            }
            if (hook) hook(c.target_id);
            break;
        }
        case Command::Kind::TractorPull: {
            // Run the host's registered tractor hook on the main thread;
            // it pulls in-range loot into the hold via loot::try_pull.
            std::function<void()> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_tractor_pull_hook;
            }
            if (hook) hook();
            break;
        }
        case Command::Kind::Rumor: {
            // Run the host's registered rumor hook on the main thread; it
            // registers a new lead via objectives::add_lead + pick_lead_pos.
            std::function<void()> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_rumor_hook;
            }
            if (hook) hook();
            break;
        }
        case Command::Kind::InventorySell: {
            // Run the host's registered inventory-sell hook on the main
            // thread; it routes to inventory::sell_item (bounds-checked).
            std::function<void(int)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_inventory_sell_hook;
            }
            if (hook) hook(c.sell_index);
            break;
        }
        case Command::Kind::InventoryGive: {
            // Run the host's registered give-item hook on the main thread;
            // it builds an InventoryItem + player::add_item.
            std::function<void(std::string, std::string, std::string, int)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_inventory_give_hook;
            }
            if (hook) hook(c.str_arg, c.give_kind, c.give_rarity, c.int_arg);
            break;
        }
        case Command::Kind::InventoryInstall: {
            // Run the host's registered install hook on the main thread; it
            // routes to inventory::install_upgrade (bounds-checked).
            std::function<void(int)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_inventory_install_hook;
            }
            if (hook) hook(c.sell_index);
            break;
        }
        case Command::Kind::InventoryEquip: {
            // Run the host's registered equip hook on the main thread; it
            // routes to inventory::equip_weapon (bounds-checked). A negative
            // mount_index tells the host to pick the first empty mount.
            std::function<void(int, int)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_inventory_equip_hook;
            }
            if (hook) hook(c.sell_index, c.mount_index);
            break;
        }
        case Command::Kind::SetPanel: {
            // Run the host's registered panel hook on the main thread; it
            // flips cockpit_hud::set_status_screen to the named screen.
            std::function<void(std::string)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_panel_hook;
            }
            if (hook) hook(c.str_arg);
            break;
        }
        case Command::Kind::CommsSelect: {
            // Run the host's registered comms-select hook on the main
            // thread; it ensures the Comms screen is up + opened, then
            // routes to comms_menu::select.
            std::function<void(int)> hook;
            {
                std::lock_guard lk(g_hooks_mu);
                hook = g_comms_select_hook;
            }
            if (hook) hook(c.int_arg);
            break;
        }
        }
    }

    // Publish camera snapshot for /state queries. Quaternion → approximate
    // yaw/pitch for human-readable telemetry. Computed from the forward
    // vector: pitch = arcsin(fwd.y), yaw = atan2(-fwd.x, -fwd.z) — same
    // sign convention as the old Euler camera so /state output is
    // backward-compatible for callers that compare past values. Roll
    // can be reconstructed from the up vector but isn't currently
    // surfaced because no caller asks for it.
    const HMM_Vec3 fwd = cam.forward();
    const float pitch_deg = std::asin(std::clamp(fwd.Y, -1.0f, 1.0f)) * HMM_RadToDeg;
    const float yaw_deg   = std::atan2(-fwd.X, -fwd.Z) * HMM_RadToDeg;

    std::lock_guard lk(g_cam_snapshot_mu);
    g_cam_pos   = cam.position;
    g_cam_euler = { pitch_deg, yaw_deg, 0.0f };
}

void maybe_capture_screenshot() {
    if (!g_pending_shot) return;

    ScreenshotWaiter* w = g_pending_shot;
    g_pending_shot = nullptr;

    bool ok = dev_remote_capture_window(w->path.c_str());
    {
        std::lock_guard lk(w->mu);
        w->done    = true;
        w->success = ok;
    }
    w->cv.notify_all();
}

void publish_system_name(const char* name) {
    std::lock_guard lk(g_system_mu);
    g_system_name = name ? name : "";
}

void publish_render_matrices(const HMM_Mat4& view_proj,
                             const HMM_Mat4& model,
                             HMM_Vec3 cam_pos) {
    std::lock_guard lk(g_proj_mu);
    g_view_proj   = view_proj;
    g_model       = model;
    g_proj_cam_pos = cam_pos;
    g_proj_ready  = true;
}

void publish_player(const PlayerInfo& p) {
    std::lock_guard lk(g_player_mu);
    g_player = p;
}

void publish_missions(const std::vector<MissionInfo>& missions) {
    std::lock_guard lk(g_missions_mu);
    g_missions = missions;
}

void push_event(const std::string& category, const std::string& text) {
    std::lock_guard lk(g_events_mu);
    g_events.push_back(Event{ ++g_event_seq, (int64_t)::time(nullptr),
                              category, text });
    while (g_events.size() > k_max_events) g_events.pop_front();
}

void publish_ships(const std::vector<ShipInfo>& ships) {
    std::lock_guard lk(g_ships_mu);
    g_ships = ships;
}

void publish_loot(const std::vector<LootInfo>& loot) {
    std::lock_guard lk(g_loot_mu);
    g_loot = loot;
}

void publish_objectives(const std::vector<LeadInfo>& objectives) {
    std::lock_guard lk(g_objectives_mu);
    g_objectives = objectives;
}

void publish_inventory(const std::vector<ItemInfo>& items, int used, int cap,
                       const std::vector<ModInfo>& mods,
                       const std::vector<MountInfo>& mounts) {
    std::lock_guard lk(g_inventory_mu);
    g_inventory      = items;
    g_permanent_mods = mods;
    g_mounts         = mounts;
    g_cargo_used     = used;
    g_cargo_cap      = cap;
}

void set_cargo_give_hook(std::function<void(std::string, int)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_cargo_give_hook = std::move(hook);
}

void set_spawn_hook(std::function<void(std::string, std::string, float)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_spawn_hook = std::move(hook);
}

void set_kill_hook(std::function<void(uint32_t)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_kill_hook = std::move(hook);
}

void set_target_hook(std::function<void(uint32_t)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_set_target_hook = std::move(hook);
}

void set_tractor_pull_hook(std::function<void()> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_tractor_pull_hook = std::move(hook);
}

void set_rumor_hook(std::function<void()> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_rumor_hook = std::move(hook);
}

void set_inventory_sell_hook(std::function<void(int)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_inventory_sell_hook = std::move(hook);
}

void set_inventory_give_hook(
    std::function<void(std::string, std::string, std::string, int)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_inventory_give_hook = std::move(hook);
}

void set_inventory_install_hook(std::function<void(int)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_inventory_install_hook = std::move(hook);
}

void set_inventory_equip_hook(std::function<void(int, int)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_inventory_equip_hook = std::move(hook);
}

void set_panel_hook(std::function<void(std::string)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_panel_hook = std::move(hook);
}

void set_comms_select_hook(std::function<void(int)> hook) {
    std::lock_guard lk(g_hooks_mu);
    g_comms_select_hook = std::move(hook);
}

} // namespace dev_remote
