// [UIT 0177] The portable part of nn::swkbd (see swkbd.h). The filters and the converters follow the MIT reference's
// runtime/swkbd_win.c (Copyright 2026 Valiantsin Dzerakh, adapted from MacNativeGames games/totk/runtime/
// swkbd_host.c @ b75be8cf, MIT); the layout is switchbrew's KeyboardConfig.
#include "swkbd.h"

#include <string.h>

// KeyboardConfig (the first 0x3d4 bytes of ShowKeyboardArg)
#define KC_MODE 0x0
#define KC_OK 0x4                 // 9 UTF-16 units
#define KC_LEFT_KEY 0x16
#define KC_RIGHT_KEY 0x18
#define KC_INVALID 0x1c
#define KC_CURSOR 0x20
#define KC_HEADER 0x24            // 0x40 units
#define KC_SUB 0xa6               // 0x80 units
#define KC_GUIDE 0x1a8            // 0x100 units
#define KC_MAX 0x3ac
#define KC_MIN 0x3b0
#define KC_PASSWORD 0x3b4
#define KC_NEWLINE 0x3bc
#define KC_UTF8 0x3bd
#define KC_INIT_OFF 0x3c0         // bytes into the work buffer
#define KC_INIT_LEN 0x3c4         // UTF-16 units
#define KC_TEXT_CHECK 0x3d0
// The work buffer {u64 ptr, u64 size} after the config: 0x518 in the SDK 20 argument (TotK 1.4.x), 0x4d0 in older ones.
static const uint32_t kWorkAt[] = {0x518, 0x4d0};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void rd_text(const uint8_t *p, uint16_t *out, int units) {   // a fixed field: NUL-terminated within units
    int i = 0;
    for (; i < units; i++) {
        out[i] = rd16(p + i * 2);
        if (!out[i]) break;
    }
    out[i < units ? i : units] = 0;
}

size_t uit_swkbd_u16len(const uint16_t *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int uit_swkbd_parse(const uint8_t *arg, UitSwkbdReq *r) {
    memset(r, 0, sizeof *r);
    r->mode = rd32(arg + KC_MODE);
    r->invalid = rd32(arg + KC_INVALID);
    r->cursor_start = rd32(arg + KC_CURSOR) == 1;
    r->left_key = rd16(arg + KC_LEFT_KEY);
    r->right_key = rd16(arg + KC_RIGHT_KEY);
    rd_text(arg + KC_OK, r->ok, 9);
    rd_text(arg + KC_HEADER, r->header, 0x40);
    rd_text(arg + KC_SUB, r->sub, 0x80);
    rd_text(arg + KC_GUIDE, r->guide, 0x100);
    int32_t mx = (int32_t)rd32(arg + KC_MAX), mn = (int32_t)rd32(arg + KC_MIN);
    r->max_len = mx <= 0 || mx > UIT_SWKBD_LIMIT ? UIT_SWKBD_LIMIT : mx;
    r->min_len = mn < 0 ? 0 : mn > r->max_len ? r->max_len : mn;
    r->password = rd32(arg + KC_PASSWORD) != 0;
    r->newline = arg[KC_NEWLINE] != 0;
    r->utf8 = arg[KC_UTF8] != 0;
    r->text_check = arg[KC_TEXT_CHECK] != 0;
    // the initial text: in the work buffer, when one is where the SDK puts it (a size the SDK allocates)
    for (unsigned i = 0; i < sizeof kWorkAt / sizeof kWorkAt[0]; i++) {
        uint64_t ptr = rd64(arg + kWorkAt[i]), size = rd64(arg + kWorkAt[i] + 8);
        if (!ptr || size < 0x1000 || size > 0xd000 || (size & 0xfff)) continue;
        r->work_at = kWorkAt[i];
        uint32_t off = rd32(arg + KC_INIT_OFF), len = rd32(arg + KC_INIT_LEN);
        if (len && off < size && off % 2 == 0) {
            uint64_t room = (size - off) / 2;
            if (len > room) len = (uint32_t)room;
            if (len > UIT_SWKBD_LIMIT) len = UIT_SWKBD_LIMIT;
            const uint8_t *s = (const uint8_t *)(uintptr_t)ptr + off;
            for (uint32_t k = 0; k < len; k++) r->initial[k] = rd16(s + k * 2);
            size_t n = uit_swkbd_u16len(r->initial, len);
            r->initial_len = (int)uit_swkbd_clean(r, r->initial, n);
            r->initial[r->initial_len] = 0;
        }
        break;
    }
    return 1;
}

int uit_swkbd_allowed(const UitSwkbdReq *r, uint32_t cp) {
    if (cp == '\n') return r->newline && r->mode != UIT_KBMODE_NUMERIC;
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) return 0;   // control characters
    if (cp >= 0xd800 && cp < 0xe000) return 0;                            // a lone surrogate
    if (cp > 0x10ffff || cp == 0xfffe || cp == 0xffff) return 0;
    if (r->mode == UIT_KBMODE_NUMERIC)
        return (cp >= '0' && cp <= '9' && !(r->invalid & UIT_KBINV_NUMBERS)) ||
               (cp != 0 && (cp == r->left_key || cp == r->right_key));
    if (r->mode == UIT_KBMODE_ASCII && cp > 0x7e) return 0;
    if ((r->invalid & UIT_KBINV_SPACE) && (cp == ' ' || cp == 0x3000)) return 0;
    if ((r->invalid & UIT_KBINV_AT) && cp == '@') return 0;
    if ((r->invalid & UIT_KBINV_PERCENT) && cp == '%') return 0;
    if ((r->invalid & UIT_KBINV_SLASH) && cp == '/') return 0;
    if ((r->invalid & UIT_KBINV_BACKSLASH) && cp == '\\') return 0;
    if ((r->invalid & UIT_KBINV_NUMBERS) && cp >= '0' && cp <= '9') return 0;
    return 1;
}

