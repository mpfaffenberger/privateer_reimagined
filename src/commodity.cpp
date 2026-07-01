// -----------------------------------------------------------------------------
// commodity.cpp — cargo.toml subset parser + catalog.
//
// The "TOML parser" here is deliberately NOT a TOML parser. It reads
// exactly the shape tools/import_privateer_db/cargo.py emits:
//
//   # comment
//   [_meta]                      <- table header (skipped wholesale)
//   [[commodities]]              <- array-of-tables header (starts an entry)
//   label = "Grain"              <- quoted string value
//   category_index = 0           <- bare number value (unused here)
//   categories = ["FOOD", ...]   <- array value (only in _meta; skipped)
//
// Anything outside that shape logs a warning with the line number so a
// future extractor change announces itself instead of being silently
// half-loaded. See the header for why this beats an offline JSON
// conversion step.
// -----------------------------------------------------------------------------

#include "commodity.h"

#include "json.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <unordered_map>

namespace {

std::vector<Commodity> g_catalog;
// id -> index into g_catalog. Rebuilt by load(); indices stay valid
// because the catalog is never resized after load() returns.
std::unordered_map<std::string, size_t> g_by_id;

// ---- contraband (Phase 1) ---------------------------------------------
// id -> severity. Rebuilt by load_contraband(); not serialized. Severity
// is a gameplay knob the search director reads to decide "smuggler" vs
// "pirate" vs "traced" — a hailing Militia will tolerate severity 1 with
// a fine, but severity 3 lights you up.
std::unordered_map<std::string, int> g_contraband;

// Trim leading/trailing whitespace. The extractor doesn't emit stray
// spaces today; this guards against a hand-edited file.
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.remove_suffix(1);
    return s;
}

} // namespace

namespace commodity {

std::string id_from_label(std::string_view label) {
    std::string id;
    id.reserve(label.size());
    for (char c : label) {
        if (c == ' ' || c == '/' || c == '-') {
            id += '_';
        } else {
            id += (char)std::tolower((unsigned char)c);
        }
    }
    return id;
}

namespace {

// Parse a cargo.toml-format file, APPENDING each [[commodities]] entry to
// g_catalog. Shared by load() (which clears first) and load_extra() (which
// doesn't). Returns the number of entries appended; -1 if the file couldn't
// be opened. Does NOT touch g_by_id — callers re-index once afterwards.
int parse_into_catalog(const std::string& toml_path) {
    std::ifstream in(toml_path);
    if (!in) {
        std::fprintf(stderr, "[commodity] cannot open '%s'\n", toml_path.c_str());
        return -1;
    }

    const size_t start = g_catalog.size();

    // Parse state: are we inside a [[commodities]] entry? Entries are
    // flushed when the next header (or EOF) arrives, so a truncated
    // final entry still lands.
    bool      in_entry = false;
    Commodity cur;
    auto flush = [&] {
        if (in_entry && !cur.label.empty()) {
            cur.id = id_from_label(cur.label);
            g_catalog.push_back(cur);
        }
        cur      = Commodity{};
        in_entry = false;
    };

    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::string_view t = trim(line);
        if (t.empty() || t.front() == '#') continue;

        // Headers. [[commodities]] starts a new entry; any other
        // [section] (i.e. [_meta]) just ends the current one.
        if (t.front() == '[') {
            flush();
            if (t == "[[commodities]]") in_entry = true;
            continue;
        }

        // key = value lines. Only label/category matter; _-prefixed
        // keys are extraction debug per the privateer_db README, and
        // numeric/array values we don't need are skipped by the same
        // fallthrough.
        const size_t eq = t.find('=');
        if (eq == std::string_view::npos) {
            std::fprintf(stderr, "[commodity] %s:%d unparsed line: '%.*s'\n",
                         toml_path.c_str(), line_no, (int)t.size(), t.data());
            continue;
        }
        const std::string_view key = trim(t.substr(0, eq));
        const std::string_view val = trim(t.substr(eq + 1));
        if (!in_entry)                       continue;  // _meta keys
        if (!key.empty() && key[0] == '_')   continue;  // debug breadcrumbs

        // Quoted-string values only — that's all label/category are.
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            const std::string_view inner = val.substr(1, val.size() - 2);
            if      (key == "label")    cur.label    = std::string(inner);
            else if (key == "category") cur.category = std::string(inner);
        }
        // Bare numbers (category_index) and arrays: nothing to do.
    }
    flush();
    return (int)(g_catalog.size() - start);
}

// Rebuild the id->index map from g_catalog and log a summary. Pointers into
// g_catalog stay valid as long as nothing resizes it after the startup load
// sequence (load + load_extra) completes.
void reindex(const char* what) {
    g_by_id.clear();
    std::set<std::string> categories;
    for (size_t i = 0; i < g_catalog.size(); ++i) {
        g_by_id[g_catalog[i].id] = i;
        categories.insert(g_catalog[i].category);
    }
    std::printf("[commodity] %s: %zu commodities, %zu categories\n",
                what, g_catalog.size(), categories.size());
}

} // namespace

int load(const std::string& toml_path) {
    g_catalog.clear();
    g_by_id.clear();
    if (parse_into_catalog(toml_path) < 0) return 0;
    reindex("loaded");
    return (int)g_catalog.size();
}

int load_extra(const std::string& toml_path) {
    const int added = parse_into_catalog(toml_path);
    if (added < 0) return 0;
    reindex("with salvage");
    return added;
}

const Commodity* find(std::string_view id) {
    const auto it = g_by_id.find(std::string(id));
    return (it == g_by_id.end()) ? nullptr : &g_catalog[it->second];
}

const std::vector<Commodity>& all() {
    return g_catalog;
}

// ---- contraband (Phase 1) ---------------------------------------------
// Parse assets/data/contraband.json:
//   { "contraband": [ { "id": "brilliance", "severity": 3 }, ... ] }
// Missing/unparseable file is non-fatal: the game runs with an empty
// contraband set and every check degrades to "not contraband". The log
// line makes the failure mode obvious (vs a silent default-true).
int load_contraband(const std::string& path) {
    g_contraband.clear();

    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[commodity] no contraband table at '%s'\n", path.c_str());
        return 0;
    }
    const json::Value* arr = root.find("contraband");
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[commodity] '%s' missing 'contraband' array\n", path.c_str());
        return 0;
    }

    int loaded = 0;
    for (const json::Value& e : arr->as_array()) {
        if (!e.is_object()) continue;
        const json::Value* id_p  = e.find("id");
        const json::Value* sev_p = e.find("severity");
        if (!id_p || !id_p->is_string()) continue;
        const std::string id = id_p->as_string();
        int sev = (sev_p && sev_p->is_number()) ? sev_p->as_int() : 1;
        g_contraband[id] = sev;
        ++loaded;
    }

    std::printf("[commodity] %d contraband ids\n", loaded);
    return loaded;
}

bool is_contraband(std::string_view id) {
    return g_contraband.find(std::string(id)) != g_contraband.end();
}

int contraband_severity(std::string_view id) {
    const auto it = g_contraband.find(std::string(id));
    return (it == g_contraband.end()) ? 0 : it->second;
}

} // namespace commodity
