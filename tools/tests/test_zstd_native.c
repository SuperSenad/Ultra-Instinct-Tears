/* [UIT 0079] Box test of src/host/zstd_native.c on made-up frames (no game data): the PC decoder against a stand-in for
 * the game's decoder (libzstd too, so results are known), its fall-backs and the check mode.
 *   clang -O2 -Isrc/runtime -I<zstd.h folder> tools/tests/test_zstd_native.c -lzstd -lpthread -o /tmp/t/zn && /tmp/t/zn
 * (clang: cpu.h uses its builtins; on the box ltools/zcc, -I/tmp/zinc and /usr/lib/x86_64-linux-gnu/libzstd.so.1 in place of -lzstd) */
#define ZN_LO 0x1000ull   /* host heap addresses stand for guest ones here */
#define ZN_HI 0x7fffffffffffull
#include "../../src/host/zstd_native.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static int aways, backs;
int runtime_core_away(void) { aways++; return 1; }
void runtime_core_back(int *a) { if (*a) backs++; }
static int ecores, games;   // [UIT 0089] TOTK_HYBRID_PLACEMENT=1 around a decode away from its core
int plat_thread_ecores(void) { ecores++; return 1; }
int plat_thread_game(void) { games++; return 1; }

/* the stand-in game decoder: decodes with libzstd (wrong = 1: writes one byte wrong, as a different decoder would) */
static int guest_calls, wrong;
static void guest_multi(Cpu *c) {
    guest_calls++;
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t r = c->x[5] && c->x[6] ? ZSTD_decompress_usingDict(d, gp(c->x[1]), c->x[2], gp(c->x[3]), c->x[4], gp(c->x[5]), c->x[6])
                                  : ZSTD_decompressDCtx(d, gp(c->x[1]), c->x[2], gp(c->x[3]), c->x[4]);
    ZSTD_freeDCtx(d);
    if (!ZSTD_isError(r) && r && wrong) ((uint8_t *)gp(c->x[1]))[r / 2] ^= 1;
    c->x[0] = ZSTD_isError(r) ? (uint64_t)-20 : r;   /* the game's own error code */
}
static void guest_ddict(Cpu *c) {
    guest_calls++;
    const uint64_t *f = gp(c->x[5]);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t r = ZSTD_decompress_usingDict(d, gp(c->x[1]), c->x[2], gp(c->x[3]), c->x[4], gp(f[1]), f[2]);
    ZSTD_freeDCtx(d);
    c->x[0] = ZSTD_isError(r) ? (uint64_t)-20 : r;
}

static uint8_t *pattern(size_t n, unsigned seed) {
    uint8_t *p = malloc(n);
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)("the quick brown fox jumps over the lazy dog "[(i * 7 + seed) % 44] ^ (i % 97 == 0 ? seed : 0));
    return p;
}
static void call(Cpu *c, int multi, uint8_t *dst, size_t cap, const uint8_t *src, size_t n, const void *dict, size_t dn,
                 const uint64_t *ddict) {
    memset(c, 0, sizeof *c);
    c->x[0] = 0x1234;   /* dctx */
    c->x[1] = (uint64_t)(uintptr_t)dst, c->x[2] = cap, c->x[3] = (uint64_t)(uintptr_t)src, c->x[4] = n;
    if (multi) c->x[5] = (uint64_t)(uintptr_t)dict, c->x[6] = dn, c->x[7] = (uint64_t)(uintptr_t)ddict;
    else c->x[5] = (uint64_t)(uintptr_t)ddict;
    c->x[30] = 0x800005f535cull;
    if (multi) uit_zstd_multiframe(c, guest_multi); else uit_zstd_using_ddict(c, guest_ddict);
}

