// -----------------------------------------------------------------------------
// cinematic_studio_io.cpp — Studio request writer + directory join (engine-
// free half of the Phase-B panel; see the header for the design contract).
// -----------------------------------------------------------------------------

#include "cinematic_studio_io.h"

#include "json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cinematic::studio_io {

// ---- JSON emitting ----------------------------------------------------------

// JSON string escape, UTF-8 preserving: only what the spec REQUIRES is
// escaped (quote, backslash, control chars). Multibyte UTF-8 sequences pass
// through verbatim — no \uXXXX mangling of em-dashes or accents.
static void emit_escaped(std::ostringstream& o, const std::string& s) {
    o << '"';
    for (const char c : s) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n";  break;
            case '\t': o << "\\t";  break;
            case '\r': o << "\\r";  break;
            case '\b': o << "\\b";  break;
            case '\f': o << "\\f";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o << buf;
                } else {
                    o << c;
                }
        }
    }
    o << '"';
}

static void emit_kv(std::ostringstream& o, const char* indent,
                    const char* key, const std::string& val, bool& first) {
    if (!first) o << ",\n";
    first = false;
    o << indent << '"' << key << "\": ";
    emit_escaped(o, val);
}

static void emit_index_list(std::ostringstream& o, const std::vector<int>& v) {
    o << '[';
    for (size_t i = 0; i < v.size(); ++i) o << (i ? ", " : "") << v[i];
    o << ']';
}

// Floats in the request are speeds (0.7-1.3) — two decimals is plenty and
// keeps the file diff-stable.
static std::string fmt_speed(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", (double)f);
    return buf;
}

std::string request_to_json(const Request& r) {
    std::ostringstream o;
    bool first = true;
    o << "{\n";
    emit_kv(o, "  ", "id",   r.id,   first);
    emit_kv(o, "  ", "kind", r.kind, first);
    if (!r.cinematic_id.empty())  emit_kv(o, "  ", "cinematic_id",  r.cinematic_id,  first);
    if (!r.brief.empty())         emit_kv(o, "  ", "brief",         r.brief,         first);
    if (!r.triggers_text.empty()) emit_kv(o, "  ", "triggers_text", r.triggers_text, first);
    if (!r.outcome_text.empty())  emit_kv(o, "  ", "outcome_text",  r.outcome_text,  first);

    // image block — always emitted (the bridge always needs a quality).
    o << ",\n  \"image\": { \"quality\": ";
    emit_escaped(o, r.image_quality);
    if (!r.image_style_extra.empty()) {
        o << ", \"style_extra\": ";
        emit_escaped(o, r.image_style_extra);
    }
    o << " }";

    if (!r.line_overrides.empty()) {
        o << ",\n  \"line_overrides\": [\n";
        for (size_t i = 0; i < r.line_overrides.size(); ++i) {
            const LineOverride& lo = r.line_overrides[i];
            o << "    { \"index\": " << lo.index;
            bool f2 = false;
            (void)f2;
            if (lo.has_text)     { o << ",\n      "; o << "\"text\": ";     emit_escaped(o, lo.text); }
            if (lo.has_emotion)  { o << ",\n      "; o << "\"emotion\": ";  emit_escaped(o, lo.emotion); }
            if (lo.has_portrait_extra) {
                o << ",\n      \"portrait_prompt_extra\": ";
                emit_escaped(o, lo.portrait_prompt_extra);
            }
            if (lo.has_voice_id) { o << ",\n      \"voice_id\": ";   emit_escaped(o, lo.voice_id); }
            if (lo.has_speed)    { o << ",\n      \"speed\": " << fmt_speed(lo.speed); }
            if (lo.has_seed_image) {
                o << ",\n      \"seed_image\": ";
                emit_escaped(o, lo.seed_image);
            }
            o << " }" << (i + 1 == r.line_overrides.size() ? "\n" : ",\n");
        }
        o << "  ]";
    }

    if (!r.regen_portraits.empty() || !r.regen_voices.empty()) {
        o << ",\n  \"regen\": { \"portraits\": ";
        emit_index_list(o, r.regen_portraits);
        o << ", \"voices\": ";
        emit_index_list(o, r.regen_voices);
        o << " }";
    }

    o << "\n}\n";
    return o.str();
}

