// -----------------------------------------------------------------------------
// json.cpp — recursive-descent parser for the tiny JSON subset we need.
// -----------------------------------------------------------------------------

#include "json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace json {

// ==== Value typed accessors =================================================

static void type_error(const char* want, Value::Type have) {
    static const char* names[] = {"null","bool","number","string","object","array"};
    throw std::runtime_error(std::string("[json] type mismatch: wanted ") +
                             want + ", have " + names[have]);
}

bool   Value::as_bool()   const { if (type != Bool_)   type_error("bool", type);   return b; }
double Value::as_number() const { if (type != Number_) type_error("number", type); return n; }
float  Value::as_float()  const { return (float)as_number(); }
int    Value::as_int()    const { return (int)as_number(); }
uint32_t Value::as_u32()  const { return (uint32_t)as_number(); }
const std::string& Value::as_string() const {
    if (type != String_) type_error("string", type);
    return s;
}
const std::unordered_map<std::string, Value>& Value::as_object() const {
    if (type != Object_) type_error("object", type);
    return obj;
}
const std::vector<Value>& Value::as_array() const {
    if (type != Array_) type_error("array", type);
    return arr;
}

const Value& Value::operator[](const std::string& key) const {
    if (type != Object_) type_error("object", type);
    auto it = obj.find(key);
    if (it == obj.end())
        throw std::runtime_error("[json] missing key: " + key);
    return it->second;
}
const Value& Value::operator[](size_t idx) const {
    if (type != Array_) type_error("array", type);
    if (idx >= arr.size())
        throw std::runtime_error("[json] array index out of range");
    return arr[idx];
}

bool Value::contains(const std::string& key) const {
    return type == Object_ && obj.find(key) != obj.end();
}
const Value* Value::find(const std::string& key) const {
    if (type != Object_) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}

double Value::number_or(double fallback) const {
    return type == Number_ ? n : fallback;
}
bool Value::bool_or(bool fallback) const {
    return type == Bool_ ? b : fallback;
}
std::string Value::string_or(const std::string& fallback) const {
    return type == String_ ? s : fallback;
}

// ==== Parser ================================================================

namespace {

// Recursion depth cap (#10): hostile deeply-nested input could otherwise
// blow the native stack via the mutual recursion below. 200 levels is far
// past anything our hand-written config files reach.
constexpr int k_max_depth = 200;

struct Parser {
    const char* p;
    const char* end;
    int line = 1;
    int col  = 1;
    int depth = 0;
    bool failed = false;

    void fail(const char* msg) {
        if (!failed) {
            std::fprintf(stderr, "[json] parse error at %d:%d — %s\n", line, col, msg);
            failed = true;
        }
    }

    bool eof() const { return p >= end; }

    void bump() {
        if (p < end) {
            if (*p == '\n') { line++; col = 1; } else { col++; }
            p++;
        }
    }

    // Skip whitespace AND // line comments. JSON spec disallows comments but
    // we're the only consumer — ergonomics > spec purity for hand-written
    // config files.
    void skip_ws() {
        for (;;) {
            while (!eof() && std::isspace((unsigned char)*p)) bump();
            if (!eof() && p + 1 < end && p[0] == '/' && p[1] == '/') {
                while (!eof() && *p != '\n') bump();
            } else break;
        }
    }

    // ---- forward decl's ------------------------------------------------
    Value parse_value();
    Value parse_string();
    Value parse_number();
    Value parse_object();
    Value parse_array();
    Value parse_keyword();

    bool expect(char c) {
        if (eof() || *p != c) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "expected '%c'", c);
            fail(buf);
            return false;
        }
        bump();
        return true;
    }

    // Read exactly 4 hex digits FOLLOWING the current position into `cp`
    // (used by the \uXXXX escape). Advances p to the last hex digit so the
    // caller's trailing bump() lands past it. Returns false (and fails) on
    // EOF or a non-hex digit.
    bool read_hex4(unsigned& cp) {
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            bump();
            if (eof()) { fail("unterminated \\u escape"); return false; }
            const char h = *p;
            unsigned d;
            if      (h >= '0' && h <= '9') d = (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') d = 10u + (unsigned)(h - 'a');
            else if (h >= 'A' && h <= 'F') d = 10u + (unsigned)(h - 'A');
            else { fail("bad hex digit in \\u escape"); return false; }
            cp = (cp << 4) | d;
        }
        return true;
    }
};

// Append a Unicode code point to `out` as UTF-8 bytes (1–4 bytes).
static void append_utf8(std::string& out, unsigned cp) {
    if (cp <= 0x7Fu) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FFu) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

