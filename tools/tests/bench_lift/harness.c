/* [UIT 0190] Host side of tools/tests/bench_lift.py: sets up the kernels inputs, calls the lifted code, prints the best time of each kernel and a hash of its result (equal across code generations). */
#include "cpu.h"
#include "syms.h"   /* written by tools/tests/bench_lift.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t l, uint64_t x) { (void)a; (void)l; (void)x; }
void rt_dispatch(Cpu *c) {
#define X(a, f) case a: f(c); return;
    switch (c->pc) { FN_TABLE }
    printf("bad dispatch %llx\n", (unsigned long long)c->pc); exit(3);
}
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; exit(8); }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; exit(4); }
void rt_udf(Cpu *c, uint64_t p) { (void)c; printf("udf %llx\n", (unsigned long long)p); exit(5); }
void rt_bad_entry(Cpu *c, uint64_t a, uint64_t f) { (void)c; (void)a; (void)f; exit(6); }
void rt_spin_gate(Cpu *c, uint64_t h) { (void)c; (void)h; }
uint64_t rt_ticks(void) { return 0; }

typedef struct { float x, y, z; } v3;
typedef struct { uint64_t score, alive; } VT;
typedef struct Actor { struct Actor *next; v3 pos, vel; float hp; uint32_t flags; const VT *vt; int id; } Actor;
typedef struct { const char *key; uint32_t val; uint32_t hash; } Ent;
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static float rf(void) { return (float)(rnd() % 20000) / 100.0f - 100.0f; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static uint8_t stack[1 << 20] __attribute__((aligned(16)));
static Cpu cpu;
static uint64_t call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, float s0) {
    Cpu *c = &cpu;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->sp = (uint64_t)(uintptr_t)(stack + sizeof stack - 256);
    memcpy(&c->v[0], &s0, 4); c->x[30] = 0; c->pc = fn;
    rt_dispatch(c);
    return c->x[0];
}
static uint32_t hstr(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
#define NP 1024
#define NA 512
#define NH 4096
#define NQ 2048
#define NS 200
#define NB 32768
static float mat[12]; static v3 pts[NP], pts0[NP];
static Actor act[NA], act0[NA]; static VT vta, vtb;
static Ent tab[NH]; static char names[NH / 2][24]; static const char *qs[NQ]; static char missq[NQ][24];
static uint32_t sv[NS], sv0[NS];
static uint8_t src[NB], dst[70000];
int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 30;
    const char *only = argc > 2 ? argv[2] : 0;
    for (int i = 0; i < 12; i++) mat[i] = rf() / 50;
    for (int i = 0; i < NP; i++) pts0[i] = (v3){rf(), rf(), rf()};
    vta = (VT){A_sc_a, A_al_a}; vtb = (VT){A_sc_b, A_al_b};
    for (int i = 0; i < NA; i++) {
        act0[i] = (Actor){i + 1 < NA ? &act[i + 1] : 0, {rf(), rf(), rf()}, {rf(), rf(), rf()}, rf(), (uint32_t)rnd(), (rnd() & 1) ? &vta : &vtb, i};
    }
    for (int i = 0; i < NH / 2; i++) {
        snprintf(names[i], 24, "res/%c%c/actor_%u.bfres", 'a' + (int)(rnd() % 26), 'a' + (int)(rnd() % 26), (unsigned)(rnd() % 100000));
        uint32_t h = hstr(names[i]);
        for (uint32_t j = h & (NH - 1);; j = (j + 1) & (NH - 1)) if (!tab[j].key) { tab[j] = (Ent){names[i], i * 7u + 1, h}; break; }
    }
    for (int i = 0; i < NQ; i++) {
        if (i % 4 == 3) { snprintf(missq[i], 24, "res/zz/miss_%d.bfres", i); qs[i] = missq[i]; }
        else qs[i] = names[rnd() % (NH / 2)];
    }
    for (int i = 0; i < NS; i++) sv0[i] = (uint32_t)rnd();
    for (int i = 0; i < NB; i++) src[i] = (i % 3 == 0) ? (uint8_t)(rnd() | 0x81) : (uint8_t)rnd();
    static const char *nm[] = {"xform", "actors", "hash", "sort", "bits", "calls"};
    double tot = 0;
    for (int k = 0; k < 6; k++) {
        if (only && strcmp(only, nm[k])) continue;
        double best = 1e30; uint64_t r = 0;
        for (int rep = 0; rep < reps; rep++) {
            memcpy(pts, pts0, sizeof pts); memcpy(act, act0, sizeof act); memcpy(sv, sv0, sizeof sv);
            double t0 = now();
            switch (k) {
            case 0: r = call(A_k_xform, (uintptr_t)mat, (uintptr_t)pts, NP, 0, 0); r ^= call(A_k_xform, (uintptr_t)mat, (uintptr_t)pts, NP, 0, 0); break;
            case 1: r = 0; for (int it = 0; it < 8; it++) r = r * 31 + call(A_k_actors, (uintptr_t)act, 0, 0, 0, 0.016f); break;
            case 2: r = call(A_k_hash, (uintptr_t)tab, NH - 1, (uintptr_t)qs, NQ, 0); break;
            case 3: r = call(A_k_sort, (uintptr_t)sv, NS, A_cmp_u32, 0, 0); break;
            case 4: r = call(A_k_bits, (uintptr_t)src, NB - 4, (uintptr_t)dst, 0, 0); break;
            case 5: r = call(A_k_calls, (uintptr_t)act, NA, 16, 0, 0); break;
            }
            double t = now() - t0;
            if (t < best) best = t;
        }
        printf("%-7s %10.1f us  result %016llx\n", nm[k], best / 1000, (unsigned long long)r);
        tot += best;
    }
    printf("total   %10.1f us\n", tot / 1000);
    return 0;
}