bool write_request(const Request& r, const std::string& studio_dir,
                   std::string& err) {
    std::error_code ec;
    const fs::path dir = fs::path(studio_dir) / "requests";
    fs::create_directories(dir, ec);   // ok if it already exists
    const fs::path path = dir / (r.id + ".json");
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "cannot open " + path.string() + " for writing";
        return false;
    }
    out << request_to_json(r);
    if (!out.good()) {
        err = "write failed for " + path.string();
        return false;
    }
    std::printf("[studio] wrote request %s\n", path.string().c_str());
    return true;
}

// ---- request/response listing ------------------------------------------------

// Non-throwing string lookup (json.h operator[] THROWS on a missing key;
// find() is the only accessor allowed on files other people write).
static std::string str_field(const json::Value& obj, const char* key,
                             const std::string& fallback) {
    const json::Value* v = obj.find(key);
    return v ? v->string_or(fallback) : fallback;
}

static long long file_mtime_unix(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    // file_clock -> system_clock (C++17-portable enough: both count from an
    // epoch; the delta trick below survives libc++/libstdc++ differences).
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(
        sys.time_since_epoch()).count();
}

std::vector<RequestEntry> scan_requests(const std::string& studio_dir) {
    std::vector<RequestEntry> out;
    std::error_code ec;
    const fs::path req_dir = fs::path(studio_dir) / "requests";
    if (!fs::is_directory(req_dir, ec)) return out;   // no dir yet = no rows

    for (const auto& de : fs::directory_iterator(req_dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file() || de.path().extension() != ".json") continue;
        RequestEntry e;
        e.id    = de.path().stem().string();
        e.mtime = file_mtime_unix(de.path());

        // Request side (non-throwing: parse_file logs + returns Null on junk).
        const json::Value req = json::parse_file(de.path().string());
        e.kind = str_field(req, "kind", "?");
        e.cinematic_id = str_field(req, "cinematic_id", "");

        // Response side — absent file stays "pending".
        const fs::path resp_path =
            fs::path(studio_dir) / "responses" / (e.id + ".json");
        if (fs::exists(resp_path, ec)) {
            const json::Value resp = json::parse_file(resp_path.string());
            e.status  = str_field(resp, "status", "pending");
            e.message = str_field(resp, "message", "");
            const std::string cid = str_field(resp, "cinematic_id", "");
            if (!cid.empty()) e.cinematic_id = cid;
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [](const RequestEntry& a, const RequestEntry& b) {
                  return a.mtime > b.mtime;   // newest first
              });
    return out;
}

bool delete_request(const std::string& studio_dir, const std::string& id) {
    std::error_code ec;
    bool any = false;
    any |= fs::remove(fs::path(studio_dir) / "requests"  / (id + ".json"), ec);
    any |= fs::remove(fs::path(studio_dir) / "responses" / (id + ".json"), ec);
    if (any) std::printf("[studio] deleted request pair '%s'\n", id.c_str());
    return any;
}

// ---- voice profiles + cinematic listing --------------------------------------

std::vector<VoiceProfile> load_voice_profiles(const std::string& path) {
    std::vector<VoiceProfile> out;
    const json::Value root = json::parse_file(path);
    if (!root.is_array()) {
        std::printf("[studio] voice profiles missing/malformed: %s\n",
                    path.c_str());
        return out;
    }
    for (const json::Value& v : root.as_array()) {
        if (!v.is_object()) continue;
        VoiceProfile p;
        p.id = str_field(v, "id", "");
        if (p.id.empty()) continue;   // an id-less profile is unusable
        p.label  = str_field(v, "label", p.id);
        p.flavor = str_field(v, "flavor", "");
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<std::string> list_cinematic_ids(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file() || de.path().extension() != ".json") continue;
        const std::string stem = de.path().stem().string();
        if (stem == "schema" || stem == "triggers") continue;   // not cutscenes
        out.push_back(stem);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string character_from_portrait(const std::string& portrait_path) {
    // Expect ".../portraits/<char>/<file>.png" (the cinematic JSON stores it
    // relative, e.g. "portraits/pirate/ambush_troy_01.png").
    const std::string key = "portraits/";
    const size_t at = portrait_path.rfind(key);
    if (at == std::string::npos) return "";
    const size_t start = at + key.size();
    const size_t slash = portrait_path.find('/', start);
    if (slash == std::string::npos || slash == start) return "";
    return portrait_path.substr(start, slash - start);
}

} // namespace cinematic::studio_io
