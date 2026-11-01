// [UIT 0330] What the offline shader translation left out of a shader, read from its reflection (<key>.res.json).
// The Ryujinx translator removes a texture read whose bindless handle it cannot trace (its result becomes 0) and reads 0
// for a global memory access it cannot match to a storage buffer; third_party/shader_translator/Program.cs now writes
// those warnings into the reflection's "notes". This loader also binds one texture from the driver bank c2 for each
// reflected texture, so textures whose handle lives in another constant buffer, and texture arrays, are counted too.
// Self-contained (no game data, no Vulkan): tools/tests/test_xnotes.c checks it.
#pragma once
#include <stdlib.h>
#include <string.h>

// [UIT 0337] tex_type_guessed / img_fmt_guessed: the translator had no recorded type for a texture handle (sampled as 2D:
// an array, cube or 3D texture then reads wrong coordinates, layer 0 or nothing) or no format for an image (assumed
// R8G8B8A8Unorm). Its notes are Chinese; System.Text.Json writes them escaped (\u7C7B), older files may hold UTF-8.
typedef struct { int tex_removed, gmem_zero, tex_other_cb, tex_arrays, tex_type_guessed, img_fmt_guessed; } vkb_xnotes;

static inline int vkb_xnotes_count(const char *s, const char *end, const char *needle) {
    int n = 0;
    size_t l = strlen(needle);
    for (const char *p = s; (p = strstr(p, needle)) && p < end; p += l) n++;
    return n;
}

// The integer after "key": within [p, end), or dflt.
static inline long vkb_xnotes_int(const char *p, const char *end, const char *key, long dflt) {
    const char *q = strstr(p, key);
    if (!q || q >= end) return dflt;
    q += strlen(key);
    while (q < end && (*q == ' ' || *q == ':')) q++;
    return q < end ? strtol(q, NULL, 10) : dflt;
}

static inline vkb_xnotes vkb_xnotes_scan(const char *j) {
    vkb_xnotes r = {0};
    const char *end = j + strlen(j);
    r.tex_removed = vkb_xnotes_count(j, end, "Failed to find handle source for bindless");
    r.gmem_zero = vkb_xnotes_count(j, end, "storage buffer for global memory");
    r.tex_type_guessed = vkb_xnotes_count(j, end, "PC \\u7C7B\\u578B") + vkb_xnotes_count(j, end, "PC \\u7c7b\\u578b") +
                         vkb_xnotes_count(j, end, "PC \xE7\xB1\xBB\xE5\x9E\x8B");   // "PC 类型"
    r.img_fmt_guessed = vkb_xnotes_count(j, end, "PC \\u683C\\u5F0F") + vkb_xnotes_count(j, end, "PC \\u683c\\u5f0f") +
                        vkb_xnotes_count(j, end, "PC \xE6\xA0\xBC\xE5\xBC\x8F");   // "PC 格式"
    const char *t = strstr(j, "\"textures\"");
    if (!t || !(t = strchr(t, '['))) return r;
    const char *tend = strchr(t, ']');   // texture entries hold no arrays
    if (!tend) tend = end;
    for (const char *o = t; (o = strchr(o, '{')) && o < tend;) {
        const char *oe = strchr(o, '}');
        if (!oe || oe > tend) break;
        long cb = vkb_xnotes_int(o, oe, "\"cbuf_slot\"", -1), arr = vkb_xnotes_int(o, oe, "\"array\"", 1);
        if (cb != -1 && (cb & 0xffff) != 2) r.tex_other_cb++;   // low 16 bits: the texture handle's slot (Ryujinx PackSlots)
        if (arr > 1) r.tex_arrays++;
        o = oe + 1;
    }
    return r;
}
static inline int vkb_xnotes_any(vkb_xnotes r) { return r.tex_removed || r.gmem_zero || r.tex_other_cb || r.tex_arrays || r.tex_type_guessed || r.img_fmt_guessed; }
