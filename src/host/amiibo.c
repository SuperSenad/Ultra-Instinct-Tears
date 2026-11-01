/* amiibo from files: dump parsing and the nn::nfp device state machine (see amiibo.h). [UIT 0175]
 * Written from switchbrew.org's NFC_services and Error_codes pages; no emulator code. No keys: the encrypted parts of a
 * dump are never read. */
#include "amiibo.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "amiibo_names.h"

/* ---------- dumps ---------- */
const char *uit_amiibo_error_text(int err) {
    switch (err) {
    case UIT_AMIIBO_OK: return "ok";
    case UIT_AMIIBO_EMPTY: return "the file is empty or can't be read";
    case UIT_AMIIBO_SIZE: return "wrong size: an amiibo dump is 540 bytes (532 or 572 also accepted), a .json file or a Flipper .nfc file";
    case UIT_AMIIBO_NOT_AMIIBO: return "not an amiibo: no amiibo header or ID in this file";
    case UIT_AMIIBO_BAD_JSON: return "not an amiibo: the .json has no 16-digit \"AmiiboId\"";
    }
    return "unknown error";
}

uint64_t uit_amiibo_id64(const uint8_t id[8]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | id[i];
    return v;
}

const char *uit_amiibo_known_name(const uint8_t id[8]) {
    uint64_t v = uit_amiibo_id64(id);
    size_t lo = 0, hi = sizeof kUitAmiiboNames / sizeof kUitAmiiboNames[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kUitAmiiboNames[mid].id == v) return kUitAmiiboNames[mid].name;
        if (kUitAmiiboNames[mid].id < v) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void uit_amiibo_key(const UitAmiibo *a, char *buf, size_t n) {
    size_t o = 0;
    if (!n) return;
    buf[0] = 0;
    for (int i = 0; i < a->uid_len && o + 3 < n; i++) o += (size_t)snprintf(buf + o, n - o, "%02x", a->uid[i]);
    if (o + 1 < n) buf[o++] = '-', buf[o] = 0;
    for (int i = 0; i < 8 && o + 3 < n; i++) o += (size_t)snprintf(buf + o, n - o, "%02x", a->id[i]);
}

/* Copy at most cap-1 bytes of UTF-8 without cutting a character. */
static void copy_utf8(char *dst, size_t cap, const char *src, size_t len) {
    if (len > cap - 1) {
        len = cap - 1;
        while (len && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int id_ok(const uint8_t id[8]) { return id[7] == 0x02 || id[7] == 0x03; }   /* amiibo version 2 / 3 */

/* NTAG215 serial number: 0..2, BCC0 = 0x88^0^1^2, 3..6, BCC1 = 3^4^5^6 (ISO 14443-3 cascade check bytes). */
static int serial_ok(const uint8_t *s, const uint8_t *bcc1) {
    return s[3] == (uint8_t)(0x88 ^ s[0] ^ s[1] ^ s[2]) && *bcc1 == (uint8_t)(s[4] ^ s[5] ^ s[6] ^ s[7]);
}
static void take_serial(UitAmiibo *a, const uint8_t *s) {
    memset(a->uid, 0, sizeof a->uid);
    a->uid[0] = s[0]; a->uid[1] = s[1]; a->uid[2] = s[2];
    a->uid[3] = s[4]; a->uid[4] = s[5]; a->uid[5] = s[6]; a->uid[6] = s[7];
    a->uid_len = 7;
}

/* Deterministic serial number for a JSON file without one: NXP manufacturer byte, then an FNV-1a hash of the ID. */
static void derive_serial(UitAmiibo *a) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) h = (h ^ a->id[i]) * 1099511628211ull;
    memset(a->uid, 0, sizeof a->uid);
    a->uid[0] = 0x04;
    for (int i = 1; i < 7; i++) a->uid[i] = (uint8_t)(h >> (8 * i));
    a->uid_len = 7;
}

/* ---- the small part of JSON a virtual amiibo file needs: top-level "Key": "string" ---- */
static const char *json_string(const char *s, const char *end, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = s; p + kl + 2 <= end; p++) {
        if (*p != '"' || (size_t)(end - p) < kl + 2 || memcmp(p + 1, key, kl) || p[kl + 1] != '"') continue;
        const char *q = p + kl + 2;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q >= end || *q != '"') return NULL;   /* not a string (null, number...) */
        q++;
        size_t o = 0;
        while (q < end && *q != '"') {
            unsigned c = (unsigned char)*q++;
            if (c == '\\' && q < end) {
                char e = *q++;
                if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'u' && end - q >= 4) {
                    unsigned u = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = *q++;
                        u = u << 4 | (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                    }
                    if (u >= 0x80) {   /* BMP only; surrogates come out as '?' */
                        char tmp[3]; int k = 0;
                        if (u >= 0xD800 && u < 0xE000) { tmp[k++] = '?'; }
                        else if (u < 0x800) { tmp[k++] = (char)(0xC0 | u >> 6); tmp[k++] = (char)(0x80 | (u & 0x3F)); }
                        else { tmp[k++] = (char)(0xE0 | u >> 12); tmp[k++] = (char)(0x80 | (u >> 6 & 0x3F)); tmp[k++] = (char)(0x80 | (u & 0x3F)); }
                        if (o + (size_t)k < cap) { memcpy(out + o, tmp, (size_t)k); o += (size_t)k; }
                        continue;
                    }
                    c = u;
                } else c = (unsigned char)e;
            }
            if (o + 1 < cap) out[o++] = (char)c;
        }
        if (q >= end) return NULL;
        out[o] = 0;
        return out;
    }
    return NULL;
}
static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static int b64val(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
         : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}
