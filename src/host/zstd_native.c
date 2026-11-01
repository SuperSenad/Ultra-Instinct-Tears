// [UIT 0079] The game's zstd decoder on the PC's own libzstd.
// Adapted from valentderah/totk-pc-recomp runtime/ov_zstd.c (reference commit c3b9a4e).
// Copyright (c) 2026 Valiantsin Dzerakh. MIT License (third_party/licenses/valentderah-totk-recomp.txt).
//
// Its header, kept: "The game's zstd decoder on the host's libzstd. Most RomFS files of TotK 1.4.3 are zstd frames, many
// with a dictionary from Pack/ZsDic.pack.zs; the decoder is BSD zstd linked into main. [...] sends every call of
// ZSTD_decompressMultiFrame (main+0x3a1f0) and of ZSTD_decompress_usingDDict (main+0x3a1e0) to the two host functions
// below [...]. Lifted, the decoder ran its literal and match copies as chains of small functions with the registers in
// Cpu; here the same frames decode in native code.
// - Results match the guest's: libzstd decodes the same format, and every result that is an error goes to the guest
//   decoder again (it writes its own error code; a frame libzstd rejects may still be one the game's version accepts).
// - A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
//   copy of its content, cached by guest address, content address, size and a fingerprint of the content.
// - Inputs of at least AWAY_BYTES give the virtual core up while they decode (other guest threads run on it).
// TOTK_ZSTD_NATIVE=0 keeps the guest decoder (the patch set switch); TOTK_ZSTD_CHECK=1 decodes each call both ways,
// keeps the guest's result and logs every call where the two differ."
//
// Here (UIT): the seven call sites of their 1.4.3 patch set are redirected when the code is lifted
// (tools/lifter/zstd_sites.py, which checks each site's original instruction and never takes main+0x547a04: that
// caller resets a static DCtx of its own with which the game's zstd rejects some valid frames, and the game relies on
// that result). The guest decoder is the lifted function itself (the call site passes it), called with the registers
// unchanged.
//   TOTK_ZSTD_NATIVE=1   decode on the PC (Performance > Loading > Faster file unpacking), default off
//   TOTK_ZSTD_CHECK=1    decode both ways, keep the game's result, log differences (needs TOTK_ZSTD_NATIVE=1)
// Logging: a few lines to stderr (the game log when logging is on), at most a handful per kind.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <zstd.h>

#include "../runtime/cpu.h"   /* [UIT 0104] relative, like the other host files (cc rule: -Ibuild only) */

#ifndef ZN_LO   // guest address window (tools/tests/test_zstd_native.c builds this file with its own)
#define ZN_LO 0x0800000000ull   // GUEST_LO, rt.h
#define ZN_HI 0x8000000000ull   // GUEST_HI
#endif
uint64_t runtime_now_ns(void);
int runtime_core_away(void);
void runtime_core_back(int *away);
int plat_thread_ecores(void);   // [UIT 0089] platform.h
int plat_thread_game(void);
typedef void (*GuestDecoder)(Cpu *c);   // the lifted decoder, handed over by the call site (tools/lifter/zstd_sites.py)
void uit_zstd_multiframe(Cpu *c, GuestDecoder guest);
void uit_zstd_using_ddict(Cpu *c, GuestDecoder guest);

#define AWAY_BYTES (64u << 10)
#define MAX_DICT (64u << 20)
#define NDICT 32
#define LOG(...) (fprintf(stderr, "[totk] zstd: " __VA_ARGS__), fputc('\n', stderr))

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }
static int in_guest(uint64_t a, uint64_t n) { return a >= ZN_LO && a < ZN_HI && n <= ZN_HI - a; }

typedef struct {
    uint64_t guest, content, size, print;
    ZSTD_DDict *dd;
} Dict;
static Dict dicts[NDICT];
static int ndicts;
static pthread_rwlock_t dict_lock = PTHREAD_RWLOCK_INITIALIZER;

static struct {
    uint64_t calls, in, out, ns, guest_ns, fallbacks, away, inplace, mismatches;
} S;

static int env_on(const char *k) { const char *e = getenv(k); return e && *e && strcmp(e, "0") && strcmp(e, "off"); }
static int g_native = -1, g_check = -1;   // read once
static int native_on(void) { if (g_native < 0) g_native = env_on("TOTK_ZSTD_NATIVE"); return g_native; }
static int check_mode(void) { if (g_check < 0) g_check = env_on("TOTK_ZSTD_CHECK"); return g_check; }

