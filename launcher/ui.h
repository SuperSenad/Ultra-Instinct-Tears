// Ultra Instinct Tears launcher: drawing helpers on top of a Dear ImGui draw list (palette, text, glyphs, controls).
#pragma once
#include "imgui.h"
#include "mini.h"

namespace ut {

namespace pal {   // restrained: slate surfaces, one teal for focus/interaction, one gold for Play/recommended
constexpr ImU32 bg       = IM_COL32(14, 19, 21, 255);
constexpr ImU32 panel    = IM_COL32(20, 27, 30, 255);
constexpr ImU32 raised   = IM_COL32(27, 37, 40, 255);
constexpr ImU32 focus    = IM_COL32(31, 44, 47, 255);
constexpr ImU32 line     = IM_COL32(40, 53, 55, 255);
constexpr ImU32 text     = IM_COL32(232, 228, 217, 255);
constexpr ImU32 body     = IM_COL32(196, 200, 192, 255);
constexpr ImU32 dim      = IM_COL32(143, 155, 150, 255);
constexpr ImU32 faint    = IM_COL32(98, 112, 108, 255);
constexpr ImU32 teal     = IM_COL32(111, 183, 168, 255);
constexpr ImU32 tealSoft = IM_COL32(111, 183, 168, 46);
constexpr ImU32 gold     = IM_COL32(205, 174, 106, 255);
constexpr ImU32 goldSoft = IM_COL32(205, 174, 106, 40);
constexpr ImU32 ink      = IM_COL32(20, 22, 20, 255);     // text on gold
}  // namespace pal

ImU32 alpha(ImU32 c, float a);
ImU32 mix(ImU32 a, ImU32 b, float t);

struct Draw {
    ImDrawList *dl = nullptr;
    ImFont *reg = nullptr, *semi = nullptr;
    float s = 1;                                  // layout scale: design units -> pixels

    float px(float v) const { return v * s; }
    float fs(float v) const { return (float)(int)(v * s + 0.5f); }     // font size in whole pixels
    ImVec2 measure(ImFont *f, float size, const mini::string &t, float wrap = 0) const;
    // Draws text; returns its height. wrap > 0 wraps at that width.
    float text(ImFont *f, float size, ImVec2 p, ImU32 c, const mini::string &t, float wrap = 0) const;
    void text_fit(ImFont *f, float size, ImVec2 p, ImU32 c, const mini::string &t, float maxw, bool keep_end = false) const;
    void text_spaced(ImFont *f, float size, ImVec2 p, ImU32 c, const mini::string &t, float spacing) const;
    float spaced_width(ImFont *f, float size, const mini::string &t, float spacing) const;
    void rect(ImVec2 a, ImVec2 b, ImU32 c, float r = 0) const { dl->AddRectFilled(a, b, c, r); }
    void outline(ImVec2 a, ImVec2 b, ImU32 c, float r = 0, float t = 1) const { dl->AddRect(a, b, c, r, 0, t); }
    void check(ImVec2 c, float size, ImU32 col) const;
    void cross(ImVec2 c, float size, ImU32 col) const;
    void chevron(ImVec2 c, float size, ImU32 col, int dir) const;      // 0 right, 1 down, 2 left, 3 up
    void reset_icon(ImVec2 c, float r, ImU32 col) const;
};

}  // namespace ut