static int parse_json(const char *s, size_t n, UitAmiibo *a, char *jname, size_t jcap) {
    const char *end = s + n;
    char v[128];
    if (!json_string(s, end, "AmiiboId", v, sizeof v)) return UIT_AMIIBO_BAD_JSON;
    const char *h = v;
    if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
    if (strlen(h) != 16) return UIT_AMIIBO_BAD_JSON;
    for (int i = 0; i < 8; i++) {
        int x = hexval(h[2 * i]), y = hexval(h[2 * i + 1]);
        if (x < 0 || y < 0) return UIT_AMIIBO_BAD_JSON;
        a->id[i] = (uint8_t)(x << 4 | y);
    }
    if (!id_ok(a->id)) return UIT_AMIIBO_BAD_JSON;
    a->uid_len = 0;
    if (json_string(s, end, "TagUuid", v, sizeof v)) {   /* base64 of the serial number (7 bytes, maybe 10) */
        uint8_t u[16]; int nu = 0, bits = 0; unsigned acc = 0, ok = 1;
        for (const char *p = v; *p && *p != '='; p++) {
            int b = b64val(*p);
            if (b < 0) { ok = 0; break; }
            acc = acc << 6 | (unsigned)b; bits += 6;
            if (bits >= 8) { bits -= 8; if (nu < 16) u[nu++] = (uint8_t)(acc >> bits); }
        }
        if (ok && nu >= 4 && nu <= 10) { memset(a->uid, 0, sizeof a->uid); memcpy(a->uid, u, (size_t)nu); a->uid_len = nu; }
    }
    if (!a->uid_len) derive_serial(a);
    if (!json_string(s, end, "Name", jname, jcap)) jname[0] = 0;
    return UIT_AMIIBO_OK;
}

/* [UIT 0181] A Flipper Zero dump (.nfc, text): "Page N: 04 A1 B2 3C" lines are the tag's 4-byte pages, so pages 0..134
 * are the raw layout above. 1: it is one (*a filled, or *err why not), 0: not this format. */
static int parse_nfc(const char *s, size_t n, UitAmiibo *a, int *err) {
    static const char kHead[] = "Filetype: Flipper NFC device";
    if (n < sizeof kHead - 1 || memcmp(s, kHead, sizeof kHead - 1)) return 0;
    uint8_t raw[540];
    uint8_t have[135];
    memset(raw, 0, sizeof raw); memset(have, 0, sizeof have);
    const char *end = s + n;
    for (const char *p = s; p < end;) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        if (eol - p > 5 && !memcmp(p, "Page ", 5)) {
            const char *q = p + 5;
            int pg = 0, digits = 0;
            while (q < eol && *q >= '0' && *q <= '9' && digits < 4) pg = pg * 10 + (*q++ - '0'), digits++;
            if (digits && q < eol && *q == ':' && pg < 135) {
                q++;
                int nb = 0;
                uint8_t b[4];
                while (q < eol && nb < 4) {
                    while (q < eol && (*q == ' ' || *q == '\t')) q++;
                    if (eol - q < 2) break;
                    int x = hexval(q[0]), y = hexval(q[1]);
                    if (x < 0 || y < 0) break;
                    b[nb++] = (uint8_t)(x << 4 | y);
                    q += 2;
                }
                if (nb == 4) { memcpy(raw + 4 * pg, b, 4); have[pg] = 1; }
            }
        }
        p = eol < end ? eol + 1 : end;
    }
    *err = UIT_AMIIBO_NOT_AMIIBO;
    if (!have[0] || !have[1] || !have[2] || !have[4] || !have[21] || !have[22]) return 1;
    if (raw[0x10] != 0xA5 || !serial_ok(raw, raw + 8) || !id_ok(raw + 0x54)) return 1;
    take_serial(a, raw);
    memcpy(a->id, raw + 0x54, 8);
    *err = UIT_AMIIBO_OK;
    return 1;
}