// Size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the old one.
static uint64_t fingerprint(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9E3779B97F4A7C15ull;
    uint64_t at[3] = {0, n / 2, n > 64 ? n - 64 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 64 && i < n; i += 8) {
            uint64_t v = 0;
            memcpy(&v, p + i, n - i < 8 ? n - i : 8);
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
    return h;
}

static ZSTD_DDict *find_dict(uint64_t guest, uint64_t content, uint64_t size, uint64_t print) {
    for (int i = 0; i < ndicts; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            return dicts[i].dd;
    return NULL;
}

// The host dictionary for a guest ZSTD_DDict or raw dictionary bytes (guest == 0); NULL when its fields look wrong.
static ZSTD_DDict *dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > MAX_DICT || !in_guest(content, size)) return NULL;
    uint64_t print = fingerprint(gp(content), size);
    pthread_rwlock_rdlock(&dict_lock);
    ZSTD_DDict *dd = find_dict(guest, content, size, print);
    pthread_rwlock_unlock(&dict_lock);
    if (dd) return dd;
    // A copy: the guest may free or reuse its buffer. Entries are never freed (another thread may be decoding with
    // one); a full table sends further dictionaries to the guest decoder.
    ZSTD_DDict *made = ZSTD_createDDict(gp(content), (size_t)size);
    if (!made) return NULL;
    int slot = -1;
    pthread_rwlock_wrlock(&dict_lock);
    dd = find_dict(guest, content, size, print);   // another thread may have made it meanwhile
    if (!dd && ndicts < NDICT) slot = ndicts, dicts[ndicts++] = (Dict){guest, content, size, print, made}, dd = made;
    pthread_rwlock_unlock(&dict_lock);
    if (dd != made) {
        ZSTD_freeDDict(made);
        if (!dd) LOG("more than %d dictionaries: further ones decode in the game's decoder", NDICT);
        return dd;
    }
    LOG("PC dictionary %d for %s %#llx: %llu bytes, id %u", slot, guest ? "DDict" : "raw dict",
        (unsigned long long)(guest ? guest : content), (unsigned long long)size, ZSTD_getDictID_fromDDict(dd));
    return dd;
}

// Decodes on this thread's own context. Returns libzstd's result; *ok 0 when the inputs cannot be taken.
static size_t host_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    static __thread ZSTD_DCtx *dctx;
    *ok = 0;
    if ((cap && !dst) || !src || !in_guest(src, size)) return 0;   // dst: decode() checks a guest one
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (!in_guest(ddict, 24)) return 0;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) {
            static int logged;
            if (logged < 4) logged++, LOG("DDict %#llx not taken: content %#llx size %llu", (unsigned long long)ddict,
                                          (unsigned long long)f[1], (unsigned long long)f[2]);
            return 0;
        }
    } else if (dict && dict_size) {
        if (!(dd = dict_for(0, dict, dict_size))) return 0;
    }
    if (!dctx && !(dctx = ZSTD_createDCtx())) return 0;
    *ok = 1;
    return dd ? ZSTD_decompress_usingDDict(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size, dd)
              : ZSTD_decompressDCtx(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size);
}

// Large inputs lend the virtual core out while they decode (the decode touches no guest thread state).
static size_t away_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    int away = size >= AWAY_BYTES ? runtime_core_away() : 0;
    if (away) __atomic_add_fetch(&S.away, 1, __ATOMIC_RELAXED);
    // [UIT 0089] TOTK_HYBRID_PLACEMENT=1: the decode on the efficiency cores, then back to the performance cores
    int ecore = away ? plat_thread_ecores() : 0;
    size_t r = host_decode(dst, cap, src, size, dict, dict_size, ddict, ok);
    if (ecore) plat_thread_game();
    runtime_core_back(&away);
    return r;
}

static void count(uint64_t in, uint64_t out, uint64_t ns) {
    uint64_t n = __atomic_add_fetch(&S.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.in, in, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.out, out, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.ns, ns, __ATOMIC_RELAXED);
    if (n == 1 || !(n & 0x3fff))
        LOG("%llu calls on the PC, %.1f MiB in, %.1f MiB out, %.0f ms (check mode: the game's decoder %.0f ms, "
            "%llu differences); %llu to the game's decoder, %llu in place, %llu with the core lent out",
            (unsigned long long)n, S.in / 1048576.0, S.out / 1048576.0, S.ns / 1e6, S.guest_ns / 1e6,
            (unsigned long long)S.mismatches, (unsigned long long)S.fallbacks, (unsigned long long)S.inplace,
            (unsigned long long)S.away);
}

