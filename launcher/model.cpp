// Ultra Instinct Tears launcher: settings.ini schema and ultrainstincttears.cfg reading/writing. See model.h.
#include "model.h"
#include "../src/host/input_map.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ut {

static mini::string trim(const mini::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

static mini::vector<mini::string> split(const mini::string &s, char c) {
    mini::vector<mini::string> out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find(c, p);
        out.push_back(s.substr(p, q == mini::string::npos ? mini::string::npos : q - p));
        if (q == mini::string::npos) return out;
        p = q + 1;
    }
}

mini::vector<Setting> parse_schema(const mini::string &text, mini::string *error) {
    mini::vector<Setting> out;
    Setting *cur = nullptr;
    int n = 0;
    for (const mini::string &raw : split(text, '\n')) {
        n++;
        mini::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            out.emplace_back();
            cur = &out.back();
            cur->key = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (!cur || eq == mini::string::npos) {
            if (error) *error = "settings.ini line " + mini::to_string(n);
            continue;
        }
        mini::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        Setting &s = *cur;
        if (k == "name") s.name = v;
        else if (k == "tab") s.tab = v;
        else if (k == "section") s.section = v;
        else if (k == "group") {
            s.groupFolded = !v.empty() && v.back() == '+';
            s.group = s.groupFolded ? v.substr(0, v.size() - 1) : v;
        } else if (k == "type") {
            s.type = v == "choice" ? Type::Choice : v == "int" ? Type::Int : v == "float" ? Type::Float
                   : v == "text" ? Type::Text : v == "pad" ? Type::Pad : v == "key" ? Type::Key : Type::Bool;
            if (s.type == Type::Bool && s.def == "-") s.def = "off";
        } else if (k == "default") s.def = v;
        else if (k == "on") s.on = v;
        else if (k == "off") s.off = v;
        else if (k == "choices") {
            for (const mini::string &c : split(v, '|')) {
                size_t colon = c.find(':');
                if (colon == mini::string::npos) s.choices.push_back({c, c});
                else s.choices.push_back({c.substr(0, colon), c.substr(colon + 1)});
            }
        } else if (k == "min") s.min = atof(v.c_str());
        else if (k == "max") s.max = atof(v.c_str());
        else if (k == "step") s.step = atof(v.c_str());
        else if (k == "unit") s.unit = v;
        else if (k == "auto") s.autoLabel = v;
        else if (k == "zero") s.zero = v;
        else if (k == "hint") s.hint = v;
        else if (k == "summary") s.summary = v;
        else if (k == "detail") s.detail = v;
        else if (k == "tradeoff") s.tradeoff = v;
        else if (k == "tip") s.tip = v;
        else if (k == "alias") s.alias = v;
        else if (k == "lift") s.lift = v;
        else if (k == "ask") s.ask = v == "1";
        else if (k == "applies") s.applies = v == "live" ? Applies::Live : v == "setup" ? Applies::Setup : v == "launcher" ? Applies::Launcher : Applies::Restart;
        else if (k == "recommended") { s.recommended = v; s.hasRecommended = true; }
        else if (k == "needs") {
            size_t e2 = v.find('=');
            s.needsKey = v.substr(0, e2);
            if (e2 != mini::string::npos) s.needsVals = split(v.substr(e2 + 1), ',');
        }
    }
    return out;
}

// ---------------------------------------------------------------- [UIT 0070] lift-features.ini

LiftFeatures parse_lift_features(const mini::string &text) {
    LiftFeatures f;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == mini::string::npos) e = text.size();
        mini::string line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t q = line.find('=');
        if (q == mini::string::npos) continue;
        f.kv[line.substr(0, q)] = line.substr(q + 1);
        f.known = true;
    }
    return f;
}

mini::string lift_unavailable(const Setting &s, const LiftFeatures &f) {
    if (s.lift.empty()) return {};
    if (!f.known)
        return "Unavailable: this build's game code was translated before the feature scan (run tools/setup.py again)";
    auto v = f.kv.find(s.lift);
    if (v != f.kv.end() && v->second == "1") return {};
    mini::string note;
    auto n = f.kv.find(s.lift + "_note");
    if (n != f.kv.end()) note = n->second;
    if (s.lift == "shadow") {
        auto p = f.kv.find("shadow_probes");
        if (p != f.kv.end() && !p->second.empty() && p->second != "0")
            return "Not confirmed yet: the scan found " + p->second + " possible places. Play a minute outdoors with "
                   "Advanced > Troubleshooting > Shadow-size check on, quit, then run tools/setup.py again";
    }
    return "Unavailable in this build: " + (note.empty() ? mini::string("the game code was not found") : note);
}

