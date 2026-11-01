// Ultra Instinct Tears launcher: the settings schema (settings.ini) and ultrainstincttears.cfg, with no UI.
#pragma once
#include "mini.h"

namespace ut {

enum class Type { Bool, Choice, Int, Float, Text, Pad, Key };   // Pad / Key: a button binding (src/host/input_map.h)

struct Choice { mini::string value, label; };    // value "-" = the line removed

// [UIT 0304] when a change takes effect (settings.ini "applies"; tools/check_launcher_keys.py checks it against the code)
enum class Applies { Restart, Live, Setup, Launcher };

struct Setting {
    mini::string key, name, tab, group, section, summary, detail, tradeoff, tip, hint, unit, zero, autoLabel, alias;
    mini::string def = "-", on = "1", off = "-", recommended, needsKey;
    mini::string lift;   // [UIT 0070] the lift feature it needs (data/generated/lift-features.ini: quality, fxaa, lod, shadow)
    mini::vector<mini::string> needsVals;
    mini::vector<Choice> choices;
    Type type = Type::Bool;
    double min = 0, max = 1, step = 1;
    bool hasRecommended = false, groupFolded = false;
    Applies applies = Applies::Restart;   // [UIT 0304] Live: the in-game overlay changes it too; Restart: tagged "restart"
    bool ask = false;    // [UIT 0270] ask = 1: the launcher asks for it on first open (the key absent = not asked yet)
};

mini::vector<Setting> parse_schema(const mini::string &text, mini::string *error);

// [UIT 0070] data/generated/lift-features.ini (tools/lifter/scan.py): what the build-time scan found in the player's
// own game code. name=1 when that feature's code is in this build, name_note=why not, shadow_probes=candidates.
struct LiftFeatures {
    bool known = false;                         // the file was there (a build lifted with 0070 or later)
    mini::map<mini::string, mini::string> kv;
};
LiftFeatures parse_lift_features(const mini::string &text);
// "" when the setting can work in this build; otherwise why not (the setting is greyed out with this text)
mini::string lift_unavailable(const Setting &s, const LiftFeatures &f);

// ultrainstincttears.cfg as lines: comments, blank lines, path lines and unknown keys are kept byte for byte.
// Matches src/host/boot.c: '#' lines and lines without '=' are skipped; the key is everything before '='; the first
// TOTK_ line for a key wins. Setting a key edits its first line and drops later duplicates; removing drops them all.
struct CfgFile {
    mini::string path;
    mini::vector<mini::string> lines;
    bool crlf = true, exists = false;

    bool load(const mini::string &p);
    bool save(mini::string *error) const;     // tmp file + rename
    mini::optional<mini::string> get(const mini::string &key) const;
    void set(const mini::string &key, const mini::optional<mini::string> &value);
};

// A setting's value as the launcher shows it.
struct Value {
    enum Kind { Normal, Auto, Custom } kind = Normal;
    bool b = false;          // Bool
    int index = 0;           // Choice
    double num = 0;          // Int / Float
    mini::string text;        // Text, or the raw text of a Custom value
};

Value read_value(const Setting &s, const mini::optional<mini::string> &raw);
mini::optional<mini::string> write_value(const Setting &s, const Value &v);   // nullopt = remove the line
Value default_value(const Setting &s);
mini::string token(const Setting &s, const Value &v);     // what needs= compares against ("-" = absent)
mini::string display(const Setting &s, const Value &v);   // "60 FPS", "On", "Auto", ...
mini::string display_token(const Setting &s, const mini::string &tok);  // label for a raw value/token
bool same(const Setting &s, const Value &a, const Value &b);
mini::string fmt_num(const Setting &s, double v);

// Bindings (Type::Pad / Type::Key): a value is a comma list of input_map.h tokens, "none" when unbound.
enum class PadFamily { Generic, Xbox, PlayStation, Nintendo };
extern PadFamily pad_family;                                       // names controller inputs like the connected pad
mini::vector<mini::string> bind_list(const mini::string &v, bool pad);   // the known tokens, canonical spelling, no "none"
mini::string bind_join(const mini::vector<mini::string> &l);        // "none" for an empty list
mini::string pad_label(const mini::string &token);                  // "B", "Circle", "Right button", "LT", ...
mini::string pad_label_long(const mini::string &token);             // "B (right face button)"
mini::string key_label(const mini::string &token);                  // "Left Shift", "Space", ";"

// [UIT 0251] settings search: case-insensitive, every query word must match (any order), punctuation ignored.
// search_norm lowercases ASCII and turns punctuation into spaces, keeping the length (so a match in the normalised
// name is a byte range of the name itself: the row highlights it). Each field is also kept "compact" (no spaces), so
// "pcores" finds "P-cores" and "60fps" finds "60 FPS".
struct SearchText { mini::string name, name_c, summary, summary_c, rest, rest_c, key; };   // key: " totk fps " (whole words)
mini::string search_norm(const mini::string &s);
mini::vector<mini::string> search_words(const mini::string &query);   // normalised words, empty when nothing to match
SearchText search_text(const Setting &s);                            // name; summary; detail, tip, key, group, choices
SearchText search_text_name(const mini::string &name);                // a mod: its name only
// 0 = no match; otherwise higher ranks first: 7/6 every word in the name (6 + 1 when the first word starts a word of
// the name), 4 name + summary, 2 anywhere (detail, tip, the cfg key, group, choice labels)
int search_score(const SearchText &t, const mini::vector<mini::string> &words);
// where the words occur in `text` (byte ranges [first, second), sorted, merged): the highlight
mini::vector<mini::pair<int, int>> search_spans(const mini::string &text, const mini::vector<mini::string> &words);

}  // namespace ut
