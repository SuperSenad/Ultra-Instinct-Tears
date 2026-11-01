// [UIT 0085] SIMD_FPCR_HOST (TOTK_LIFT_FPCR_HOST) differential test: every FP helper in runtime/simd.h and
// simd_fp.h, built once as shipped (host MXCSR in round-to-nearest, per-op FPCR.RMode check, exact slow path) and
// once with SIMD_FPCR_HOST (MXCSR rounding follows FPCR, no per-op check), on edge and random values under every
// FPCR FZ / DN / RMode combination. Results must match bit for bit. The one allowed difference is the documented
// FZ tininess case (x86 flushes after rounding, ARM before: a result just below the smallest normal that rounds up
// to it), which the shipped fast path already has in round-to-nearest; it is counted and printed, not failed.
//
// Build and run (Linux, from the repo root; the same three objects build with zig cc / zwcc):
//   CC=clang   (any clang or zig cc with -march=x86-64-v3)
//   for p in REF HOST; do $CC -O2 -march=x86_64_v3 -fno-strict-aliasing -c -DPART_$p -Isrc/runtime tools/tests/test_fpcr_host.c -o /tmp/fh_$p.o; done
//   $CC -O2 -Isrc/runtime tools/tests/test_fpcr_host.c /tmp/fh_REF.o /tmp/fh_HOST.o -lm -o /tmp/fh && /tmp/fh
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } R128;
enum { K_OP_S, K_OP_D, K_VOP_S, K_VOP_D, K_CMP_S, K_CMP_D, K_VCMP_S, K_VCMP_D, K_FRINT_S, K_FRINT_D, K_VFRINT_S,
       K_VFRINT_D, K_TOINT_S, K_TOINT_D, K_FROMINT_S, K_FROMINT_D, K_VFROMINT_S, K_VFROMINT_D, K_VTOINT_S, K_D2S,
       K_S2D, K_D2S_ODD, K_N };

#if defined(PART_REF) || defined(PART_HOST)
#ifdef PART_HOST
#define SIMD_FPCR_HOST 1
#define EVAL eval_host
#else
#define EVAL eval_ref
#endif
#include <immintrin.h>
#include "cpu.h"
#include "simd.h"

static float f32(uint64_t u) { float x; uint32_t v = (uint32_t)u; memcpy(&x, &v, 4); return x; }
static double f64(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }
static uint64_t b32(float x) { uint32_t v; memcpy(&v, &x, 4); return v; }
static uint64_t b64(double x) { uint64_t v; memcpy(&v, &x, 8); return v; }
static vf32 v32(const uint64_t *a) { vf32 r; memcpy(&r, a, 16); return r; }
static vf64 v64(const uint64_t *a) { vf64 r; memcpy(&r, a, 16); return r; }
static R128 rv(const void *p) { R128 r; memcpy(&r, p, 16); return r; }