Value Parser::parse_string() {
    // Leading '"' already looked at in parse_value; we assume *p == '"'.
    bump();
    std::string out;
    out.reserve(16);
    while (!eof() && *p != '"') {
        if (*p == '\\') {
            bump();
            if (eof()) { fail("unterminated escape"); break; }
            switch (*p) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    // \uXXXX unicode escape (np-cinematic fix): decode 4
                    // hex digits to a code point, stitch UTF-16 surrogate
                    // pairs, and emit UTF-8 bytes. Previously this hit the
                    // default and failed with "bad escape sequence", which
                    // is exactly what tripped up an em-dash (\u2014) written
                    // by a JSON encoder with ensure_ascii=True. Now the
                    // engine accepts BOTH literal UTF-8 and \u escapes.
                    unsigned cp = 0;
                    if (!read_hex4(cp)) return {};
                    // High surrogate: pair it with a following \uDC00..DFFF.
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p + 2 < end && p[1] == '\\' && p[2] == 'u') {
                            bump();            // -> '\\'
                            bump();            // -> 'u'
                            unsigned lo = 0;
                            if (!read_hex4(lo)) return {};
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10)
                                             + (lo - 0xDC00);
                            } else {
                                fail("bad low surrogate in \\u escape");
                                return {};
                            }
                        } else {
                            fail("unpaired high surrogate in \\u escape");
                            return {};
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:   fail("bad escape sequence"); return {};
            }
            bump();
        } else {
            out.push_back(*p);
            bump();
        }
    }
    if (!expect('"')) return {};
    Value v;
    v.type = Value::String_;
    v.s    = std::move(out);
    return v;
}

Value Parser::parse_number() {
    // Copy the numeric token into a bounded NUL-terminated buffer BEFORE
    // strtod (#6): strtod scans to a NUL, but the public parse(string_view)
    // entry is not guaranteed NUL-terminated — handing strtod `p` directly
    // could read past `end`. Scan only the chars a JSON number can hold,
    // capped to the buffer (any real number is a handful of chars; a token
    // longer than this is malformed and strtod stops at the first bad one).
    char   buf[64];
    size_t n = 0;
    for (const char* q = p; q < end && n + 1 < sizeof buf; ++q) {
        const char ch = *q;
        const bool numeric = (ch >= '0' && ch <= '9') ||
                             ch == '+' || ch == '-' || ch == '.' ||
                             ch == 'e' || ch == 'E';
        if (!numeric) break;
        buf[n++] = ch;
    }
    buf[n] = '\0';

    char* endp = nullptr;
    double d = std::strtod(buf, &endp);
    if (endp == buf) { fail("bad number"); return {}; }
    int consumed = (int)(endp - buf);
    for (int i = 0; i < consumed; ++i) bump();
    Value v;
    v.type = Value::Number_;
    v.n    = d;
    return v;
}

Value Parser::parse_keyword() {
    auto eat = [&](const char* kw) {
        int n = (int)std::strlen(kw);
        if (end - p < n) return false;
        for (int i = 0; i < n; ++i) if (p[i] != kw[i]) return false;
        for (int i = 0; i < n; ++i) bump();
        return true;
    };
    Value v;
    if (eat("true"))  { v.type = Value::Bool_; v.b = true;  return v; }
    if (eat("false")) { v.type = Value::Bool_; v.b = false; return v; }
    if (eat("null"))  { v.type = Value::Null_;              return v; }
    fail("unknown literal");
    return {};
}

Value Parser::parse_object() {
    bump();                 // consume '{'
    Value v;
    v.type = Value::Object_;
    skip_ws();
    if (!eof() && *p == '}') { bump(); return v; }
    for (;;) {
        skip_ws();
        if (eof() || *p != '"') { fail("expected string key"); return {}; }
        Value k = parse_string();
        if (failed) return {};
        skip_ws();
        if (!expect(':')) return {};
        skip_ws();
        Value val = parse_value();
        if (failed) return {};
        v.obj.emplace(std::move(k.s), std::move(val));
        skip_ws();
        if (!eof() && *p == ',') { bump(); continue; }
        if (!eof() && *p == '}') { bump(); return v; }
        fail("expected ',' or '}' in object");
        return {};
    }
}

Value Parser::parse_array() {
    bump();                 // consume '['
    Value v;
    v.type = Value::Array_;
    skip_ws();
    if (!eof() && *p == ']') { bump(); return v; }
    for (;;) {
        skip_ws();
        Value elem = parse_value();
        if (failed) return {};
        v.arr.push_back(std::move(elem));
        skip_ws();
        if (!eof() && *p == ',') { bump(); continue; }
        if (!eof() && *p == ']') { bump(); return v; }
        fail("expected ',' or ']' in array");
        return {};
    }
}

Value Parser::parse_value() {
    skip_ws();
    if (eof()) { fail("unexpected end of input"); return {}; }
    char c = *p;
    if (c == '"')                               return parse_string();
    if (c == '{' || c == '[') {
        // Bounded recursion (#10): each object/array nest is one native
        // stack frame via parse_value; cap the descent so hostile input
        // can't overflow the stack.
        if (++depth > k_max_depth) { fail("max nesting depth exceeded"); --depth; return {}; }
        Value v = (c == '{') ? parse_object() : parse_array();
        --depth;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9'))     return parse_number();
    if (c == 't' || c == 'f' || c == 'n')       return parse_keyword();
    fail("unexpected character");
    return {};
}

} // anonymous namespace

// ==== public entry points ===================================================

Value parse(std::string_view text) {
    Parser p{ text.data(), text.data() + text.size() };
    Value v = p.parse_value();
    if (p.failed) return {};
    p.skip_ws();
    if (!p.eof()) {
        std::fprintf(stderr, "[json] trailing content at %d:%d\n", p.line, p.col);
        return {};
    }
    return v;
}

Value parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[json] cannot open '%s'\n", path.c_str());
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

} // namespace json
