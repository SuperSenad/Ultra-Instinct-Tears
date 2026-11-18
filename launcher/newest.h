// Ultra Instinct Tears setup: which GitHub release is the newest one (the same rules as tools/update.py), without any
// library: a small JSON reader, the version order (update.py version_key), the package among a release's files
// (pick_asset) and the SHA-256 a release publishes (sha_from_text). Header only; read by launcher/installer.inc. [UIT 0435]
#pragma once
#include "mini.h"

namespace ut {

// ---- JSON: every value in one pool, children by index. Strings are UTF-8 (\uXXXX and surrogate pairs decoded); bytes
// outside ASCII pass through untouched, so release notes in any language parse. Depth is limited, nothing recurses forever.
struct JNode {
    char t = 'z';                 // z null, b bool, n number, s string, a array, o object
    mini::string s;
    double n = 0;
    bool b = false;
    mini::vector<int> kids;       // array items, or object values
    mini::vector<mini::string> keys;
};
struct Json {
    mini::vector<JNode> v;
    int root = -1;
    const char *p = nullptr, *e = nullptr;
    int depth = 0;
    bool bad = false;

    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void utf8(mini::string &o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | (c >> 12 & 63)); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
    }
    bool hex4(unsigned *out) {
        if (e - p < 4) return false;
        unsigned x = 0;
        for (int k = 0; k < 4; k++) {
            char c = p[k];
            x <<= 4;
            if (c >= '0' && c <= '9') x |= c - '0';
            else if (c >= 'a' && c <= 'f') x |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') x |= c - 'A' + 10;
            else return false;
        }
        p += 4;
        *out = x;
        return true;
    }
    bool str(mini::string *o) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { *o += *p++; continue; }
            p++;
            if (p >= e) return false;
            char c = *p++;
            switch (c) {
            case 'n': *o += '\n'; break;
            case 't': *o += '\t'; break;
            case 'r': *o += '\r'; break;
            case 'b': *o += '\b'; break;
            case 'f': *o += '\f'; break;
            case 'u': {
                unsigned x;
                if (!hex4(&x)) return false;
                if (x >= 0xD800 && x < 0xDC00) {   // a surrogate pair: the low half follows
                    unsigned y = 0;
                    if (e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        const char *save = p;
                        p += 2;
                        if (hex4(&y) && y >= 0xDC00 && y < 0xE000) x = 0x10000 + ((x - 0xD800) << 10) + (y - 0xDC00);
                        else { p = save; x = 0xFFFD; }
                    } else x = 0xFFFD;
                } else if (x >= 0xDC00 && x < 0xE000) x = 0xFFFD;
                utf8(*o, x);
                break;
            }
            default: *o += c;   // \" \\ \/ and anything else as it stands
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    int add() { v.push_back(JNode()); return (int)v.size() - 1; }
    int value() {
        ws();
        if (p >= e || bad || depth > 40) { bad = true; return -1; }
        int id = add();
        char c = *p;
        if (c == '"') { v[id].t = 's'; mini::string s; if (!str(&s)) { bad = true; return -1; } v[id].s = s; return id; }
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            v[id].t = obj ? 'o' : 'a';
            p++, depth++;
            ws();
            if (p < e && *p == (obj ? '}' : ']')) { p++, depth--; return id; }
            for (;;) {
                ws();
                mini::string key;
                if (obj) {
                    if (!str(&key)) { bad = true; return -1; }
                    ws();
                    if (p >= e || *p != ':') { bad = true; return -1; }
                    p++;
                }
                int k = value();
                if (k < 0) { bad = true; return -1; }
                v[id].kids.push_back(k);
                if (obj) v[id].keys.push_back(key);
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == (obj ? '}' : ']')) { p++; break; }
                bad = true;
                return -1;
            }
            depth--;
            return id;
        }
        if (e - p >= 4 && !SDL_strncmp(p, "true", 4)) { v[id].t = 'b', v[id].b = true; p += 4; return id; }
        if (e - p >= 5 && !SDL_strncmp(p, "false", 5)) { v[id].t = 'b'; p += 5; return id; }
        if (e - p >= 4 && !SDL_strncmp(p, "null", 4)) { p += 4; return id; }
        const char *q = p;
        while (q < e && ((*q >= '0' && *q <= '9') || *q == '-' || *q == '+' || *q == '.' || *q == 'e' || *q == 'E')) q++;
        if (q == p) { bad = true; return -1; }
        mini::string num(p, (size_t)(q - p));
        v[id].t = 'n', v[id].n = SDL_atof(num.c_str());
        p = q;
        return id;
    }
    bool parse(const mini::string &text) {
        v.clear();
        p = text.data(), e = p + text.size(), depth = 0, bad = false, root = -1;
        if (e - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   // a BOM
        root = value();
        return root >= 0 && !bad;
    }
    // the member key of object o (-1 if none)
    int get(int o, const char *key) const {
        if (o < 0 || v[o].t != 'o') return -1;
        for (size_t k = 0; k < v[o].keys.size(); k++) if (v[o].keys[k] == key) return v[o].kids[k];
        return -1;
    }
    mini::string text(int o, const char *key) const { int k = get(o, key); return k >= 0 && v[k].t == 's' ? v[k].s : mini::string(); }
    bool flag(int o, const char *key) const { int k = get(o, key); return k >= 0 && v[k].t == 'b' && v[k].b; }
    double number(int o, const char *key) const { int k = get(o, key); return k >= 0 && v[k].t == 'n' ? v[k].n : 0; }
    bool is_array(int o) const { return o >= 0 && v[o].t == 'a'; }
    bool is_object(int o) const { return o >= 0 && v[o].t == 'o'; }
};