// x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them. guest: the
// lifted function, called with the registers exactly as the game's call left them.
static void decode(Cpu *c, uint64_t dict, uint64_t dict_size, uint64_t ddict, void (*guest)(Cpu *), int multi) {
    uint64_t dst = c->x[1], cap = c->x[2], src = c->x[3], size = c->x[4];
    if (!native_on()) { guest(c); return; }
    // In place (the input inside the output buffer): the result depends on how the decoder's writes overtake its
    // reads, which libzstd does not promise to do as the game's version does, and a host decode that failed half way
    // would leave the guest decoder a damaged input. Such calls stay in the guest.
    if (cap && src < dst + cap && dst < src + size) {
        __atomic_add_fetch(&S.inplace, 1, __ATOMIC_RELAXED);
        guest(c);
        return;
    }
    if (check_mode()) {
        uint8_t *mine = cap ? malloc((size_t)cap) : NULL;
        int ok = 0;
        uint64_t t0 = runtime_now_ns();
        size_t r = 0;
        if (!cap || mine) {   // into a buffer of its own: host_decode takes it as a guest one, so it is called directly
            static __thread ZSTD_DCtx *cx;
            ZSTD_DDict *dd = NULL;
            int take = in_guest(src, size) && src;
            if (take && ddict) {
                uint64_t f[3];
                take = in_guest(ddict, 24) && (memcpy(f, gp(ddict), sizeof f), (dd = dict_for(ddict, f[1], f[2])) != NULL);
            } else if (take && dict && dict_size)
                take = (dd = dict_for(0, dict, dict_size)) != NULL;
            if (take && (cx || (cx = ZSTD_createDCtx()))) {
                ok = 1;
                r = dd ? ZSTD_decompress_usingDDict(cx, mine, (size_t)cap, gp(src), (size_t)size, dd)
                       : ZSTD_decompressDCtx(cx, mine, (size_t)cap, gp(src), (size_t)size);
            }
        }
        uint64_t t1 = runtime_now_ns();
        guest(c);
        uint64_t g = c->x[0];
        __atomic_add_fetch(&S.guest_ns, runtime_now_ns() - t1, __ATOMIC_RELAXED);
        int differ = ok && !ZSTD_isError(r) && (g != r || (r && memcmp(mine, gp(dst), r)));
        if (differ) {
            uint64_t k = __atomic_add_fetch(&S.mismatches, 1, __ATOMIC_RELAXED);
            if (k <= 8)
                LOG("check: DIFFERENCE %llu bytes from %#llx (cap %llu, dict %#llx/%llu, ddict %#llx, %s, return to "
                    "%#llx): the PC decoded %llu bytes, the game %lld", (unsigned long long)size,
                    (unsigned long long)src, (unsigned long long)cap, (unsigned long long)dict,
                    (unsigned long long)dict_size, (unsigned long long)ddict, multi ? "MultiFrame" : "usingDDict",
                    (unsigned long long)c->x[30], (unsigned long long)r, (long long)g);
        } else if (!ok || ZSTD_isError(r)) {
            static int logged;
            if (logged < 6) logged++, LOG("check: PC %s, game %lld (%llu bytes at %#llx)",
                                          !ok ? "inputs not taken" : ZSTD_getErrorName(r), (long long)g,
                                          (unsigned long long)size, (unsigned long long)src);
        }
        free(mine);
        count(size, ok && !ZSTD_isError(r) ? r : 0, t1 - t0);
        return;
    }
    uint64_t t0 = runtime_now_ns();
    int ok = 0;
    size_t r = cap && !in_guest(dst, cap) ? 0 : away_decode(dst, cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {   // the guest decoder decides (and writes its own error code)
        __atomic_add_fetch(&S.fallbacks, 1, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 8) logged++, LOG("%llu bytes at %#llx go to the game's decoder: %s", (unsigned long long)size,
                                      (unsigned long long)src, !ok ? "inputs not taken" : ZSTD_getErrorName(r));
        guest(c);
        return;
    }
    c->x[0] = r;
    count(size, r, runtime_now_ns() - t0);
}

// Called by the lifted game code in place of `bl ZSTD_decompressMultiFrame` / `bl ZSTD_decompress_usingDDict`.
// guest: the lifted ZSTD_decompressMultiFrame (main+0x3a1f0) / ZSTD_decompress_usingDDict (main+0x3a1e0).
void uit_zstd_multiframe(Cpu *c, GuestDecoder guest) { decode(c, c->x[5], c->x[6], c->x[7], guest, 1); }
void uit_zstd_using_ddict(Cpu *c, GuestDecoder guest) { decode(c, 0, 0, c->x[5], guest, 0); }
