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

int load(const std::string& toml_path) {
    g_catalog.clear();
    g_by_id.clear();

    std::ifstream in(toml_path);
    if (!in) {
        std::fprintf(stderr, "[commodity] cannot open '%s' — catalog empty\n",
                     toml_path.c_str());
        return 0;
    }

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

    // Index by id + count distinct categories for the summary line.
    std::set<std::string> categories;
    for (size_t i = 0; i < g_catalog.size(); ++i) {
        g_by_id[g_catalog[i].id] = i;
        categories.insert(g_catalog[i].category);
    }

    std::printf("[commodity] %zu commodities, %zu categories\n",
                g_catalog.size(), categories.size());
    return (int)g_catalog.size();
}

const Commodity* find(std::string_view id) {
    const auto it = g_by_id.find(std::string(id));
    return (it == g_by_id.end()) ? nullptr : &g_catalog[it->second];
}

const std::vector<Commodity>& all() {
    return g_catalog;
}

} // namespace commodity
