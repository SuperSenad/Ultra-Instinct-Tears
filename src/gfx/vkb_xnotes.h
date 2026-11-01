// [UIT 0330] What the offline shader translation left out of a shader, read from its reflection (<key>.res.json).
// The Ryujinx translator removes a texture read whose bindless handle it cannot trace (its result becomes 0) and reads 0
// for a global memory access it cannot match to a storage buffer; third_party/shader_translator/Program.cs now writes
// those warnings into the reflection's "notes". This loader also binds one texture from the driver bank c2 for each
// reflected texture, so textures whose handle lives in another constant buffer, and texture arrays, are counted too.
// Self-contained (no game data, no Vulkan): tools/tests/test_xnotes.c checks it.
#pragma once
#include <stdlib.h>
#include <string.h>

typedef struct { int tex_removed, gmem_zero, tex_other_cb, tex_arrays; } vkb_xnotes;

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
static inline int vkb_xnotes_any(vkb_xnotes r) { return r.tex_removed || r.gmem_zero || r.tex_other_cb || r.tex_arrays; }
