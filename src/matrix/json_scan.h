#pragma once
// Zero/low-allocation JSON field scanners, used instead of cJSON for anything
// parsed while background threads (SyncWorker, Client's action worker) are
// alive. This mirrors a hard-won pattern from the sibling discord-wii-u
// project: cJSON's parser does many rapid small allocations, and running it
// concurrently with another thread's allocations corrupts newlib's
// non-thread-safe heap on Wii U. cJSON is only safe here during the single-
// threaded login() call, before any worker thread has been started.
//
// These helpers only build small, bounded std::string results (or none at
// all when just locating byte ranges) — no bulk/recursive allocation.
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>

namespace Matrix {

// Given `p` pointing at the first byte of a JSON value (after the ':' and any
// whitespace), returns a pointer just past the end of that value. Handles
// objects, arrays, strings, numbers, and true/false/null. Used to bound
// values whose key is not known ahead of time (e.g. room IDs used as object
// keys) or whose type varies.
inline const char *skip_json_value(const char *p, const char *end) {
    if (p >= end) return p;
    char c = *p;
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        while (p < end) {
            char ch = *p;
            if (in_str) {
                if (ch == '\\') { p += 2; continue; }
                if (ch == '"') { in_str = false; p++; continue; }
                p++;
                continue;
            }
            if (ch == '"') { in_str = true; p++; continue; }
            if (ch == open) depth++;
            else if (ch == close) { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        return p;
    }
    if (c == '"') {
        p++;
        while (p < end) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') { p++; break; }
            p++;
        }
        return p;
    }
    // number / true / false / null — read until a delimiter
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

// Decode a JSON string literal starting at *p == '"' (advances p past the
// closing quote) into a UTF-8 std::string, handling the standard escapes
// including \uXXXX (with surrogate pair support for astral codepoints).
inline std::string decode_json_string(const char *&p, const char *end) {
    std::string result;
    if (p >= end || *p != '"') return result;
    p++; // skip opening quote
    while (p < end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p >= end) break;
            switch (*p) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"';  break;
                case '\\': result += '\\'; break;
                case '/': result += '/';  break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'u': {
                    auto hx = [](char ch) -> uint32_t {
                        if (ch >= '0' && ch <= '9') return (uint32_t)(ch - '0');
                        if (ch >= 'a' && ch <= 'f') return (uint32_t)(ch - 'a' + 10);
                        if (ch >= 'A' && ch <= 'F') return (uint32_t)(ch - 'A' + 10);
                        return 0;
                    };
                    if (p + 4 < end) {
                        uint32_t cp = (hx(p[1]) << 12) | (hx(p[2]) << 8) | (hx(p[3]) << 4) | hx(p[4]);
                        p += 4;
                        if (cp >= 0xD800u && cp <= 0xDBFFu && p + 6 < end && p[1] == '\\' && p[2] == 'u') {
                            uint32_t lo = (hx(p[3]) << 12) | (hx(p[4]) << 8) | (hx(p[5]) << 4) | hx(p[6]);
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                                p += 6;
                            }
                        }
                        if (cp < 0x80u) {
                            result += (char)cp;
                        } else if (cp < 0x800u) {
                            result += (char)(0xC0u | (cp >> 6));
                            result += (char)(0x80u | (cp & 0x3Fu));
                        } else if (cp < 0x10000u) {
                            result += (char)(0xE0u | (cp >> 12));
                            result += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                            result += (char)(0x80u | (cp & 0x3Fu));
                        } else {
                            result += (char)(0xF0u | (cp >> 18));
                            result += (char)(0x80u | ((cp >> 12) & 0x3Fu));
                            result += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                            result += (char)(0x80u | (cp & 0x3Fu));
                        }
                    }
                    break;
                }
                default: result += *p; break;
            }
            p++;
        } else {
            result += *p++;
        }
    }
    if (p < end && *p == '"') p++; // skip closing quote
    return result;
}

