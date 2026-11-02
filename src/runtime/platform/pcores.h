// [UIT 0033] Hybrid CPUs (Intel 12th gen and later: performance and efficiency cores): the CPU sets of the fastest
// class. cls[i] / ids[i]: each logical CPU's efficiency class (higher: faster) and CPU set id (GetSystemCpuSetInformation).
// Returns how many went into out (the highest class), or 0 when every CPU has the same class (not hybrid: nothing to
// prefer) or n is 0. Pure logic: tools/tests/test_pcores.c.
#ifndef UIT_PCORES_H
#define UIT_PCORES_H
#include <stdint.h>
static inline int pcore_pick(const uint8_t *cls, const uint32_t *ids, int n, uint32_t *out) {
    if (n <= 0) return 0;
    uint8_t lo = cls[0], hi = cls[0];
    for (int i = 1; i < n; i++) { if (cls[i] < lo) lo = cls[i]; if (cls[i] > hi) hi = cls[i]; }
    if (lo == hi) return 0;
    int k = 0;
    for (int i = 0; i < n; i++) if (cls[i] == hi) out[k++] = ids[i];
    return k;
}
// [UIT 0089] The CPU sets for background work (TOTK_HYBRID_PLACEMENT=1): the class just below the fastest, the
// ordinary efficiency cores. On CPUs with three classes (low-power E-cores, E-cores, P-cores) the low-power ones are
// left out: they are slow enough that a background job waited for could hold things up. 0 when not hybrid.
static inline int ecore_pick(const uint8_t *cls, const uint32_t *ids, int n, uint32_t *out) {
    if (n <= 0) return 0;
    uint8_t hi = cls[0];
    for (int i = 1; i < n; i++) if (cls[i] > hi) hi = cls[i];
    int have = 0; uint8_t next = 0;
    for (int i = 0; i < n; i++) if (cls[i] < hi && (!have || cls[i] > next)) { next = cls[i]; have = 1; }
    if (!have) return 0;
    int k = 0;
    for (int i = 0; i < n; i++) if (cls[i] == next) out[k++] = ids[i];
    return k;
}
// [UIT 0237] TOTK_FRAME_CORE: one physical performance core for the game's frame thread (the one that acquires and
// presents). core[i]: each logical CPU's physical core (CpuSet.CoreIndex). Of the fastest class, the physical core with
// the highest index (core 0 takes more of Windows' interrupts) goes to fr (both of its hyperthreads), every other
// fastest-class CPU to rest. Returns how many went into rest; 0 (nothing split, fr empty) when not hybrid or when fewer
// than 4 physical fastest-class cores exist (too few left for the translation and recording threads). Pure logic:
// tools/tests/test_pcores.c.
static inline int frame_core_split(const uint8_t *cls, const uint32_t *ids, const uint8_t *core, int n, uint32_t *fr,
                                   int *nfr, uint32_t *rest) {
    *nfr = 0;
    if (n <= 0) return 0;
    uint8_t lo = cls[0], hi = cls[0];
    for (int i = 1; i < n; i++) { if (cls[i] < lo) lo = cls[i]; if (cls[i] > hi) hi = cls[i]; }
    if (lo == hi) return 0;
    uint8_t seen[256] = {0};
    int phys = 0, top = -1;
    for (int i = 0; i < n; i++) {
        if (cls[i] != hi) continue;
        if (!seen[core[i]]) { seen[core[i]] = 1; phys++; }
        if ((int)core[i] > top) top = core[i];
    }
    if (phys < 4) return 0;
    int kf = 0, kr = 0;
    for (int i = 0; i < n; i++) {
        if (cls[i] != hi) continue;
        if (core[i] == top) fr[kf++] = ids[i]; else rest[kr++] = ids[i];
    }
    *nfr = kf;
    return kr;
}
// [UIT 0358] TOTK_VK_RECORD_CORES: of the CPUs the P-core threads share (ids, each with its physical core in core),
// the want physical cores with the highest index go to the GPU command (recording) threads (rec, both hyperthreads
// of each), the others to the game's and the other graphics threads (rest), so a recording thread never runs on a game
// thread's hyperthread twin. Returns how many went into rest; 0 (nothing split, rec empty) when want is 0 or fewer
// than 3 physical cores would be left. Pure logic: tools/tests/test_pcores.c.
static inline int rec_core_split(const uint32_t *ids, const uint8_t *core, int n, int want, uint32_t *rec, int *nrec,
                                 uint32_t *rest) {
    *nrec = 0;
    if (n <= 0 || want <= 0) return 0;
    uint8_t seen[256] = {0}, mine[256] = {0};
    int phys = 0;
    for (int i = 0; i < n; i++) if (!seen[core[i]]) { seen[core[i]] = 1; phys++; }
    if (phys - want < 3) return 0;
    for (int c = 255, got = 0; c >= 0 && got < want; c--) if (seen[c]) { mine[c] = 1; got++; }
    int kk = 0, kr = 0;
    for (int i = 0; i < n; i++) {
        if (mine[core[i]]) rec[kk++] = ids[i]; else rest[kr++] = ids[i];
    }
    *nrec = kk;
    return kr;
}
#endif
