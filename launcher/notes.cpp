// Ultra Instinct Tears launcher: release notes as blocks ([UIT 0377]). See notes.h.
#include "notes.h"

namespace ut {

static void put_utf8(mini::string &o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// One UTF-8 sequence at s[i]: its length (0 = not valid) and code point.
static size_t utf8_one(const mini::string &s, size_t i, unsigned *cp) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { *cp = c; return 1; }
    size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!n || i + n > s.size()) return 0;
    unsigned v = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (size_t k = 1; k < n; k++) {
        unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) return 0;
        v = (v << 6) | (d & 0x3F);
    }
    if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}

mini::string notes_utf8_fix(const mini::string &s) {
    static const unsigned short k1252[32] = {   // 0x80..0x9F; 0 = unused (dropped)
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    mini::string o;
    for (size_t i = 0; i < s.size();) {
        unsigned cp;
        size_t n = utf8_one(s, i, &cp);
        if (n) { o.append(s.c_str() + i, n); i += n; continue; }
        unsigned char c = (unsigned char)s[i++];
        unsigned w = c >= 0xA0 ? c : k1252[c - 0x80];
        if (w) put_utf8(o, w);
    }
    return o;
}

const char *notes_ascii_for(unsigned cp) {
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x2039: case 0x203A: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB: return "\"";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: case 0x2023: case 0x25CF: case 0x25AA: return "*";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x2194: return "<->";
    case 0x21D2: return "=>";
    case 0x2191: return "up";
    case 0x2193: return "down";
    case 0x2248: return "~";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2260: return "!=";
    case 0x00D7: return "x";
    case 0x2122: return "(TM)";
    case 0x00A9: return "(c)";
    case 0x00AE: return "(R)";
    case 0x2713: case 0x2714: return "ok";
    case 0x2717: case 0x2718: return "x";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xFE0F: return "";
    default: return nullptr;
    }
}

mini::string notes_glyph_safe(const mini::string &s0, bool (*has)(unsigned, void *), void *ctx) {
    mini::string s = notes_utf8_fix(s0), o;
    for (size_t i = 0; i < s.size();) {
        unsigned cp;
        size_t n = utf8_one(s, i, &cp);
        if (!n) { i++; continue; }   // cannot happen after notes_utf8_fix
        if (cp < 0x80 || (has && has(cp, ctx))) o.append(s.c_str() + i, n);
        else if (const char *a = notes_ascii_for(cp)) o += a;
        i += n;
    }
    return o;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static bool is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static mini::string trim(const mini::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) a++;
    while (b > a && is_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static bool starts(const mini::string &s, const char *p) {
    size_t n = 0;
    while (p[n]) n++;
    return s.size() >= n && s.compare(0, n, mini::string(p)) == 0;
}

mini::string notes_inline(const mini::string &s) {
    mini::string o;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && !is_word(s[i + 1]) && !is_space(s[i + 1])) { o += s[i + 1]; i += 2; continue; }
        if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {   // an image: dropped
            size_t cb = s.find(']', i + 2);
            if (cb != mini::string::npos && cb + 1 < s.size() && s[cb + 1] == '(') {
                size_t cp = s.find(')', cb + 2);
                if (cp != mini::string::npos) { i = cp + 1; continue; }
            }
        }
        if (c == '[') {   // [text](url): the text
            size_t cb = s.find(']', i + 1);
            if (cb != mini::string::npos && cb + 1 < s.size() && s[cb + 1] == '(') {
                size_t cp = s.find(')', cb + 2);
                if (cp != mini::string::npos) { o += notes_inline(s.substr(i + 1, cb - i - 1)); i = cp + 1; continue; }
            }
        }
        if (c == '<' && i + 1 < s.size() && (is_alpha(s[i + 1]) || s[i + 1] == '/')) {   // <https://...> or an HTML tag
            size_t gt = s.find('>', i + 1), lt = s.find('<', i + 1);
            if (gt != mini::string::npos && (lt == mini::string::npos || gt < lt)) {
                mini::string in = s.substr(i + 1, gt - i - 1);
                if (starts(in, "http://") || starts(in, "https://")) o += in;
                else o += ' ';
                i = gt + 1;
                continue;
            }
        }
        if (c == '`') { i++; continue; }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) { i += 2; continue; }   // ** or __
        if (c == '*') {   // *emphasis*: the star goes when it opens or closes a word
            bool open = i + 1 < s.size() && !is_space(s[i + 1]) && (i == 0 || !is_word(s[i - 1]));
            bool close = i > 0 && !is_space(s[i - 1]) && (i + 1 >= s.size() || !is_word(s[i + 1]));
            if (open || close) { i++; continue; }
        }
        o += c;
        i++;
    }
    mini::string r;   // spaces collapsed
    for (char c : o) {
        if (is_space(c)) { if (!r.empty() && r.back() != ' ') r += ' '; }
        else r += c;
    }
    return trim(r);
}