// ---------------------------------------------------------------- ultrainstincttears.cfg

static mini::string line_key(const mini::string &line) {
    if (line.empty() || line[0] == '#') return {};
    size_t eq = line.find('=');
    if (eq == mini::string::npos) return {};
    mini::string k = line.substr(0, eq);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();   // be lenient: "KEY = v"
    return k;
}

static mini::string line_value(const mini::string &line) {
    mini::string v = line.substr(line.find('=') + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    return v;
}

bool CfgFile::load(const mini::string &p) {
    path = p;
    lines.clear();
    size_t size = 0;
    char *data = (char *)SDL_LoadFile(p.c_str(), &size);
    exists = data != nullptr;
    if (!data) { crlf = true; return false; }
    mini::string text(data, size);
    SDL_free(data);
    crlf = text.find("\r\n") != mini::string::npos || text.find('\n') == mini::string::npos;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB) text.erase(0, 3);
    for (mini::string l : split(text, '\n')) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();   // the final newline
    return true;
}

bool CfgFile::save(mini::string *error) const {
    mini::string text;
    for (const mini::string &l : lines) { text += l; text += crlf ? "\r\n" : "\n"; }
    mini::string tmp = path + ".tmp";
    if (!SDL_SaveFile(tmp.c_str(), text.data(), text.size())) {
        if (error) *error = SDL_GetError();
        return false;
    }
    if (!SDL_RenamePath(tmp.c_str(), path.c_str())) {   // replaces the old file (MoveFileEx on Windows)
        if (error) *error = SDL_GetError();
        SDL_RemovePath(tmp.c_str());
        return false;
    }
    return true;
}

mini::optional<mini::string> CfgFile::get(const mini::string &key) const {
    for (const mini::string &l : lines)
        if (line_key(l) == key) return line_value(l);
    return mini::nullopt;
}

static const char *kUserHeader = "# Your settings (kept when tools/setup.py runs again):";

void CfgFile::set(const mini::string &key, const mini::optional<mini::string> &value) {
    bool done = false;
    for (size_t i = 0; i < lines.size();) {
        if (line_key(lines[i]) == key) {
            if (value && !done) { lines[i] = key + "=" + *value; done = true; i++; }
            else lines.erase(lines.begin() + i);
            continue;
        }
        i++;
    }
    if (!value || done) return;
    if (key.rfind("TOTK_", 0) == 0) {
        bool header = false;
        for (const mini::string &l : lines) header |= l.rfind("# Your settings", 0) == 0;
        if (!header) {
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            lines.push_back(kUserHeader);
        }
    }
    lines.push_back(key + "=" + *value);
}

// ---------------------------------------------------------------- values

static bool parse_num(const mini::string &t, double *out) {
    const char *s = t.c_str();
    char *end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ') end++;
    if (*end) return false;
    *out = v;
    return true;
}

static int choice_index(const Setting &s, const mini::string &v) {
    for (size_t i = 0; i < s.choices.size(); i++)
        if (s.choices[i].value == v) return (int)i;
    return -1;
}

Value default_value(const Setting &s) { return read_value(s, mini::nullopt); }