// Find a top-level "key" inside [obj, end) and return a pointer just past its
// ':' (and any following whitespace), or nullptr if not found / not at depth 0.
inline const char *find_top_level_key(const char *obj, const char *end, const char *key) {
    size_t klen = strlen(key);
    int depth = 0;
    bool in_str = false;
    const char *p = obj;
    while (p < end) {
        char c = *p;
        if (in_str) {
            if (c == '\\') { p += 2; continue; }
            if (c == '"') { in_str = false; p++; continue; }
            p++;
            continue;
        }
        if (c == '"') {
            if (depth == 1 && (size_t)(end - p) > klen + 2 &&
                memcmp(p + 1, key, klen) == 0 && p[klen + 1] == '"' && p[klen + 2] == ':') {
                const char *v = p + klen + 3;
                while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
                return v;
            }
            in_str = true; p++; continue;
        }
        if (c == '{' || c == '[') { depth++; p++; continue; }
        if (c == '}' || c == ']') { depth--; p++; continue; }
        p++;
    }
    return nullptr;
}

inline std::string json_str_field(const char *obj, const char *end, const char *key) {
    const char *v = find_top_level_key(obj, end, key);
    if (!v || v >= end || *v != '"') return {};
    return decode_json_string(v, end);
}

inline long long json_int_field(const char *obj, const char *end, const char *key, long long def = 0) {
    const char *v = find_top_level_key(obj, end, key);
    if (!v) return def;
    bool neg = false;
    if (v < end && *v == '-') { neg = true; v++; }
    if (v >= end || *v < '0' || *v > '9') return def;
    long long t = 0;
    while (v < end && *v >= '0' && *v <= '9') { t = t * 10 + (*v++ - '0'); }
    return neg ? -t : t;
}

inline bool json_bool_field(const char *obj, const char *end, const char *key, bool def = false) {
    const char *v = find_top_level_key(obj, end, key);
    if (!v) return def;
    if (v + 4 <= end && memcmp(v, "true", 4) == 0) return true;
    if (v + 5 <= end && memcmp(v, "false", 5) == 0) return false;
    return def;
}

inline bool json_has_key(const char *obj, const char *end, const char *key) {
    return find_top_level_key(obj, end, key) != nullptr;
}

// Bounds of a "key": value where value is an object or array (start points at
// '{'/'[', end points just past the matching '}'/']').
inline bool json_value_bounds(const char *obj, const char *end, const char *key,
                                const char **out_s, const char **out_e) {
    const char *v = find_top_level_key(obj, end, key);
    if (!v || v >= end) return false;
    *out_s = v;
    *out_e = skip_json_value(v, end);
    return true;
}

// Iterates top-level "key": value entries of an object whose bounds are
// [obj, end) (obj must point at the opening '{'). Advances *cursor each call.
// Returns false when the object is exhausted. Used for maps keyed by dynamic
// IDs (room IDs under rooms.join, user IDs under state.members) where the
// key names themselves are the data we want, not fixed field names.
struct ObjectEntry {
    std::string key;
    const char *val_s;
    const char *val_e;
};

inline bool next_object_entry(const char **cursor, const char *end, ObjectEntry &out) {
    const char *p = *cursor;
    // On first call *cursor points at '{'; skip it once.
    if (p < end && *p == '{') p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
    if (p >= end || *p == '}') { *cursor = p; return false; }
    if (*p != '"') { *cursor = p; return false; }
    out.key = decode_json_string(p, end);
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p < end && *p == ':') p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    out.val_s = p;
    out.val_e = skip_json_value(p, end);
    *cursor = out.val_e;
    return true;
}

// Iterates elements of an array whose bounds are [arr, end) (arr points at
// the opening '['). Advances *cursor each call; returns false when exhausted.
inline bool next_array_element(const char **cursor, const char *end, const char **out_s, const char **out_e) {
    const char *p = *cursor;
    if (p < end && *p == '[') p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
    if (p >= end || *p == ']') { *cursor = p; return false; }
    *out_s = p;
    *out_e = skip_json_value(p, end);
    *cursor = *out_e;
    return true;
}

// Escapes a plain UTF-8 string for embedding as a JSON string literal value
// (caller supplies the surrounding quotes).
inline std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += (char)c;
        }
    }
    return out;
}

} // namespace Matrix
