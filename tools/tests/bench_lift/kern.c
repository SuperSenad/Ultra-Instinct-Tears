// [UIT 0190] Game-like kernels compiled for AArch64 (as the Switch build would be), lifted by tools/lifter and run natively
// by tools/tests/bench_lift.py. Written for this project (no third-party code).
#include <stdint.h>
typedef struct { float x, y, z; } v3;
typedef struct { float m[12]; } m34;
typedef struct Actor { struct Actor *next; v3 pos, vel; float hp; uint32_t flags; const struct VT *vt; int id; } Actor;
typedef struct VT { float (*score)(const Actor *); int (*alive)(const Actor *); } VT;

__attribute__((noinline)) v3 xform(const m34 *m, v3 p) {
    v3 r = {m->m[0]*p.x + m->m[1]*p.y + m->m[2]*p.z + m->m[3], m->m[4]*p.x + m->m[5]*p.y + m->m[6]*p.z + m->m[7],
            m->m[8]*p.x + m->m[9]*p.y + m->m[10]*p.z + m->m[11]};
    return r;
}
// 1. transform a point array (animation / physics): calls a small leaf per element
uint64_t k_xform(const m34 *m, v3 *pts, int n) {
    float acc = 0;
    for (int i = 0; i < n; i++) { v3 q = xform(m, pts[i]); pts[i] = q; acc += q.x + q.y * 0.5f - q.z; }
    union { float f; uint32_t u; } u = {acc}; return u.u;
}
// 2. walk an actor list, integrate, virtual calls (C++ style)
float sc_a(const Actor *a) { return a->hp * 2.0f + a->pos.y; }
float sc_b(const Actor *a) { return a->hp - a->vel.x * a->vel.x; }
int al_a(const Actor *a) { return a->hp > 0.0f; }
int al_b(const Actor *a) { return (a->flags & 3) != 0 && a->hp > -5.0f; }
uint64_t k_actors(Actor *head, float dt) {
    float best = -1e30f; int bid = -1, alive = 0;
    for (Actor *a = head; a; a = a->next) {
        a->pos.x += a->vel.x * dt; a->pos.y += a->vel.y * dt; a->pos.z += a->vel.z * dt;
        if (a->pos.y < 0) { a->pos.y = 0; a->vel.y = -a->vel.y * 0.5f; a->flags |= 4; }
        if (!a->vt->alive(a)) continue;
        alive++;
        float s = a->vt->score(a);
        if (s > best) { best = s; bid = a->id; }
    }
    return (uint64_t)(uint32_t)bid << 32 | (uint32_t)alive;
}
// 3. hash table of names (resource lookups): FNV hash + open addressing + strcmp
static uint32_t hstr(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
typedef struct { const char *key; uint32_t val; uint32_t hash; } Ent;
uint64_t k_hash(Ent *tab, uint32_t mask, const char *const *q, int nq) {
    uint64_t sum = 0;
    for (int i = 0; i < nq; i++) {
        uint32_t h = hstr(q[i]);
        for (uint32_t j = h & mask;; j = (j + 1) & mask) {
            if (!tab[j].key) break;
            if (tab[j].hash == h && seq(tab[j].key, q[i])) { sum += tab[j].val; break; }
        }
    }
    return sum;
}
// 4. sort with a comparator through a function pointer (qsort-style, blr)
typedef int (*cmpf)(const void *, const void *);
int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }
uint64_t k_sort(uint32_t *v, int n, cmpf cmp) {
    for (int i = 1; i < n; i++) { uint32_t t = v[i]; int j = i - 1; while (j >= 0 && cmp(&v[j], &t) > 0) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
    uint64_t h = 0; for (int i = 0; i < n; i++) h = h * 31 + v[i]; return h;
}
// 5. integer / bit work (decompression-like): LZ-style byte copy with bit flags
uint64_t k_bits(const uint8_t *src, int n, uint8_t *dst) {
    uint32_t bits = 0; int nb = 0, o = 0;
    for (int i = 0; i < n && o < 60000;) {
        if (!nb) { bits = src[i++]; nb = 8; }
        if (bits & 1) dst[o++] = src[i++];
        else { int d = (src[i] | (src[i + 1] & 0xf0) << 4) + 1, l = (src[i + 1] & 15) + 3; i += 2; if (d > o) d = o ? o : 1; for (int k = 0; k < l && o < 60000; k++, o++) dst[o] = o >= d ? dst[o - d] : 0; }
        bits >>= 1; nb--;
    }
    uint64_t h = 1469598103934665603ull; for (int k = 0; k < o; k++) h = (h ^ dst[k]) * 1099511628211ull; return h;
}
// 6. call-heavy getters / small methods (C++ accessor style)
__attribute__((noinline)) int get_flags(const Actor *a) { return a->flags; }
__attribute__((noinline)) float get_hp(const Actor *a) { return a->hp; }
__attribute__((noinline)) int is_enemy(const Actor *a) { return (get_flags(a) & 2) != 0; }
__attribute__((noinline)) float threat(const Actor *a) { return is_enemy(a) ? get_hp(a) * 1.5f : 0.0f; }
uint64_t k_calls(Actor *arr, int n, int rounds) {
    float t = 0;
    for (int r = 0; r < rounds; r++) for (int i = 0; i < n; i++) t += threat(&arr[i]);
    union { float f; uint32_t u; } u = {t}; return u.u;
}