Value read_value(const Setting &s, const mini::optional<mini::string> &raw) {
    Value v;
    switch (s.type) {
    case Type::Bool:
        if (!raw) v.b = s.def == "on";
        else if (s.off != "-") v.b = *raw != s.off;
        else v.b = true;                         // a presence flag: any value turns it on
        break;
    case Type::Choice: {
        int i = raw ? choice_index(s, *raw) : choice_index(s, "-");
        if (!raw && i < 0) i = choice_index(s, s.def);
        if (i < 0 && raw) { v.kind = Value::Custom; v.text = *raw; i = 0; }
        v.index = i < 0 ? 0 : i;
        break;
    }
    case Type::Int:
    case Type::Float:
        if (!raw) {
            if (!s.autoLabel.empty() || s.def == "-") v.kind = Value::Auto, v.num = s.min;
            else v.num = atof(s.def.c_str());
        } else if (!parse_num(*raw, &v.num)) {
            v.kind = Value::Custom, v.text = *raw;
        }
        break;
    case Type::Text:
        if (raw) v.text = *raw;
        else if (s.def != "-") v.text = s.def;
        break;
    case Type::Pad:
    case Type::Key: {
        mini::string t = raw ? *raw : s.def;
        bool pad = s.type == Type::Pad;
        mini::vector<mini::string> l = bind_list(t, pad);
        if (l.empty() && raw && trim(*raw) != "none" && !trim(*raw).empty()) l = bind_list(s.def, pad);   // unreadable: the game uses the default
        v.text = bind_join(l);
        break;
    }
    }
    return v;
}

mini::string fmt_num(const Setting &s, double v) {
    char buf[64];
    if (s.type == Type::Int) snprintf(buf, sizeof buf, "%lld", (long long)llround(v));
    else {
        snprintf(buf, sizeof buf, "%.3f", v);
        mini::string t = buf;
        while (t.back() == '0') t.pop_back();
        if (t.back() == '.') t.pop_back();
        return t;
    }
    return buf;
}

mini::optional<mini::string> write_value(const Setting &s, const Value &v) {
    if (v.kind == Value::Custom) return v.text;
    switch (s.type) {
    case Type::Bool: {
        if (v.b == (s.def == "on")) return mini::nullopt;     // the default: keep the file clean
        const mini::string &w = v.b ? s.on : s.off;
        if (w == "-") return mini::nullopt;
        return w;
    }
    case Type::Choice: {
        const mini::string &w = s.choices[v.index].value;
        if (w == "-" || (w == s.def && choice_index(s, "-") < 0)) return mini::nullopt;
        return w;
    }
    case Type::Int:
    case Type::Float:
        if (v.kind == Value::Auto) return mini::nullopt;
        if (s.def != "-" && s.autoLabel.empty() && mini::fabs(v.num - atof(s.def.c_str())) < 1e-9) return mini::nullopt;
        return fmt_num(s, v.num);
    case Type::Text:
        if (v.text.empty() || v.text == s.def) return mini::nullopt;
        return v.text;
    case Type::Pad:
    case Type::Key: {
        bool pad = s.type == Type::Pad;
        mini::string t = bind_join(bind_list(v.text, pad));
        if (t == bind_join(bind_list(s.def, pad))) return mini::nullopt;
        return t.empty() ? mini::string("none") : t;          // cleared: "none" (an empty value would mean the default)
    }
    }
    return mini::nullopt;
}

mini::string token(const Setting &s, const Value &v) {
    if (v.kind == Value::Custom) return v.text;
    switch (s.type) {
    case Type::Bool: return v.b ? s.on : s.off;
    case Type::Choice: {
        const mini::string &w = s.choices[v.index].value;
        return w;
    }
    default: {
        auto w = write_value(s, v);
        return w ? *w : "-";
    }
    }
}

mini::string display(const Setting &s, const Value &v) {
    if (v.kind == Value::Custom) return "Custom: " + v.text;
    switch (s.type) {
    case Type::Bool: return v.b ? "On" : "Off";
    case Type::Choice: return s.choices[v.index].label;
    case Type::Int:
    case Type::Float: {
        if (v.kind == Value::Auto) {
            mini::string a = s.autoLabel.empty() ? "Auto" : s.autoLabel;
            size_t p = a.find(" (");
            return p == mini::string::npos ? a : a.substr(0, p);
        }
        if (v.num == 0 && !s.zero.empty()) return s.zero;
        mini::string t = fmt_num(s, v.num);
        if (!s.unit.empty()) t += " " + s.unit;
        return t;
    }
    case Type::Text: return v.text.empty() ? "Not set" : v.text;
    case Type::Pad:
    case Type::Key: {
        mini::vector<mini::string> l = bind_list(v.text, s.type == Type::Pad);
        if (l.empty()) return "Not bound";
        mini::string out;
        for (size_t k = 0; k < l.size(); k++) {
            if (k) out += " or ";
            out += s.type == Type::Pad ? pad_label(l[k]) : key_label(l[k]);
        }
        return out;
    }
    }
    return {};
}