static bool is_checksum_line(const mini::string &t) {   // a 64-digit hex run: the SHA-256 line, no use to the player
    size_t run = 0;
    for (size_t i = 0; i <= t.size(); i++) {
        char c = i < t.size() ? t[i] : ' ';
        bool hx = is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (hx) run++;
        else { if (run == 64) return true; run = 0; }
    }
    return false;
}

mini::vector<NoteBlock> notes_parse(const mini::string &md0) {
    mini::string md = notes_utf8_fix(md0), s;
    for (size_t i = 0; i < md.size();) {   // HTML comments out (an unclosed one runs to the end)
        if (md.compare(i, 4, mini::string("<!--")) == 0) {
            size_t e = md.find("-->", i + 4);
            if (e == mini::string::npos) break;
            i = e + 3;
            continue;
        }
        s += md[i++];
    }
    mini::vector<NoteBlock> out;
    NoteBlock cur;
    bool open = false, fence = false;
    auto flush = [&] {
        if (!open) return;
        cur.text = notes_inline(cur.text);
        if (!cur.text.empty()) out.push_back(cur);
        cur = NoteBlock();
        open = false;
    };
    size_t a = 0;
    while (a <= s.size()) {
        size_t e = s.find('\n', a);
        if (e == mini::string::npos) e = s.size();
        mini::string line = s.substr(a, e - a);
        a = e + 1;
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) indent += 1;
        mini::string t = trim(line);
        if (starts(t, "```") || starts(t, "~~~")) { flush(); fence = !fence; continue; }
        if (fence) {   // code: each line as it is
            flush();
            if (!t.empty()) { NoteBlock b; b.text = t; out.push_back(b); }
            continue;
        }
        if (t.empty()) { flush(); continue; }
        if (is_checksum_line(t)) continue;
        size_t h = 0;
        while (h < t.size() && t[h] == '#') h++;
        if (h >= 1 && h <= 6 && h < t.size() && t[h] == ' ') {
            flush();
            mini::string x = trim(t.substr(h));
            while (!x.empty() && x.back() == '#') x.pop_back();   // closing #s
            cur.kind = NoteBlock::Heading, cur.level = (int)h, cur.text = x, open = true;
            flush();
            continue;
        }
        if (t.size() >= 3 && (t[0] == '-' || t[0] == '*' || t[0] == '_')) {   // a rule: ---, ***, ___
            bool rule = true;
            for (char c : t) if (c != t[0] && c != ' ') rule = false;
            if (rule) { flush(); continue; }
        }
        if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            flush();
            cur.kind = NoteBlock::Bullet, cur.level = indent >= 2 ? 1 : 0, cur.marker = "\xE2\x80\xA2", cur.text = t.substr(2), open = true;
            continue;
        }
        size_t d = 0;
        while (d < t.size() && d < 4 && is_digit(t[d])) d++;
        if (d >= 1 && d <= 3 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
            flush();
            cur.kind = NoteBlock::Number, cur.level = indent >= 2 ? 1 : 0, cur.marker = t.substr(0, d) + ".", cur.text = t.substr(d + 2), open = true;
            continue;
        }
        if (open) cur.text += " " + t;   // a soft line break: the same paragraph or list item
        else cur.kind = NoteBlock::Para, cur.text = t, open = true;
    }
    flush();
    return out;
}

mini::vector<NoteBlock> notes_excerpt(const mini::vector<NoteBlock> &b, size_t max_chars, size_t max_blocks, bool *cut) {
    mini::vector<NoteBlock> out;
    size_t total = 0;
    bool c = false;
    for (size_t i = 0; i < b.size(); i++) {
        size_t n = b[i].text.size();
        if (!out.empty() && (total + n > max_chars || out.size() >= max_blocks)) { c = true; break; }
        if (out.empty() && n > max_chars) {   // one long first block: up to a sentence end, else a word
            mini::string t = b[i].text.substr(0, max_chars);
            while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();   // never half a character
            if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
            size_t p = mini::string::npos;
            for (size_t q = t.size(); q-- > 1;)
                if ((t[q - 1] == '.' || t[q - 1] == '!' || t[q - 1] == '?') && t[q] == ' ') { p = q - 1; break; }
            if (p != mini::string::npos && p > max_chars / 3) t = t.substr(0, p + 1);
            else {
                size_t sp = t.rfind(" ");
                if (sp != mini::string::npos) t = t.substr(0, sp);
                while (!t.empty() && (t.back() == ',' || t.back() == ';' || t.back() == ':')) t.pop_back();
                t += "...";
            }
            NoteBlock x = b[i];
            x.text = t;
            out.push_back(x);
            c = true;
            break;
        }
        out.push_back(b[i]);
        total += n;
    }
    while (!out.empty() && out.back().kind == NoteBlock::Heading) { out.pop_back(); c = true; }
    if (cut) *cut = c;
    return out;
}

}  // namespace ut
