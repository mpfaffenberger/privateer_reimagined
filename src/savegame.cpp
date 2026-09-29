// -----------------------------------------------------------------------------
// savegame.cpp — files, slots, and the public savegame API (savegame.h).
//
// See savegame.h for the path choice, slot scheme, versioning + stable-key
// rationale. This file owns everything that touches the filesystem: the
// per-user data dir, slot paths, atomic writes, and the save listing. The
// PlayerState <-> JSON codec lives behind savegame_codec.h
// (savegame_write.cpp / savegame_read.cpp); load() and peek_path() own the
// "never throws" guarantee by catching anything the codec lets escape.
// -----------------------------------------------------------------------------

#include "savegame.h"

#include "json.h"
#include "player.h"
#include "savegame_codec.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace savegame {

namespace {

// Platform-appropriate per-user data dir. On macOS this is
// `~/Library/Application Support/`; on Windows it's `%APPDATA%` (typically
// `C:\Users\<user>\AppData\Roaming`); on Linux it's `$XDG_DATA_HOME` or
// `~/.local/share/`. Falls back to `$HOME` (Unix) or `%USERPROFILE%` (Win)
// only when the proper override isn't set.
//
// Returns "" if no usable path can be found (degenerate; caller logs).
//
// NP_DATA_DIR overrides the platform default entirely. Test harnesses use it
// to keep scratch/corrupt fixture saves out of the player's real save picker
// (#242); it also lets a portable install carry its saves alongside the game.
std::string user_data_dir() {
    if (const char* o = std::getenv("NP_DATA_DIR"); o && *o) {
        return (fs::path(o) / "new_privateer").string();
    }
#ifdef _WIN32
    // Prefer APPDATA — that's where Windows apps are expected to stash
    // mutable per-user state. %USERPROFILE% is a last-resort fallback.
    if (const char* a = std::getenv("APPDATA"); a && *a) {
        fs::path p = fs::path(a);
        p /= "new_privateer";
        return p.string();
    }
    if (const char* u = std::getenv("USERPROFILE"); u && *u) {
        fs::path p = fs::path(u);
        p /= "AppData";
        p /= "Roaming";
        p /= "new_privateer";
        return p.string();
    }
    return {};
#else
    // macOS: $HOME/Library/Application Support/new_privateer
    // Linux: $XDG_DATA_HOME/new_privateer  or  $HOME/.local/share/new_privateer
    if (const char* h = std::getenv("HOME"); h && *h) {
        fs::path p = fs::path(h);
#ifdef __APPLE__
        p /= "Library";
        p /= "Application Support";
        p /= "new_privateer";
        return p.string();
#else
        if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
            fs::path px = fs::path(x);
            px /= "new_privateer";
            return px.string();
        }
        p /= ".local";
        p /= "share";
        p /= "new_privateer";
        return p.string();
#endif
    }
    return {};
#endif
}

} // namespace

std::string saves_dir() {
    std::string base = user_data_dir();
    if (base.empty()) {
        std::fprintf(stderr, "[save] no user data dir (HOME/APPDATA unset) — cannot locate saves dir\n");
        return {};
    }
    const fs::path dir = fs::path(base) / "saves";
    // Log via string(), never path::c_str(): that's wchar_t* on Windows and
    // "%s" would print only the drive letter (#239).
    const std::string dir_str = dir.string();
    std::error_code ec;
    fs::create_directories(dir, ec);   // no-op if it already exists
    if (ec) {
        std::fprintf(stderr, "[save] could not create '%s': %s\n",
                     dir_str.c_str(), ec.message().c_str());
        return {};
    }
    return dir_str;
}

std::string slot_path(int slot) {
    std::string base = user_data_dir();
    if (base.empty()) return {};
    fs::path dir = fs::path(base) / "saves";
    return (dir / ("save_" + std::to_string(slot) + ".json")).string();
}

// ---- save -------------------------------------------------------------------