// ---- versions: tools/update.py version_key. 1.0.0 (a release) > 1.0.0-rc1 (before it); 1.0.0-3-gabc1234 (git describe, 3
// commits after the tag) > 1.0.0; a trailing -dirty is ignored.
struct VerKey {
    bool ok = false;
    long long base[3] = {0, 0, 0};
    int kind = 1;                       // 0 before the release, 1 the release, 2 commits after it
    long long post = 0;                 // kind 2: the commit count
    struct Part { int kind; long long num; mini::string text; };
    mini::vector<Part> parts;           // kind 0: the words and numbers after the version
};
inline VerKey ver_key(const mini::string &in) {
    VerKey k;
    const char *s = in.c_str();
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == 'v') s++;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return k;
        long long x = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (*s - '0'); if (x > 100000000) x = 100000000; s++; }
        k.base[i] = x;
        if (i < 2) { if (*s != '.') return k; s++; }
    }
    mini::string rest = s;
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\n' || rest.back() == '\r' || rest.back() == '\t')) rest.pop_back();
    if (rest.size() >= 6 && rest.substr(rest.size() - 6) == "-dirty") rest = rest.substr(0, rest.size() - 6);
    k.ok = true;
    if (rest.empty()) { k.kind = 1; return k; }
    // -N-gHEX
    if (rest[0] == '-') {
        size_t i = 1;
        long long n = 0;
        while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') n = n * 10 + (rest[i++] - '0');
        if (i > 1 && i + 2 < rest.size() && rest[i] == '-' && rest[i + 1] == 'g') {
            size_t j = i + 2;
            while (j < rest.size() && ((rest[j] >= '0' && rest[j] <= '9') || (rest[j] >= 'a' && rest[j] <= 'f'))) j++;
            if (j == rest.size() && j > i + 2) { k.kind = 2, k.post = n; return k; }
        }
    }
    k.kind = 0;
    size_t i = 0;
    while (i < rest.size() && (rest[i] == '-' || rest[i] == '+')) i++;
    mini::string cur;
    for (size_t j = i; j <= rest.size(); j++) {
        if (j == rest.size() || rest[j] == '.' || rest[j] == '-') {
            if (!cur.empty()) {
                bool digits = true;
                for (char c : cur) if (c < '0' || c > '9') digits = false;
                VerKey::Part pt{digits ? 0 : 1, digits ? SDL_atoi(cur.c_str()) : 0, digits ? mini::string() : cur};
                k.parts.push_back(pt);
            }
            cur.clear();
        } else cur += rest[j];
    }
    return k;
}
inline int ver_cmp(const VerKey &a, const VerKey &b) {
    for (int i = 0; i < 3; i++) if (a.base[i] != b.base[i]) return a.base[i] < b.base[i] ? -1 : 1;
    if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
    if (a.kind == 2) return a.post == b.post ? 0 : a.post < b.post ? -1 : 1;
    if (a.kind == 0) {
        size_t n = mini::min(a.parts.size(), b.parts.size());
        for (size_t i = 0; i < n; i++) {
            const VerKey::Part &x = a.parts[i], &y = b.parts[i];
            if (x.kind != y.kind) return x.kind < y.kind ? -1 : 1;
            if (x.num != y.num) return x.num < y.num ? -1 : 1;
            int c = SDL_strcmp(x.text.c_str(), y.text.c_str());
            if (c) return c < 0 ? -1 : 1;
        }
        if (a.parts.size() != b.parts.size()) return a.parts.size() < b.parts.size() ? -1 : 1;
    }
    return 0;
}
// a version a release tag "vX.Y.Z[-suffix]" names, or "" (update.py tag_version)
inline mini::string tag_version(const mini::string &tag) {
    mini::string t = tag;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r')) t.pop_back();
    while (!t.empty() && (t[0] == ' ' || t[0] == '\n')) t = t.substr(1);
    if (t.size() < 6 || t[0] != 'v') return {};
    mini::string v = t.substr(1);
    size_t i = 0;
    for (int part = 0; part < 3; part++) {
        size_t s = i;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') i++;
        if (i == s) return {};
        if (part < 2) { if (i >= v.size() || v[i] != '.') return {}; i++; }
    }
    if (i == v.size()) return v;
    if (v[i] != '-' || i + 1 >= v.size()) return {};
    for (size_t j = i + 1; j < v.size(); j++) {
        char c = v[j];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return {};
    }
    return v;
}