size_t uit_swkbd_clean(const UitSwkbdReq *r, uint16_t *t, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp = t[i];
        size_t w = 1;
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n && t[i + 1] >= 0xdc00 && t[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (t[i + 1] - 0xdc00u);
            w = 2;
        }
        if (uit_swkbd_allowed(r, cp)) {
            if (o + w > (size_t)r->max_len) break;
            t[o] = t[i];
            if (w == 2) t[o + 1] = t[i + 1];
            o += w;
        }
        i += w;
    }
    return o;
}

size_t uit_swkbd_utf8_to_utf16(const char *src, size_t n, uint16_t *out, size_t cap) {
    const unsigned char *s = (const unsigned char *)src;
    size_t i = 0, o = 0;
    while (i < n) {
        uint32_t c = s[i], cp = 0xfffd;
        size_t need = c < 0x80 ? 0 : c >= 0xc2 && c < 0xe0 ? 1 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xf0 && c < 0xf5 ? 3 : 9;
        if (need == 0) { cp = c; i++; }
        else if (need == 9) i++;
        else {
            uint32_t v = c & (need == 1 ? 0x1f : need == 2 ? 0x0f : 0x07);
            size_t k = 1;
            for (; k <= need; k++) {
                if (i + k >= n || (s[i + k] & 0xc0) != 0x80) break;
                v = v << 6 | (s[i + k] & 0x3fu);
            }
            if (k <= need) i += k;   // cut short: one U+FFFD for what was there
            else {
                i += need + 1;
                int over = (need == 2 && v < 0x800) || (need == 3 && (v < 0x10000 || v > 0x10ffff));
                cp = over || (v >= 0xd800 && v < 0xe000) ? 0xfffd : v;
            }
        }
        if (cp >= 0x10000) {
            if (out && o + 2 > cap) break;
            if (out) { out[o] = (uint16_t)(0xd800 + ((cp - 0x10000) >> 10)); out[o + 1] = (uint16_t)(0xdc00 + (cp & 0x3ff)); }
            o += 2;
        } else {
            if (out && o + 1 > cap) break;
            if (out) out[o] = (uint16_t)cp;
            o++;
        }
    }
    return o;
}

size_t uit_swkbd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00u);
        else if (cp >= 0xd800 && cp < 0xe000) cp = 0xfffd;
        unsigned char b[4];
        size_t w;
        if (cp < 0x80) { b[0] = (unsigned char)cp; w = 1; }
        else if (cp < 0x800) { b[0] = (unsigned char)(0xc0 | cp >> 6); b[1] = (unsigned char)(0x80 | (cp & 63)); w = 2; }
        else if (cp < 0x10000) {
            b[0] = (unsigned char)(0xe0 | cp >> 12); b[1] = (unsigned char)(0x80 | (cp >> 6 & 63)); b[2] = (unsigned char)(0x80 | (cp & 63)); w = 3;
        } else {
            b[0] = (unsigned char)(0xf0 | cp >> 18); b[1] = (unsigned char)(0x80 | (cp >> 12 & 63));
            b[2] = (unsigned char)(0x80 | (cp >> 6 & 63)); b[3] = (unsigned char)(0x80 | (cp & 63)); w = 4;
        }
        if (out) {
            if (o + w > cap) break;
            memcpy(out + o, b, w);
        }
        o += w;
    }
    return o;
}

uint32_t uit_swkbd_finish(const UitSwkbdReq *r, int ok, const uint16_t *t, size_t n, void *buf, uint64_t size) {
    if (n > (size_t)r->max_len) n = (size_t)r->max_len;
    if (ok && n < (size_t)r->min_len) ok = 0;   // the applet's OK needs min_len; anything else is a cancel
    if (!ok) n = 0;
    if (buf && size) {
        if (r->utf8) {
            char *o = (char *)buf;
            size_t w = uit_swkbd_utf16_to_utf8(t, n, o, (size_t)size - 1);
            o[w] = 0;
        } else if (size >= 2) {
            uint8_t *o = (uint8_t *)buf;
            size_t room = (size_t)(size / 2) - 1;
            if (n > room) {
                n = room;
                if (n && t[n - 1] >= 0xd800 && t[n - 1] < 0xdc00) n--;   // never half a pair
            }
            for (size_t i = 0; i < n; i++) memcpy(o + i * 2, t + i, 2);   // the game's buffer may be unaligned
            uint16_t z = 0;
            memcpy(o + n * 2, &z, 2);
        } else *(uint8_t *)buf = 0;
    }
    return ok ? 0 : UIT_SWKBD_RESULT_CANCELED;
}