int uit_amiibo_parse(const uint8_t *d, size_t n, const char *file_name, UitAmiibo *out) {
    UitAmiibo a;
    memset(&a, 0, sizeof a);
    if (out) memset(out, 0, sizeof *out);
    if (!d || !n) return UIT_AMIIBO_EMPTY;
    char jname[64] = "";
    size_t k = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) k = 3;   /* UTF-8 BOM */
    while (k < n && (d[k] == ' ' || d[k] == '\t' || d[k] == '\r' || d[k] == '\n')) k++;
    int nfc_err = 0;
    if (n < (1u << 20) && parse_nfc((const char *)d + k, n - k, &a, &nfc_err)) {
        if (nfc_err) return nfc_err;
    } else if (k < n && d[k] == '{' && n < (1u << 20)) {
        int e = parse_json((const char *)d + k, n - k, &a, jname, sizeof jname);
        if (e) return e;
    } else if (n == 532 || n == 540 || n == 572) {
        if (d[0x10] == 0xA5 && serial_ok(d, d + 8) && id_ok(d + 0x54)) {            /* raw (tag) layout */
            take_serial(&a, d);
            memcpy(a.id, d + 0x54, 8);
        } else if (d[0x28] == 0xA5 && serial_ok(d + 0x1D4, d) && id_ok(d + 0x1DC)) { /* plain (decrypted) layout */
            take_serial(&a, d + 0x1D4);
            memcpy(a.id, d + 0x1DC, 8);
        } else return UIT_AMIIBO_NOT_AMIIBO;
    } else return UIT_AMIIBO_SIZE;

    const char *known = uit_amiibo_known_name(a.id);
    if (known) copy_utf8(a.name, sizeof a.name, known, strlen(known));
    else if (jname[0]) copy_utf8(a.name, sizeof a.name, jname, strlen(jname));
    else {
        const char *base = file_name ? file_name : "amiibo";
        for (const char *p = base; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
        size_t bl = strlen(base);
        const char *dot = strrchr(base, '.');
        if (dot && dot != base) bl = (size_t)(dot - base);
        char tmp[96];
        if (bl > 40) bl = 40;
        snprintf(tmp, sizeof tmp, "%.*s (%016llx)", (int)bl, base, (unsigned long long)uit_amiibo_id64(a.id));
        copy_utf8(a.name, sizeof a.name, tmp, strlen(tmp));
    }
    if (out) *out = a;
    return UIT_AMIIBO_OK;
}

/* [UIT 0181] Built-in figures: the Legend of Zelda series characters of the names table (game ID byte 0x01, character
 * byte below 0x80; 0x0180 on is Animal Crossing), so the Amiibo tab can scan one with no file at all. An amiibo is
 * only its ID and serial number to this game (it reads TagInfo and ModelInfo, nothing encrypted), so a figure made from
 * the public ID is what a dump would give, with a fixed serial number derived from the ID (derive_serial). */
static int builtin_ok(uint64_t id) { return (id >> 56) == 0x01 && ((id >> 48) & 0xFF) < 0x80; }
int uit_amiibo_builtin_count(void) {
    int n = 0;
    for (size_t k = 0; k < sizeof kUitAmiiboNames / sizeof kUitAmiiboNames[0]; k++) n += builtin_ok(kUitAmiiboNames[k].id);
    return n;
}
int uit_amiibo_builtin(int i, UitAmiibo *out) {
    if (out) memset(out, 0, sizeof *out);
    for (size_t k = 0; i >= 0 && k < sizeof kUitAmiiboNames / sizeof kUitAmiiboNames[0]; k++) {
        if (!builtin_ok(kUitAmiiboNames[k].id) || i--) continue;
        UitAmiibo a;
        memset(&a, 0, sizeof a);
        for (int b = 0; b < 8; b++) a.id[b] = (uint8_t)(kUitAmiiboNames[k].id >> (56 - 8 * b));
        derive_serial(&a);
        /* the Super Smash Bros. figures (series byte 0x00) share names with the Zelda series ones */
        snprintf(a.name, sizeof a.name, "%s%s", kUitAmiiboNames[k].name, a.id[6] == 0x00 ? " (Smash Bros.)" : "");
        if (out) *out = a;
        return 0;
    }
    return -1;
}