int main(void) {
    static Cpu c;
    size_t n = 300000;
    uint8_t *raw = pattern(n, 3), *dict = pattern(20000, 5);
    size_t zc = ZSTD_compressBound(n);
    uint8_t *z = malloc(zc), *zd = malloc(zc), *out = malloc(n + 64);
    size_t zn = ZSTD_compress(z, zc, raw, n, 3);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    size_t zdn = ZSTD_compress_usingDict(cc, zd, zc, raw, n, dict, 20000, 3);
    CHECK(!ZSTD_isError(zn) && !ZSTD_isError(zdn));

    /* 1: off (the default): the game's decoder, registers as the call left them */
    unsetenv("TOTK_ZSTD_NATIVE"); g_native = g_check = -1;
    guest_calls = 0; memset(out, 0, n);
    call(&c, 1, out, n + 64, z, zn, NULL, 0, NULL);
    CHECK(guest_calls == 1 && c.x[0] == n && !memcmp(out, raw, n) && S.calls == 0);

    /* 2: on: the PC decodes, the game's decoder is not called */
    setenv("TOTK_ZSTD_NATIVE", "1", 1); g_native = -1;
    guest_calls = 0; memset(out, 0, n); aways = backs = 0;
    call(&c, 1, out, n + 64, z, zn, NULL, 0, NULL);
    CHECK(guest_calls == 0 && c.x[0] == n && !memcmp(out, raw, n) && S.calls == 1);
    CHECK(aways == 0 && backs == 0 && ecores == 0);   /* the input is under 64 KiB: the core stays */
    size_t bz = ZSTD_compressBound(1 << 20), bn; uint8_t *bzb = malloc(bz), *bo = malloc(1 << 20);
    { uint8_t *noisy = malloc(1 << 20); uint64_t x = 88172645463325252ull;   /* hardly compressible: a big input */
      for (int i = 0; i < 1 << 20; i++) x ^= x << 13, x ^= x >> 7, x ^= x << 17, noisy[i] = (uint8_t)(i & 1 ? x : x >> 40) & 0x7f;
      bn = ZSTD_compress(bzb, bz, noisy, 1 << 20, 1); call(&c, 1, bo, 1 << 20, bzb, bn, NULL, 0, NULL);
      CHECK(bn >= AWAY_BYTES && c.x[0] == (1u << 20) && !memcmp(bo, noisy, 1 << 20) && aways == 1 && backs == 1); free(noisy); }
    CHECK(ecores == 1 && games == 1);   /* [UIT 0089] E-cores for the away decode, then back to the P-cores */

    /* 3: a raw dictionary (MultiFrame's dict / dict size) */
    guest_calls = 0; memset(out, 0, n);
    call(&c, 1, out, n + 64, zd, zdn, dict, 20000, NULL);
    CHECK(guest_calls == 0 && c.x[0] == n && !memcmp(out, raw, n) && ndicts == 1);

    /* 4: a game ZSTD_DDict (dictBuffer, dictContent, dictSize): made once, then taken from the cache */
    uint64_t dd[8] = {(uint64_t)(uintptr_t)dict, (uint64_t)(uintptr_t)dict, 20000};
    for (int k = 0; k < 3; k++) { memset(out, 0, n); call(&c, 0, out, n + 64, zd, zdn, NULL, 0, dd); }
    CHECK(guest_calls == 0 && c.x[0] == n && !memcmp(out, raw, n) && ndicts == 2);
    /* the same address with new content (the game reused the buffer): a new PC dictionary, not the stale one */
    uint8_t *dict2 = pattern(20000, 11); memcpy(dict, dict2, 20000);
    size_t zdn2 = ZSTD_compress_usingDict(cc, zd, zc, raw, n, dict, 20000, 3);
    memset(out, 0, n); call(&c, 0, out, n + 64, zd, zdn2, NULL, 0, dd);
    CHECK(guest_calls == 0 && c.x[0] == n && !memcmp(out, raw, n) && ndicts == 3);

    /* 5: a frame libzstd rejects (cut short) goes to the game's decoder, which writes its own error code */
    guest_calls = 0; uint64_t fb = S.fallbacks;
    call(&c, 1, out, n + 64, z, zn - 9, NULL, 0, NULL);
    CHECK(S.fallbacks == fb + 1 && guest_calls == 1 && c.x[0] == (uint64_t)-20);
    /* too small a buffer: an error on the PC, so the game's decoder decides */
    guest_calls = 0; call(&c, 1, out, n / 2, z, zn, NULL, 0, NULL);
    CHECK(guest_calls == 1 && c.x[0] == (uint64_t)-20);
    /* a DDict whose fields look wrong: not taken */
    uint64_t baddd[3] = {0, 0, 0}; guest_calls = 0;
    call(&c, 0, out, n + 64, zd, zdn2, NULL, 0, baddd);
    CHECK(guest_calls == 1);

    /* 6: in place (input inside the output buffer): the game's decoder */
    uint8_t *ip = malloc(n + zn + 64); memcpy(ip + n + 64 - zn, z, zn); guest_calls = 0; uint64_t inpl = S.inplace;
    { uint8_t *src = ip + n + 64 - zn; memset(&c, 0, sizeof c); c.x[1] = (uint64_t)(uintptr_t)ip; c.x[2] = n + 64;
      c.x[3] = (uint64_t)(uintptr_t)src; c.x[4] = zn; uit_zstd_multiframe(&c, guest_multi); }
    CHECK(guest_calls == 1 && S.inplace == inpl + 1);

    /* 7: check mode keeps the game's result and counts differences */
    setenv("TOTK_ZSTD_CHECK", "1", 1); g_check = -1;
    guest_calls = 0; wrong = 0; uint64_t mm = S.mismatches;
    memset(out, 0, n); call(&c, 1, out, n + 64, z, zn, NULL, 0, NULL);
    CHECK(guest_calls == 1 && c.x[0] == n && !memcmp(out, raw, n) && S.mismatches == mm);
    wrong = 1; memset(out, 0, n); call(&c, 1, out, n + 64, z, zn, NULL, 0, NULL);
    CHECK(guest_calls == 2 && c.x[0] == n && out[n / 2] == (raw[n / 2] ^ 1) && S.mismatches == mm + 1);
    wrong = 0;

    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails != 0;
}
