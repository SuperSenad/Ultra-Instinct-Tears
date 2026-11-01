// Ultra Instinct Tears launcher: drawing helpers. See ui.h.
#include "ui.h"
#include <math.h>

namespace ut {

ImU32 alpha(ImU32 c, float a) {
    unsigned al = (unsigned)(((c >> IM_COL32_A_SHIFT) & 0xFF) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ImU32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float x = (float)((a >> sh) & 0xFF), y = (float)((b >> sh) & 0xFF);
        out |= (ImU32)(x + (y - x) * t + 0.5f) << sh;
    }
    return out;
}

ImVec2 Draw::measure(ImFont *f, float size, const mini::string &t, float wrap) const {
    return f->CalcTextSizeA(size, FLT_MAX, wrap, t.c_str(), t.c_str() + t.size());
}

float Draw::text(ImFont *f, float size, ImVec2 p, ImU32 c, const mini::string &t, float wrap) const {
    p.x = mini::floor(p.x), p.y = mini::floor(p.y);
    dl->AddText(f, size, p, c, t.c_str(), t.c_str() + t.size(), wrap);
    return measure(f, size, t, wrap).y;
}

// UTF-8 aware trimming so the ellipsis never splits a character.
static size_t prev_char(const mini::string &t, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)t[i] & 0xC0) == 0x80) i--;
    return i;
}

void Draw::text_fit(ImFont *f, float size, ImVec2 p, ImU32 c, const mini::string &t, float maxw, bool keep_end) const {
    if (measure(f, size, t).x <= maxw) { text(f, size, p, c, t); return; }
    const mini::string ell = "\xE2\x80\xA6";
    if (!keep_end) {
        size_t n = t.size();
        while (n > 0 && measure(f, size, t.substr(0, n) + ell).x > maxw) n = prev_char(t, n);
        text(f, size, p, c, t.substr(0, n) + ell);
    } else {
        size_t st = 0;
        while (st < t.size() && measure(f, size, ell + t.substr(st)).x > maxw) {
            st++;
            while (st < t.size() && ((unsigned char)t[st] & 0xC0) == 0x80) st++;
        }
        text(f, size, p, c, ell + t.substr(st));
    }
}

float Draw::spaced_width(ImFont *f, float size, const mini::string &t, float spacing) const {
    float w = 0;
    for (char ch : t) w += measure(f, size, mini::string(1, ch)).x + spacing;
    return t.empty() ? 0 : w - spacing;
}

void Draw::text_spaced(ImFont *f, float size, ImVec2 p, ImU32 c, const mini::string &t, float spacing) const {
    for (size_t i = 0; i < t.size();) {   // one code point at a time (a UTF-8 dot is 2 bytes)
        size_t n = 1;
        while (i + n < t.size() && ((unsigned char)t[i + n] & 0xC0) == 0x80) n++;
        mini::string one = t.substr(i, n);
        i += n;
        text(f, size, p, c, one);
        p.x += measure(f, size, one).x + spacing;
    }
}

void Draw::check(ImVec2 c, float sz, ImU32 col) const {
    ImVec2 pts[3] = {{c.x - sz * 0.45f, c.y + sz * 0.02f}, {c.x - sz * 0.12f, c.y + sz * 0.34f}, {c.x + sz * 0.48f, c.y - sz * 0.32f}};
    dl->AddPolyline(pts, 3, col, 0, px(2.0f));
}

void Draw::cross(ImVec2 c, float sz, ImU32 col) const {
    float h = sz * 0.34f;
    dl->AddLine({c.x - h, c.y - h}, {c.x + h, c.y + h}, col, px(2.0f));
    dl->AddLine({c.x - h, c.y + h}, {c.x + h, c.y - h}, col, px(2.0f));
}

void Draw::chevron(ImVec2 c, float sz, ImU32 col, int dir) const {
    float h = sz * 0.5f, w = sz * 0.28f;
    ImVec2 p[3];
    switch (dir) {
    case 0: p[0] = {c.x - w, c.y - h}; p[1] = {c.x + w, c.y}; p[2] = {c.x - w, c.y + h}; break;
    case 2: p[0] = {c.x + w, c.y - h}; p[1] = {c.x - w, c.y}; p[2] = {c.x + w, c.y + h}; break;
    case 1: p[0] = {c.x - h, c.y - w}; p[1] = {c.x, c.y + w}; p[2] = {c.x + h, c.y - w}; break;
    default: p[0] = {c.x - h, c.y + w}; p[1] = {c.x, c.y - w}; p[2] = {c.x + h, c.y + w}; break;
    }
    dl->AddPolyline(p, 3, col, 0, px(2.0f));
}

void Draw::reset_icon(ImVec2 c, float r, ImU32 col) const {
    const float pi = 3.14159265f;
    dl->PathArcTo(c, r, -pi * 0.35f, pi * 1.25f, 20);
    dl->PathStroke(col, 0, px(1.8f));
    float a = -pi * 0.35f;
    ImVec2 tip = {c.x + mini::cos(a) * r, c.y + mini::sin(a) * r};
    dl->AddTriangleFilled({tip.x - r * 0.55f, tip.y - r * 0.15f}, {tip.x + r * 0.35f, tip.y - r * 0.6f},
                          {tip.x + r * 0.25f, tip.y + r * 0.45f}, col);
}

}  // namespace ut