// Atomic write: temp file then rename (POSIX rename is atomic within a
// filesystem), so a crash mid-write can't clobber an existing good save.
static bool write_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[save] cannot open '%s' for write\n", tmp.c_str());
            return false;
        }
        f << content;
        if (!f.good()) {
            std::fprintf(stderr, "[save] write error on '%s'\n", tmp.c_str());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        std::fprintf(stderr, "[save] rename '%s' -> '%s' failed: %s\n",
                     tmp.c_str(), path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool save(const PlayerState& p, int slot) {
    if (saves_dir().empty()) return false;   // ensures the tree exists
    const std::string path = slot_path(slot);
    if (!write_atomic(path, codec::encode(p))) return false;
    std::printf("[save] wrote slot %d -> %s (%lld cr)\n",
                slot, path.c_str(), (long long)p.credits);
    return true;
}

std::string save_timestamped(const PlayerState& p) {
    const std::string dir = saves_dir();   // ensures the tree exists
    if (dir.empty()) return {};
    // Unique filename from nanoseconds-since-epoch: saves never collide and
    // never overwrite — unbounded accumulation by design (np-3dp.19).
    const long long nanos = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string path =
        (fs::path(dir) / ("save_" + std::to_string(nanos) + ".json")).string();
    if (!write_atomic(path, codec::encode(p))) return {};
    std::printf("[save] wrote %s (%lld cr)\n", path.c_str(), (long long)p.credits);
    return path;
}

// ---- load -------------------------------------------------------------------

bool load(PlayerState& p, int slot) {
    const std::string path = slot_path(slot);
    if (path.empty()) return false;
    return load(p, path);
}

bool load(PlayerState& p, const std::string& path) {
    if (path.empty()) return false;
    const int slot = -1;   // path-based load; the slot # is only for logs

    // Missing file is the common, non-error case (no save yet) — quiet-ish.
    if (!fs::exists(fs::path(path))) {
        std::printf("[save] no file at %s\n", path.c_str());
        return false;
    }

    // The parser logs its own diagnostics and returns a Null Value on any
    // syntax error, so a truncated/corrupt file lands here as !is_object().
    const json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[save] slot %d: corrupt or unparseable (%s)\n",
                     slot, path.c_str());
        return false;
    }

    // Belt-and-suspenders: typed accessors throw on a type mismatch we didn't
    // guard. Catch everything so a malformed-but-parseable file can't crash.
    // Decode into a fresh struct, then commit on success so a partial parse
    // never leaves the live PlayerState half-overwritten.
    try {
        PlayerState out;
        if (!codec::decode(root, slot, out)) return false;
        p = std::move(out);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[save] slot %d: deserialize error — %s\n", slot, ex.what());
        return false;
    }

    std::printf("[save] loaded %s (%lld cr, system '%s', base '%s')\n",
                path.c_str(), (long long)p.credits, p.current_system.c_str(),
                p.last_docked_base.c_str());
    return true;
}

// ---- peek -------------------------------------------------------------------

SlotInfo peek_path(const std::string& path) {
    SlotInfo info;
    if (path.empty() || !fs::exists(fs::path(path))) return info;
    info.path = path;

    const json::Value root = json::parse_file(path);
    if (!root.is_object()) return info;   // corrupt -> exists stays false

    try {
        info = codec::peek(root);
        info.path = path;
    } catch (const std::exception&) {
        info = SlotInfo{};   // anything unexpected -> treat as absent
    }
    return info;
}

SlotInfo peek(int slot) {
    const std::string path = slot_path(slot);
    if (path.empty()) return SlotInfo{};
    return peek_path(path);
}

// ---- list -------------------------------------------------------------------

std::vector<SlotInfo> list_saves() {
    std::vector<SlotInfo> out;
    const std::string dir = saves_dir();
    if (dir.empty()) return out;

    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(fs::path(dir), ec)) {
        if (ec) break;
        if (!ent.is_regular_file()) continue;
        const std::string fname = ent.path().filename().string();
        // Only our save files; skip the .tmp scratch + anything else.
        if (fname.rfind("save_", 0) != 0) continue;
        if (ent.path().extension() != ".json") continue;
        SlotInfo info = peek_path(ent.path().string());
        if (!info.exists) {
            // Name the offender: the raw [json] parse error above this line
            // carries no filename, which made #242 needlessly mysterious.
            std::fprintf(stderr, "[save] skipping unreadable save file '%s'\n",
                         ent.path().string().c_str());
            continue;
        }
        out.push_back(std::move(info));
    }
    // Newest first (by embedded unix timestamp; ties broken by path so the
    // order is stable).
    std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b) {
        if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
        return a.path > b.path;
    });
    return out;
}

} // namespace savegame