/* ---------- nn::nfp device ---------- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t (*clk)(void);
static UitNfpStore store;
static struct {
    int inited, ds;
    UitAmiibo tag; uint64_t found_at;           /* the figure on the reader (FOUND / MOUNTED) */
    int holding; UitAmiibo held; uint64_t hold_until, arrive_at;
    char key[48];
    int area_size; uint32_t area_access; int area_open; uint8_t area[UIT_NFP_AREA_SIZE];
    unsigned sig; int result;
} S;
static struct { uint8_t uid[10]; uint8_t id[8]; } seen[64];   /* [UIT 0211] see note_serial */
static int seen_n;

static uint64_t now_ms(void) { return clk ? clk() : 0; }

/* [UIT 0180] What the game asks of the reader, in words, for the log and diag/amiibo.txt (nfp.c's sink): every call's
 * answer, the figure arriving and leaving, a scan waiting and running out. A call repeated with the same answer (the
 * game polling) is one line plus a count. Without it a "the amiibo did nothing" report had nothing to go on. */
static void (*log_sink)(const char *line);
static char log_last[200];
static unsigned log_repeats;
void uit_nfp_set_log(void (*sink)(const char *line)) {
    pthread_mutex_lock(&mu);
    log_sink = sink; log_last[0] = 0; log_repeats = 0;
    pthread_mutex_unlock(&mu);
}
static void say(const char *fmt, ...) {   /* holding mu */
    if (!log_sink) return;
    char b[200];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (!strcmp(b, log_last)) { log_repeats++; return; }
    if (log_repeats) { char r[64]; snprintf(r, sizeof r, "(the line before: %u more times)", log_repeats); log_sink(r); }
    log_repeats = 0;
    snprintf(log_last, sizeof log_last, "%s", b);
    log_sink(b);
}
const char *uit_nfp_result_text(uint32_t r) {
    switch (r) {
    case 0: return "ok";
    case UIT_NFP_DEVICE_NOT_FOUND: return "DeviceNotFound (2115-0064)";
    case UIT_NFP_INVALID_ARGUMENT: return "InvalidArgument (2115-0065)";
    case UIT_NFP_AREA_TOO_LARGE: return "WrongApplicationAreaSize (2115-0068)";
    case UIT_NFP_NOT_MOUNTED: return "NotMounted (2115-0073)";
    case UIT_NFP_NEED_RESTART: return "NeedRestart: the figure was taken off (2115-0096)";
    case UIT_NFP_INVALID_STATE: return "WrongDeviceState (2115-0097)";
    case UIT_NFP_AREA_NOT_CREATED: return "ApplicationAreaIsNotInitialized (2115-0128)";
    case UIT_NFP_ACCESS_ID_MISMATCH: return "WrongApplicationAreaId (2115-0152)";
    case UIT_NFP_AREA_EXISTS: return "ApplicationAreaExist (2115-0168)";
    }
    return "another error";
}
const char *uit_nfp_state_text(int ds) {
    switch (ds) {
    case UIT_NFP_DS_INIT: return "idle";
    case UIT_NFP_DS_SEARCHING: return "searching";
    case UIT_NFP_DS_FOUND: return "figure found";
    case UIT_NFP_DS_REMOVED: return "figure removed";
    case UIT_NFP_DS_MOUNTED: return "figure mounted";
    }
    return "?";
}
static void said(const char *call, uint32_t r) {   /* holding mu: the answer to one of the game's calls */
    if (!S.inited && r == UIT_NFP_INVALID_STATE) say("%s: %s (the game has not initialised nfp)", call, uit_nfp_result_text(r));
    else say("%s: %s (reader %s)", call, uit_nfp_result_text(r), uit_nfp_state_text(S.ds));
}

void uit_nfp_setup(uint64_t (*now)(void), const UitNfpStore *st) {
    pthread_mutex_lock(&mu);
    clk = now;
    if (st) store = *st; else memset(&store, 0, sizeof store);
    pthread_mutex_unlock(&mu);
}
void uit_nfp_reset(void) {
    pthread_mutex_lock(&mu);
    memset(&S, 0, sizeof S);
    S.area_size = -1;
    seen_n = 0;
    pthread_mutex_unlock(&mu);
}
__attribute__((constructor)) static void nfp_boot(void) { S.area_size = -1; }