// p: the op / mode / conversion shape (see main). a, b, d: 128-bit operands (scalars use the low bits).
__attribute__((noinline)) R128 EVAL(int k, int p, uint64_t fpcr, const uint64_t *a, const uint64_t *b, const uint64_t *d) {
    Cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    Cpu *c = &cpu;
    unsigned saved = _mm_getcsr();
    rt_fpcr_write(c, fpcr);
    R128 r = {0, 0};
    int rm = p & 7, fbits = (p >> 3) & 63, ib = (p >> 9) & 1 ? 64 : 32, uns = (p >> 10) & 1;
    switch (k) {
    case K_OP_S: r.lo = b32(op_s(c, p, f32(a[0]), f32(b[0]), f32(d[0]))); break;
    case K_OP_D: r.lo = b64(op_d(c, p, f64(a[0]), f64(b[0]), f64(d[0]))); break;
    case K_VOP_S: { vf32 x = vop_s(c, p, v32(a), v32(b), v32(d)); r = rv(&x); break; }
    case K_VOP_D: { vf64 x = vop_d(c, p, v64(a), v64(b), v64(d)); r = rv(&x); break; }
    case K_CMP_S: r.lo = cmp_s(c, f32(a[0]), f32(b[0])); break;
    case K_CMP_D: r.lo = cmp_d(c, f64(a[0]), f64(b[0])); break;
    case K_VCMP_S: { vu32 x = vcmp_s(c, p, v32(a), v32(b)); r = rv(&x); break; }
    case K_VCMP_D: { vu64 x = vcmp_d(c, p, v64(a), v64(b)); r = rv(&x); break; }
    case K_FRINT_S: r.lo = b32(frint_s(c, f32(a[0]), p)); break;
    case K_FRINT_D: r.lo = b64(frint_d(c, f64(a[0]), p)); break;
    case K_VFRINT_S: { vf32 x = vfrint_s(c, v32(a), p); r = rv(&x); break; }
    case K_VFRINT_D: { vf64 x = vfrint_d(c, v64(a), p); r = rv(&x); break; }
    case K_TOINT_S: r.lo = toint_s(c, f32(a[0]), rm, fbits, ib, uns); break;
    case K_TOINT_D: r.lo = toint_d(c, f64(a[0]), rm, fbits, ib, uns); break;
    case K_FROMINT_S: r.lo = b32(fromint_s(c, a[0], ib, uns, fbits)); break;
    case K_FROMINT_D: r.lo = b64(fromint_d(c, a[0], ib, uns, fbits)); break;
    case K_VFROMINT_S: { vu32 v; memcpy(&v, a, 16); vf32 x = vfromint_s(c, v, fbits, uns); r = rv(&x); break; }
    case K_VFROMINT_D: { vu64 v; memcpy(&v, a, 16); vf64 x = vfromint_d(c, v, fbits, uns); r = rv(&x); break; }
    case K_VTOINT_S: { vu32 x = vtoint_s(c, v32(a), rm, fbits, uns); r = rv(&x); break; }
    case K_D2S: r.lo = b32(simd_d2s(c, f64(a[0]))); break;
    case K_S2D: r.lo = b64(simd_s2d(c, f32(a[0]))); break;
    case K_D2S_ODD: r.lo = b32(simd_d2s_odd(c, f64(a[0]))); break;
    }
    _mm_setcsr(saved);
    return r;
}