// ---- files and checksums
inline mini::string lower_of(const mini::string &s) { mini::string o; for (char c : s) o += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; return o; }
inline bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
// A SHA-256 for file name in text: "<hex>  name" (sha256sum), "name: <hex>", or (need_word false) a lone hex (a .sha256 file
// of one line). need_word: release notes, where a line must name the file or say sha256. tools/update.py sha_from_text.
inline mini::string sha_from_text(const mini::string &text, const mini::string &name, bool need_word) {
    mini::vector<mini::string> hexes;
    mini::string lname = lower_of(name);
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        mini::string line = text.substr(at, nl == mini::string::npos ? mini::string::npos : nl - at);
        at = nl == mini::string::npos ? text.size() + 1 : nl + 1;
        mini::vector<mini::string> hs;
        for (size_t i = 0; i < line.size();) {
            if (!is_hex(line[i])) { i++; continue; }
            size_t j = i;
            while (j < line.size() && is_hex(line[j])) j++;
            if (j - i == 64) hs.push_back(lower_of(line.substr(i, 64)));
            i = j;
        }
        if (hs.size() != 1) continue;
        mini::string low = lower_of(line);
        if (low.find(lname) != mini::string::npos) return hs[0];
        if (low.find("sha256") != mini::string::npos || low.find("sha-256") != mini::string::npos || !need_word) hexes.push_back(hs[0]);
    }
    return hexes.size() == 1 ? hexes[0] : mini::string();
}
inline bool src_zip_name(const mini::string &n) {   // UltraInstinctTears-src*.zip, any case
    mini::string l = lower_of(n);
    const char *pre = "ultrainstincttears-src";
    size_t pl = SDL_strlen(pre);
    return l.size() >= pl + 4 && l.substr(0, pl) == pre && l.substr(l.size() - 4) == ".zip";
}

}  // namespace ut