static void drop_tag(void) {   /* holding mu */
    S.area_size = -1; S.area_open = 0; S.key[0] = 0;
    memset(&S.tag, 0, sizeof S.tag);
}
/* [UIT 0210] see amiibo.h */
static int (*fresh_want)(void);
static uint64_t fresh_rng;
void uit_nfp_set_fresh_serial(int (*want)(void), uint64_t seed) {
    pthread_mutex_lock(&mu);
    fresh_want = want; fresh_rng = seed;
    pthread_mutex_unlock(&mu);
}
static uint64_t fresh_next(void) {   /* holding mu: splitmix64 */
    uint64_t z = (fresh_rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static void fresh_serial(UitAmiibo *t, uint64_t now) {   /* holding mu */
    if (!fresh_rng) fresh_rng = now ^ ((uint64_t)(uintptr_t)&fresh_rng << 16) ^ 0x5549543130323130ull;
    uint64_t r = fresh_next();
    memset(t->uid, 0, sizeof t->uid);
    t->uid[0] = 0x04;   /* NXP, as on every amiibo */
    for (int i = 1; i < 7; i++) t->uid[i] = (uint8_t)(r >> (8 * i));
    if (t->uid[3] == 0x88) t->uid[3] = 0x89;   /* 0x88 there is the cascade tag, never a serial byte */
    t->uid_len = 7;
}
static void hexs(const uint8_t *u, int n, char *b, size_t cap) {
    size_t o = 0; b[0] = 0;
    for (int i = 0; i < n && o + 3 <= cap; i++) o += (size_t)snprintf(b + o, cap - o, "%02x", u[i]);
}
/* [UIT 0211] Figures with the same serial number are one figure to the game's once-a-day limit: amiibo packs made with
 * one serial for every file (or copies of one dump) then say "can't be used again today" for a figure never scanned.
 * The serials seen this session, to say so in the log. */
static void note_serial(const UitAmiibo *a, const char *ser) {   /* holding mu */
    for (int i = 0; i < seen_n; i++) {
        if (memcmp(seen[i].uid, a->uid, sizeof a->uid)) continue;
        if (memcmp(seen[i].id, a->id, 8))
            say("  serial number %s was already on the reader this session with another amiibo (ID %016llx): the game "
                "counts both as the same figure for its once-a-day limit", ser, (unsigned long long)uit_amiibo_id64(seen[i].id));
        else say("  serial number %s: this figure was already scanned this session", ser);
        return;
    }
    if (seen_n < (int)(sizeof seen / sizeof seen[0])) {
        memcpy(seen[seen_n].uid, a->uid, sizeof a->uid); memcpy(seen[seen_n].id, a->id, 8); seen_n++;
    }
}
static void arrive(const UitAmiibo *a, uint64_t now) {   /* holding mu; ds == SEARCHING */
    drop_tag();
    S.tag = *a;
    uit_amiibo_key(a, S.key, sizeof S.key);   /* the application area stays with the file's own serial */
    S.ds = UIT_NFP_DS_FOUND; S.found_at = now;
    S.holding = 0; S.arrive_at = 0;
    S.sig |= UIT_NFP_SIG_ACTIVATE;
    S.result = 2;
    char ser[24];
    hexs(a->uid, a->uid_len, ser, sizeof ser);
    say("on the reader: %s (amiibo ID %016llx, serial number %s): the game is told (activate event)", a->name,
        (unsigned long long)uit_amiibo_id64(a->id), ser);
    if (fresh_want && fresh_want()) {
        fresh_serial(&S.tag, now);
        hexs(S.tag.uid, S.tag.uid_len, ser, sizeof ser);
        say("  Limitless amiibo scanning: the game reads it with a new serial number %s (its once-a-day record is per "
            "serial number, so it has not seen this figure today)", ser);
    } else note_serial(a, ser);
}
void uit_nfp_tag_serial(char *buf, size_t n) {
    pthread_mutex_lock(&mu);
    if (n) buf[0] = 0;
    if (n && (S.ds == UIT_NFP_DS_FOUND || S.ds == UIT_NFP_DS_MOUNTED)) hexs(S.tag.uid, S.tag.uid_len, buf, n);
    pthread_mutex_unlock(&mu);
}
static _Atomic int present_ms = UIT_NFP_PRESENT_MS;   /* [UIT 0208] */
int uit_nfp_set_present_ms(int ms) {
    if (ms < UIT_NFP_PRESENT_MIN_MS) ms = UIT_NFP_PRESENT_MIN_MS;
    if (ms > UIT_NFP_PRESENT_MAX_MS) ms = UIT_NFP_PRESENT_MAX_MS;
    atomic_store(&present_ms, ms);
    return ms;
}
int uit_nfp_present_ms(void) { return atomic_load(&present_ms); }
static void tick_locked(void) {
    uint64_t now = now_ms();
    int pms = uit_nfp_present_ms();
    if ((S.ds == UIT_NFP_DS_FOUND || S.ds == UIT_NFP_DS_MOUNTED) && now - S.found_at >= (uint64_t)pms) {
        say("taken off the reader after %d.%d s%s", pms / 1000, pms % 1000 / 100,
            S.ds == UIT_NFP_DS_FOUND ? ": the game never mounted it (no Mount while it was there)" : "");
        drop_tag();   /* lifted off the reader: unsaved application-area changes are lost, as on hardware */
        S.ds = UIT_NFP_DS_REMOVED;
        S.sig |= UIT_NFP_SIG_DEACTIVATE;
    }
    if (S.holding && (int64_t)(now - S.hold_until) >= 0) {
        say("%s waited %d s and was taken off: the game did not search for an amiibo (%s)", S.held.name,
            UIT_NFP_HOLD_MS / 1000, !S.inited ? "nfp not initialised" : uit_nfp_state_text(S.ds));
        S.holding = 0; S.arrive_at = 0; if (S.result == 1) S.result = 3;
    }
    if (S.holding && S.inited && S.ds == UIT_NFP_DS_SEARCHING) {
        if (!S.arrive_at) S.arrive_at = now + UIT_NFP_ARRIVE_MS;
        else if ((int64_t)(now - S.arrive_at) >= 0) arrive(&S.held, now);
    }
}
#define LOCKED_TICK() pthread_mutex_lock(&mu); tick_locked()
#define RET(v) do { uint32_t r_ = (v); pthread_mutex_unlock(&mu); return r_; } while (0)
#define RETN(call, v) do { uint32_t r_ = (v); said(call, r_); pthread_mutex_unlock(&mu); return r_; } while (0)

void uit_nfp_tick(void) { LOCKED_TICK(); pthread_mutex_unlock(&mu); }
unsigned uit_nfp_take_signals(void) {
    pthread_mutex_lock(&mu);
    unsigned s = S.sig; S.sig = 0;
    pthread_mutex_unlock(&mu);
    return s;
}
int uit_nfp_scan_result(void) { pthread_mutex_lock(&mu); int r = S.result; pthread_mutex_unlock(&mu); return r; }

int uit_nfp_present(const UitAmiibo *a) {
    LOCKED_TICK();
    if (S.inited && S.ds == UIT_NFP_DS_SEARCHING) { say("scanned %s: the game was searching", a->name); arrive(a, now_ms()); RET(1); }
    say("scanned %s: the game is not searching (%s): it waits on the reader up to %d s", a->name,
        !S.inited ? "nfp not initialised" : uit_nfp_state_text(S.ds), UIT_NFP_HOLD_MS / 1000);
    S.held = *a; S.holding = 1; S.hold_until = now_ms() + UIT_NFP_HOLD_MS; S.arrive_at = 0;
    S.result = 1;
    RET(2);
}

uint32_t uit_nfp_initialize(void) {
    LOCKED_TICK();
    if (!S.inited) { S.inited = 1; S.ds = UIT_NFP_DS_INIT; drop_tag(); S.arrive_at = 0; }
    RETN("Initialize", 0);
}
uint32_t uit_nfp_finalize(void) {
    LOCKED_TICK();
    S.inited = 0; S.ds = UIT_NFP_DS_INIT; drop_tag(); S.arrive_at = 0;
    RETN("Finalize", 0);
}
int uit_nfp_state(void) { pthread_mutex_lock(&mu); int s = S.inited; pthread_mutex_unlock(&mu); return s; }

static uint32_t check_locked(uint64_t h) {
    if (!S.inited) return UIT_NFP_INVALID_STATE;
    if (h != UIT_NFP_HANDLE) return UIT_NFP_DEVICE_NOT_FOUND;
    return 0;
}
uint32_t uit_nfp_check(uint64_t h) { pthread_mutex_lock(&mu); RET(check_locked(h)); }

uint32_t uit_nfp_list(uint64_t *handles, int *count, int max) {
    LOCKED_TICK();
    if (count) *count = 0;
    if (!S.inited) RETN("ListDevices", UIT_NFP_INVALID_STATE);
    if (!handles || !count || max < 1) RETN("ListDevices", UIT_NFP_INVALID_ARGUMENT);
    handles[0] = UIT_NFP_HANDLE; *count = 1;
    RETN("ListDevices", 0);
}
int uit_nfp_device_state(uint64_t h) {
    LOCKED_TICK();
    int ds = (S.inited && h == UIT_NFP_HANDLE) ? S.ds : UIT_NFP_DS_INIT;
    pthread_mutex_unlock(&mu);
    return ds;
}
uint32_t uit_nfp_start(uint64_t h) {
    LOCKED_TICK();
    uint32_t r = check_locked(h);
    if (r) RETN("StartDetection", r);
    if (S.ds == UIT_NFP_DS_FOUND || S.ds == UIT_NFP_DS_MOUNTED) RETN("StartDetection", UIT_NFP_INVALID_STATE);
    S.ds = UIT_NFP_DS_SEARCHING; S.arrive_at = 0;
    tick_locked();   /* a figure waiting on the reader: arrival scheduled UIT_NFP_ARRIVE_MS from now */
    RETN("StartDetection", 0);
}
uint32_t uit_nfp_stop(uint64_t h) {
    LOCKED_TICK();
    uint32_t r = check_locked(h);
    if (r) RETN("StopDetection", r);
    drop_tag(); S.ds = UIT_NFP_DS_INIT; S.arrive_at = 0;
    RETN("StopDetection", 0);
}
uint32_t uit_nfp_mount(uint64_t h, int model_type, int target) {
    LOCKED_TICK();
    uint32_t r = check_locked(h);
    if (r) RETN("Mount", r);
    say("Mount asked: model type %d, target %d (%s)", model_type, target,
        target == 1 ? "ROM" : target == 2 ? "RAM" : target == 3 ? "ROM and RAM" : "unknown");   /* [UIT 0211] */
    if (model_type != 0 || !(target & 3)) RETN("Mount", UIT_NFP_INVALID_ARGUMENT);
    if (S.ds == UIT_NFP_DS_MOUNTED) RETN("Mount", 0);
    if (S.ds != UIT_NFP_DS_FOUND) RETN("Mount", S.ds == UIT_NFP_DS_REMOVED ? UIT_NFP_NEED_RESTART : UIT_NFP_INVALID_STATE);
    S.area_size = -1; S.area_open = 0;
    if (store.load) {
        uint32_t acc = 0;
        int n = store.load(S.key, S.area, sizeof S.area, &acc);
        if (n >= 0) { S.area_size = n > UIT_NFP_AREA_SIZE ? UIT_NFP_AREA_SIZE : n; S.area_access = acc;
                      memset(S.area + S.area_size, 0, sizeof S.area - (size_t)S.area_size); }
    }
    S.ds = UIT_NFP_DS_MOUNTED;
    S.result = 4;
    say("Mount: %s read by the game (application area %s)", S.tag.name, S.area_size >= 0 ? "saved before" : "none yet");
    RETN("Mount", 0);
}
uint32_t uit_nfp_unmount(uint64_t h) {
    LOCKED_TICK();
    uint32_t r = check_locked(h);
    if (r) RETN("Unmount", r);
    if (S.ds != UIT_NFP_DS_MOUNTED) RETN("Unmount", S.ds == UIT_NFP_DS_REMOVED ? UIT_NFP_NEED_RESTART : UIT_NFP_INVALID_STATE);
    S.ds = UIT_NFP_DS_FOUND; S.area_open = 0;
    RETN("Unmount", 0);
}
static uint32_t need(uint64_t h, int mounted) {   /* holding mu */
    uint32_t r = check_locked(h);
    if (r) return r;
    if (S.ds == UIT_NFP_DS_MOUNTED || (!mounted && S.ds == UIT_NFP_DS_FOUND)) return 0;
    if (S.ds == UIT_NFP_DS_REMOVED) return UIT_NFP_NEED_RESTART;
    return mounted && S.ds == UIT_NFP_DS_FOUND ? UIT_NFP_NOT_MOUNTED : UIT_NFP_INVALID_STATE;
}
uint32_t uit_nfp_tag_info(uint64_t h, uint8_t out[0x58]) {
    LOCKED_TICK();
    uint32_t r = need(h, 0);
    if (r) RETN("GetTagInfo", r);
    memset(out, 0, 0x58);
    memcpy(out, S.tag.uid, 10);
    out[0xA] = (uint8_t)S.tag.uid_len;
    out[0x20] = 1;   /* protocol: NFC-A */
    out[0x24] = 2;   /* tag type: Type 2 (NTAG215) */
    RETN("GetTagInfo", 0);
}
uint32_t uit_nfp_model_info(uint64_t h, uint8_t out[0x40]) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("GetModelInfo", r);
    memset(out, 0, 0x40);
    memcpy(out, S.tag.id, 4);            /* character ID (3 bytes) and the byte after it, as stored */
    out[4] = S.tag.id[5]; out[5] = S.tag.id[4];   /* numbering ID: big-endian on the tag, little-endian here */
    out[6] = S.tag.id[6];
    RETN("GetModelInfo", 0);
}
static void put_date(uint8_t *p) { p[0] = 0xE7; p[1] = 0x07; p[2] = 5; p[3] = 12; }   /* 2023-05-12 */
uint32_t uit_nfp_common_info(uint64_t h, uint8_t out[0x40]) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("GetCommonInfo", r);
    memset(out, 0, 0x40);
    put_date(out);
    out[8] = UIT_NFP_AREA_SIZE;
    RETN("GetCommonInfo", 0);
}
uint32_t uit_nfp_register_info(uint64_t h, uint8_t out[0x100]) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("GetRegisterInfo", r);
    memset(out, 0, 0x100);
    put_date(out + 0x58);
    /* Nickname: the first 10 characters of the shown name (amiibo nicknames hold 10 UTF-16 units). */
    const char *s = S.tag.name; size_t o = 0; int chars = 0;
    while (s[o] && chars < 10 && o < 0x28) {
        size_t l = 1;
        while (s[o + l] && ((unsigned char)s[o + l] & 0xC0) == 0x80) l++;
        if (o + l > 0x28 || s[o] == '(') break;
        o += l; chars++;
    }
    while (o && s[o - 1] == ' ') o--;
    memcpy(out + 0x5C, s, o);
    RETN("GetRegisterInfo", 0);
}
uint32_t uit_nfp_open_area(uint64_t h, uint32_t access_id) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("OpenApplicationArea", r);
    if (S.area_size < 0) RETN("OpenApplicationArea", UIT_NFP_AREA_NOT_CREATED);
    if (S.area_access != access_id) RETN("OpenApplicationArea", UIT_NFP_ACCESS_ID_MISMATCH);
    S.area_open = 1;
    RETN("OpenApplicationArea", 0);
}
uint32_t uit_nfp_get_area(uint64_t h, void *buf, uint64_t *size, uint64_t cap) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("GetApplicationArea", r);
    if (!S.area_open) RETN("GetApplicationArea", UIT_NFP_AREA_NOT_CREATED);
    if (!buf) RETN("GetApplicationArea", UIT_NFP_INVALID_ARGUMENT);
    uint64_t n = cap < UIT_NFP_AREA_SIZE ? cap : UIT_NFP_AREA_SIZE;
    memcpy(buf, S.area, (size_t)n);
    if (size) *size = n;
    RETN("GetApplicationArea", 0);
}
uint32_t uit_nfp_set_area(uint64_t h, const void *buf, uint64_t n) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("SetApplicationArea", r);
    if (!S.area_open) RETN("SetApplicationArea", UIT_NFP_AREA_NOT_CREATED);
    if (!buf && n) RETN("SetApplicationArea", UIT_NFP_INVALID_ARGUMENT);
    if (n > UIT_NFP_AREA_SIZE) RETN("SetApplicationArea", UIT_NFP_AREA_TOO_LARGE);
    memset(S.area, 0, sizeof S.area);
    if (n) memcpy(S.area, buf, (size_t)n);
    S.area_size = UIT_NFP_AREA_SIZE;
    RETN("SetApplicationArea", 0);
}
uint32_t uit_nfp_create_area(uint64_t h, uint32_t access_id, const void *buf, uint64_t n) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("CreateApplicationArea", r);
    if (S.area_size >= 0) RETN("CreateApplicationArea", UIT_NFP_AREA_EXISTS);
    if (!buf && n) RETN("CreateApplicationArea", UIT_NFP_INVALID_ARGUMENT);
    if (n > UIT_NFP_AREA_SIZE) RETN("CreateApplicationArea", UIT_NFP_AREA_TOO_LARGE);
    memset(S.area, 0, sizeof S.area);
    if (n) memcpy(S.area, buf, (size_t)n);
    S.area_size = UIT_NFP_AREA_SIZE; S.area_access = access_id;
    if (store.save) store.save(S.key, S.area, UIT_NFP_AREA_SIZE, access_id);   /* creating writes the tag */
    RETN("CreateApplicationArea", 0);
}
uint32_t uit_nfp_flush(uint64_t h) {
    LOCKED_TICK();
    uint32_t r = need(h, 1);
    if (r) RETN("Flush", r);
    if (S.area_size >= 0 && store.save) store.save(S.key, S.area, (size_t)S.area_size, S.area_access);
    RETN("Flush", 0);
}