mini::string display_token(const Setting &s, const mini::string &tok) {
    mini::optional<mini::string> raw;
    if (tok != "-") raw = tok;
    if (s.type == Type::Bool) {
        if (tok == "on") return "On";
        if (tok == "off") return "Off";
    }
    if (s.type == Type::Choice && tok == "-" && choice_index(s, "-") < 0) raw = s.def;
    return display(s, read_value(s, raw));
}

// ---------------------------------------------------------------- bindings

PadFamily pad_family = PadFamily::Generic;

mini::vector<mini::string> bind_list(const mini::string &v, bool pad) {
    mini::vector<mini::string> out;
    for (const mini::string &part : split(v, ',')) {
        mini::string t = trim(part);
        while (!t.empty() && t[0] == ' ') t.erase(0, 1);
        if (t.empty()) continue;
        mini::string c;
        if (pad) {
            int p = uit_pad_find(t.c_str(), t.size());
            if (p >= 0) c = kUitPadInputs[p].token;
        } else {
            int k = uit_key_find(t.c_str(), t.size());
            if (k >= 0 && !kUitKeys[k].stick) c = kUitKeys[k].token;
        }
        if (!c.empty() && mini::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

mini::string bind_join(const mini::vector<mini::string> &l) {
    if (l.empty()) return "none";
    mini::string out;
    for (size_t k = 0; k < l.size(); k++) {
        if (k) out += ",";
        out += l[k];
    }
    return out;
}

mini::string pad_label(const mini::string &t) {
    // position-named tokens -> the label printed on each family's controller
    struct L { const char *tok, *gen, *xbox, *ps, *nin; };
    static const L k[] = {
        {"south", "Bottom button", "A", "Cross", "B"},           {"east", "Right button", "B", "Circle", "A"},
        {"west", "Left button", "X", "Square", "Y"},             {"north", "Top button", "Y", "Triangle", "X"},
        {"leftshoulder", "L shoulder", "LB", "L1", "L"},         {"rightshoulder", "R shoulder", "RB", "R1", "R"},
        {"lefttrigger", "L trigger", "LT", "L2", "ZL"},          {"righttrigger", "R trigger", "RT", "R2", "ZR"},
        {"start", "Start", "Menu", "Options", "+"},              {"back", "Back", "View", "Create", "-"},
        {"guide", "Guide", "Xbox", "PS", "Home"},                {"leftstick", "L stick press", "LS", "L3", "L stick"},
        {"rightstick", "R stick press", "RS", "R3", "R stick"},  {"dpup", "D-pad up", "D-pad up", "D-pad up", "D-pad up"},
        {"dpdown", "D-pad down", "D-pad down", "D-pad down", "D-pad down"},
        {"dpleft", "D-pad left", "D-pad left", "D-pad left", "D-pad left"},
        {"dpright", "D-pad right", "D-pad right", "D-pad right", "D-pad right"},
        {"misc1", "Share", "Share", "Mic", "Capture"},           {"touchpad", "Touchpad", "Touchpad", "Touchpad", "Touchpad"},
        {"rightpaddle1", "Paddle P1", "P1", "Paddle R4", "Paddle"}, {"leftpaddle1", "Paddle P3", "P3", "Paddle L4", "Paddle"},
        {"rightpaddle2", "Paddle P2", "P2", "Paddle R5", "Paddle"}, {"leftpaddle2", "Paddle P4", "P4", "Paddle L5", "Paddle"},
    };
    for (const L &l : k)
        if (t == l.tok)
            return pad_family == PadFamily::Xbox ? l.xbox : pad_family == PadFamily::PlayStation ? l.ps
                 : pad_family == PadFamily::Nintendo ? l.nin : l.gen;
    return t;
}

mini::string pad_label_long(const mini::string &t) {
    mini::string l = pad_label(t);
    if (pad_family == PadFamily::Generic) return l;
    const char *where = t == "south" ? "bottom" : t == "east" ? "right" : t == "west" ? "left" : t == "north" ? "top" : nullptr;
    return where ? l + " (" + where + " button)" : l;
}

mini::string key_label(const mini::string &t) {
    int k = uit_key_find(t.c_str(), t.size());
    return k < 0 ? t : mini::string(kUitKeys[k].label);
}

bool same(const Setting &s, const Value &a, const Value &b) {
    return write_value(s, a) == write_value(s, b);
}

// ---------------------------------------------------------------- [UIT 0251] settings search

mini::string search_norm(const mini::string &s) {
    mini::string o = s;
    for (auto &c : o) {
        unsigned char u = (unsigned char)c;
        if (u >= 'A' && u <= 'Z') c = (char)(u + 32);
        else if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u >= 0x80)) c = ' ';
    }
    return o;
}

static mini::string search_compact(const mini::string &n) {
    mini::string o;
    for (char c : n) if (c != ' ') o += c;
    return o;
}

mini::vector<mini::string> search_words(const mini::string &query) {
    mini::vector<mini::string> w;
    mini::string n = search_norm(query), cur;
    for (char c : n + " ") {
        if (c == ' ') {
            if (!cur.empty() && mini::find(w.begin(), w.end(), cur) == w.end()) w.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    return w;
}

SearchText search_text_name(const mini::string &name) {
    SearchText t;
    t.name = search_norm(name), t.name_c = search_compact(t.name);
    return t;
}

SearchText search_text(const Setting &s) {
    SearchText t = search_text_name(s.name);
    t.summary = search_norm(s.summary), t.summary_c = search_compact(t.summary);
    mini::string r = s.detail + " | " + s.tradeoff + " | " + s.tip + " | " + s.hint + " | " + s.key + " | " + s.alias + " | " + s.group +
                     " | " + s.section + " | " + s.unit + " | " + s.autoLabel;
    for (const Choice &c : s.choices) r += " | " + c.label;
    t.rest = search_norm(r), t.rest_c = search_compact(t.rest);
    t.key = " " + search_norm(s.key) + " ";
    return t;
}

static bool search_in(const mini::string &n, const mini::string &c, const mini::string &w) {
    return n.find(w) != mini::string::npos || c.find(w) != mini::string::npos;
}

int search_score(const SearchText &t, const mini::vector<mini::string> &words) {
    if (words.empty()) return 0;
    int tier = 3;   // 3 all in the name (or whole words of the cfg key: "fps" finds TOTK_FPS), 2 name + summary, 1 anywhere
    mini::string joined;
    for (const mini::string &w : words) {
        joined += (joined.empty() ? "" : " ") + w;
        if (search_in(t.name, t.name_c, w) || t.key.find(" " + w + " ") != mini::string::npos) continue;
        if (search_in(t.summary, t.summary_c, w)) { tier = mini::min(tier, 2); continue; }
        if (search_in(t.rest, t.rest_c, w)) { tier = 1; continue; }
        return 0;
    }
    int score = tier * 4;
    if (tier == 3) {
        size_t p = t.name.find(words[0]);
        bool start = false;
        while (p != mini::string::npos && !start) {
            start = p == 0 || t.name[p - 1] == ' ';
            p = t.name.find(words[0], p + 1);
        }
        score += start;
        // the whole cfg key (with or without TOTK_), e.g. "fps", "totk_fps", "TOTK FPS": that setting first
        if (t.key == " " + joined + " " || t.key == " totk " + joined + " ") score += 2;
    }
    return score;
}

mini::vector<mini::pair<int, int>> search_spans(const mini::string &text, const mini::vector<mini::string> &words) {
    mini::string n = search_norm(text);
    mini::vector<mini::pair<int, int>> sp;
    for (const mini::string &w : words)
        for (size_t p = n.find(w); p != mini::string::npos; p = n.find(w, p + 1)) {
            mini::pair<int, int> r((int)p, (int)(p + w.size()));
            size_t k = 0;   // insert sorted by start
            while (k < sp.size() && sp[k].first <= r.first) k++;
            sp.insert(sp.begin() + k, r);
        }
    mini::vector<mini::pair<int, int>> out;   // merge overlapping / touching ranges
    for (auto &r : sp) {
        if (!out.empty() && r.first <= out.back().second) out.back().second = mini::max(out.back().second, r.second);
        else out.push_back(r);
    }
    return out;
}

}  // namespace ut