#else
R128 eval_ref(int k, int p, uint64_t fpcr, const uint64_t *a, const uint64_t *b, const uint64_t *d);
R128 eval_host(int k, int p, uint64_t fpcr, const uint64_t *a, const uint64_t *b, const uint64_t *d);

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd64(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static const uint32_t E32[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00400000, 0x00800000, 0x80800000,
    0x00800001, 0x3f800000, 0xbf800000, 0x3fc00000, 0xbfc00000, 0x40200000, 0xc0200000, 0x3f000000, 0xbf000000,
    0x3effffff, 0x3f7fffff, 0x3f800001, 0x4b000000, 0x4b7fffff, 0x4effffff, 0x4f000000, 0xcf000000, 0x4f800000,
    0x5effffff, 0x5f000000, 0x5f800000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00001,
    0x7f800001, 0xff812345, 0x34000000, 0x33ffffff, 0x0c000000, 0x1f800000, 0x40490fdb, 0x42c80000, 0x3eaaaaab};
static const uint64_t E64[] = {0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000fffffffffffffull,
    0x800fffffffffffffull, 0x0010000000000000ull, 0x8010000000000000ull, 0x0010000000000001ull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x3ff8000000000000ull, 0xbff8000000000000ull, 0x4004000000000000ull, 0xc004000000000000ull,
    0x3fe0000000000000ull, 0x3fdfffffffffffffull, 0x3fefffffffffffffull, 0x3ff0000000000001ull, 0x4330000000000000ull,
    0x433fffffffffffffull, 0x43dfffffffffffffull, 0x43e0000000000000ull, 0xc3e0000000000000ull, 0x43f0000000000000ull,
    0x41efffffffe00000ull, 0x41dfffffffc00000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000001ull, 0x7ff0000000000001ull, 0x3810000000000000ull,
    0x380fffffffffffffull, 0x3800000000000000ull, 0x36a0000000000000ull, 0x47efffffe0000000ull, 0x47efffffefffffffull,
    0x400921fb54442d18ull, 0x3fd5555555555555ull, 0x37f0000000000000ull, 0x380ffffff0000000ull};
#define NE32 (int)(sizeof E32 / sizeof *E32)
#define NE64 (int)(sizeof E64 / sizeof *E64)
static const uint64_t I64[] = {0, 1, 2, 3, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull, 0x00ffffff, 0x01000001,
    0x01000003, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x20000000000001ull,
    0x20000000000003ull, 0xfffffffffffff800ull, 0xffffffffffffffbfull, 0x8000000000000401ull, 0xfffffffeull, 0x80000001ull};
#define NI (int)(sizeof I64 / sizeof *I64)

static uint32_t pick32(void) {
    uint64_t r = rnd64();
    switch (r & 7) {
    case 0: case 1: case 2: return E32[(r >> 8) % NE32];
    case 3: return (uint32_t)(r >> 16) & 0x807fffff;                           // denormal
    case 4: return ((uint32_t)(r >> 16) & 0x80ffffff) | 0x00800000;           // near the smallest normal
    case 5: return ((uint32_t)(r >> 16) & 0x81ffffff) | 0x4a000000;           // near integers / halves
    default: return (uint32_t)(r >> 20);
    }
}
static uint64_t pick64(void) {
    uint64_t r = rnd64();
    switch (r & 7) {
    case 0: case 1: case 2: return E64[(r >> 8) % NE64];
    case 3: return rnd64() & 0x800fffffffffffffull;
    case 4: return (rnd64() & 0x801fffffffffffffull) | 0x0010000000000000ull;
    case 5: return (rnd64() & 0x803fffffffffffffull) | 0x4320000000000000ull;
    case 6: return (rnd64() & 0x800fffffffffffffull) | 0x3800000000000000ull;    // just below float range
    default: return rnd64();
    }
}
static uint64_t pickint(void) {
    uint64_t r = rnd64();
    if ((r & 3) == 0) return I64[(r >> 8) % NI];
    if ((r & 3) == 1) return -(r >> 40);
    return rnd64() >> (r >> 58);
}

// The FZ tininess case: a lane where one result is +-0 and the other +-smallest normal of the same sign.
static int tiny_lane(uint64_t x, uint64_t y, int w) {
    uint64_t sg = w == 32 ? 0x80000000u : 0x8000000000000000ull, minn = w == 32 ? 0x00800000u : 0x0010000000000000ull;
    if ((x & sg) != (y & sg)) return 0;
    x &= ~sg, y &= ~sg;
    return (x == 0 && y == minn) || (x == minn && y == 0);
}
static int tiny_only(R128 r, R128 h, int w) {
    uint64_t m = w == 32 ? 0xffffffffu : ~0ull;
    for (int i = 0; i < 128 / w; i++) {
        int sh = (i * w) & 63;
        uint64_t x = (((i * w) >= 64 ? r.hi : r.lo) >> sh) & m, y = (((i * w) >= 64 ? h.hi : h.lo) >> sh) & m;
        if (x != y && !tiny_lane(x, y, w)) return 0;
    }
    return 1;
}

int main(void) {
    static const char *kn[K_N] = {"op_s", "op_d", "vop_s", "vop_d", "cmp_s", "cmp_d", "vcmp_s", "vcmp_d", "frint_s",
        "frint_d", "vfrint_s", "vfrint_d", "toint_s", "toint_d", "fromint_s", "fromint_d", "vfromint_s", "vfromint_d",
        "vtoint_s", "d2s", "s2d", "d2s_odd"};
    long n = 0, bad = 0, tiny = 0, perk[K_N] = {0};
    // Known answers (ARM ARM pseudocode), for both builds: each bug the differential run found once.
    static const struct { int k, p; uint64_t fpcr, a, b, want; const char *what; } kat[] = {
        {K_OP_S, 11, 0x1000000, 0x00400000, 0xf6f5bb0e, 0, "FMAX under FZ flushes a denormal operand"},
        {K_VOP_S, 11, 0x1000000, 0x00400000, 0xf6f5bb0e, 0, "vector FMAX under FZ flushes a denormal operand"},
        {K_OP_S, 12, 0x1000000, 0x80400000, 0x3f800000, 0x80000000, "FMIN under FZ flushes to -0"},
        {K_OP_D, 11, 0x1000000, 0x0000000000000001ull, 0xbff0000000000000ull, 0, "double FMAX under FZ"},
        {K_FRINT_S, 4, 0x800000, 0x40200000, 0, 0x40400000, "FRINTA 2.5 -> 3 (RMode M)"},
        {K_FRINT_S, 4, 0x400000, 0xc0200000, 0, 0xc0400000, "FRINTA -2.5 -> -3 (RMode P)"},
        {K_FRINT_S, 4, 0x400000, 0x3effffff, 0, 0, "FRINTA 0.49999997 -> 0 (RMode P)"},
        {K_VFRINT_S, 4, 0x800000, 0x40200000, 0, 0x40400000, "vector FRINTA 2.5 -> 3 (RMode M)"},
        {K_FRINT_S, 0, 0x400000, 0x40200000, 0, 0x40000000, "FRINTN 2.5 -> 2 (RMode P)"},
        {K_VFRINT_D, 0, 0x400000, 0x4004000000000000ull, 0, 0x4000000000000000ull, "vector FRINTN 2.5 -> 2 (RMode P)"},
        {K_TOINT_S, 0, 0x400000, 0x40200000, 0, 2, "FCVTNS 2.5 -> 2 (RMode P)"},
        {K_TOINT_S, 4 | 1 << 9, 0x800000, 0x40200000, 0, 3, "FCVTAS 2.5 -> 3 (RMode M)"},
        {K_OP_S, 4, 0x400000, 0x3f800001, 0x3f800001, 0xbf800003, "FNMUL rounds the product up, then negates (RMode P)"},
        {K_FROMINT_D, 1 << 10 | 1 << 9, 0x800000, 0, 0, 0, "UCVTF 0 -> +0 (RMode M)"},
        {K_VFROMINT_S, 1 << 10, 0x800000, 0, 0, 0, "vector UCVTF 0 -> +0 (RMode M)"},
        {K_OP_S, 10, 0x400000, 0xff7fffff, 0x7f7fffff, 0xff7fffff, "FRSQRTS overflow -> -max (RMode P)"},
        {K_OP_S, 0, 0x400000, 0x3f800000, 0x33800000, 0x3f800001, "FADD 1 + 2^-24 rounds up (RMode P)"},
        {K_D2S, 0, 0xc00000, 0x3ff0000010000000ull, 0, 0x3f800000, "FCVT d->s truncates (RMode Z)"},
        {K_FROMINT_S, 1 << 9, 0x400000, 0x0000000001000001ull, 0, 0x4b800001, "SCVTF 2^24+1 rounds up (RMode P)"},
    };
    int katbad = 0;
    for (unsigned i = 0; i < sizeof kat / sizeof *kat; i++) {
        uint64_t a[2] = {kat[i].a, 0}, b[2] = {kat[i].b, 0}, d[2] = {0, 0};
        for (int h = 0; h < 2; h++) {
            R128 r = (h ? eval_host : eval_ref)(kat[i].k, kat[i].p, kat[i].fpcr, a, b, d);
            if (r.lo != kat[i].want) {
                printf("KAT FAIL (%s): %s: got %llx want %llx\n", h ? "host" : "shipped", kat[i].what,
                       (unsigned long long)r.lo, (unsigned long long)kat[i].want);
                katbad++;
            }
        }
    }
    printf("fpcr_host: %d known answers x 2 builds, %d wrong\n", (int)(sizeof kat / sizeof *kat), katbad);
    for (int fz = 0; fz < 2; fz++)
    for (int dn = 0; dn < 2; dn++)
    for (int rmode = 0; rmode < 4; rmode++) {
        uint64_t fpcr = (uint64_t)rmode << 22 | (uint64_t)fz << 24 | (uint64_t)dn << 25;
        for (int k = 0; k < K_N; k++)
        for (int it = 0; it < 6000; it++) {
            uint64_t a[2], b[2], d[2];
            int w = (k == K_OP_D || k == K_VOP_D || k == K_CMP_D || k == K_VCMP_D || k == K_FRINT_D || k == K_VFRINT_D ||
                     k == K_TOINT_D || k == K_D2S || k == K_D2S_ODD) ? 64 : 32;
            for (int i = 0; i < 2; i++) {
                if (w == 64) a[i] = pick64(), b[i] = pick64(), d[i] = pick64();
                else a[i] = pick32() | (uint64_t)pick32() << 32, b[i] = pick32() | (uint64_t)pick32() << 32,
                     d[i] = pick32() | (uint64_t)pick32() << 32;
            }
            if (k == K_FROMINT_S || k == K_FROMINT_D || k == K_VFROMINT_D) a[0] = pickint(), a[1] = pickint();
            if (k == K_VFROMINT_S) a[0] = (uint32_t)pickint() | pickint() << 32, a[1] = (uint32_t)pickint() | pickint() << 32;
            int p = 0;
            uint64_t r = rnd64();
            switch (k) {
            case K_OP_S: case K_OP_D: case K_VOP_S: case K_VOP_D: p = (int)(r % 15); break;
            case K_VCMP_S: case K_VCMP_D: p = (int)(r % 5); break;
            case K_FRINT_S: case K_FRINT_D: case K_VFRINT_S: case K_VFRINT_D: p = (int)(r % 7); break;
            case K_TOINT_S: case K_TOINT_D: case K_VTOINT_S: {
                static const int fb[] = {0, 0, 0, 1, 7, 16, 31};
                p = (int)(r % 5) | fb[(r >> 8) % 7] << 3 | (int)((r >> 16) & 3) << 9;
                break;
            }
            case K_FROMINT_S: case K_FROMINT_D: case K_VFROMINT_S: case K_VFROMINT_D: {
                static const int fb[] = {0, 0, 0, 1, 7, 16, 31};
                p = fb[(r >> 8) % 7] << 3 | (int)((r >> 16) & 3) << 9;
                if (k == K_VFROMINT_D) p |= 1 << 9;
                break;
            }
            }
            R128 x = eval_ref(k, p, fpcr, a, b, d), y = eval_host(k, p, fpcr, a, b, d);
            n++;
            if (x.lo == y.lo && x.hi == y.hi) continue;
            int fw = (k == K_CMP_S || k == K_CMP_D || k == K_VCMP_S || k == K_VCMP_D || (k >= K_TOINT_S && k <= K_TOINT_D) ||
                      k == K_VTOINT_S) ? 0 : (k == K_S2D ? 64 : w);
            if (k == K_FROMINT_D || k == K_VFROMINT_D) fw = 64;
            if (k == K_FROMINT_S || k == K_VFROMINT_S || k == K_D2S || k == K_D2S_ODD) fw = 32;
            if (fz && fw && k <= K_VOP_D && tiny_only(x, y, fw)) { tiny++; continue; }
            if (perk[k]++ < 4)
                printf("DIFF %s p=%d fpcr=%llx a=%016llx%016llx b=%016llx%016llx d=%016llx%016llx ref=%016llx%016llx host=%016llx%016llx\n",
                       kn[k], p, (unsigned long long)fpcr, (unsigned long long)a[1], (unsigned long long)a[0],
                       (unsigned long long)b[1], (unsigned long long)b[0], (unsigned long long)d[1], (unsigned long long)d[0],
                       (unsigned long long)x.hi, (unsigned long long)x.lo, (unsigned long long)y.hi, (unsigned long long)y.lo);
            bad++;
        }
    }
    for (int k = 0; k < K_N; k++) if (perk[k]) printf("  %s: %ld differ\n", kn[k], perk[k]);
    printf("fpcr_host: %ld cases, %ld differ, %ld FZ tininess (allowed)\n", n, bad, tiny);
    if (bad || katbad) { printf("FAIL\n"); return 1; }
    printf("OK\n");
    return 0;
}
#endif
