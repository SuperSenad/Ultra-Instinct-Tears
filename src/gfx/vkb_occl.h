// [UIT 0049/0050] NVN SAMPLES_PASSED counters (ResetCounter / ReportCounter) for TOTK_VK_OCCLUSION.
// NVN keeps one samples-passed counter per queue: ResetCounter(SAMPLES_PASSED) zeroes it, every draw after it adds the
// samples that passed the depth/stencil tests, and ReportCounter(SAMPLES_PASSED, addr) writes the running total to
// NVNcounterData.counter at addr. The game compares reports (SetRenderEnableConditional, or on the CPU) to hide objects
// that are behind others. Until 0049 the port wrote 1<<20 for every report: two reports compared "not equal" are then
// equal, and what they gate is hidden although it is in view.
//   visible (0049, cheap): each report writes a different, growing value (1<<20, then 4096 more each time): reads as
//                "visible" whether the game tests one report against zero or two reports against each other (also in
//                the low 32 bits: later minus earlier is 4096 per report there too).
//   gpu (0050, faithful):  the draws after a samples-passed ResetCounter each run inside a Vulkan occlusion query; when
//                the submission retires, a report gets the sum of the queries between that reset and the report (plus
//                what earlier submissions added since the reset), as on the Switch.
// Plain C, no Vulkan: the walk side runs on the translation thread, the retire side on the retire thread (box test:
// tools/tests/test_occl.c).
#pragma once
#include <stdint.h>

#define OQ_CAP 16384u            // queried draws per submission (a slot's query pool)
#define OQ_NONE 0xFFFFFFFFu

// ---- recording (the walk, or the one replaying thread): which draws are queried, and what each report sums
typedef struct {
    uint32_t n;          // queries handed out in this submission (0..OQ_CAP)
    uint32_t win0;       // the open window's first query in this submission (when win_here)
    uint32_t last_reset; // query count at this submission's last samples-passed reset, OQ_NONE: none
    int armed;           // a reset was seen: draws are counted from it on
    int win_here;        // the open window began in this submission (else it carries in from earlier ones)
    int bad;             // the open window lost draws (OQ_CAP reached): its reports are not exact
    uint64_t seq[OQ_CAP];// the command position (t_seq) of each queried draw, ascending
} oq_walk;

static inline void oq_sub_begin(oq_walk *w) {   // a new submission: its own queries; an open window carries in
    w->n = 0; w->win0 = 0; w->win_here = 0; w->last_reset = OQ_NONE;
}
static inline void oq_reset(oq_walk *w) {       // ResetCounter(SAMPLES_PASSED)
    w->armed = 1; w->win_here = 1; w->win0 = w->n; w->last_reset = w->n; w->bad = 0;
}
static inline int oq_draw(oq_walk *w, uint64_t seq) {   // a draw that will be replayed: its query, or -1
    if (!w->armed) return -1;
    if (w->n >= OQ_CAP) { w->bad = 1; return -1; }
    w->seq[w->n] = seq;
    return (int)w->n++;
}
// ReportCounter(SAMPLES_PASSED): 1 and the queries [*qa, *qb) of this submission (plus the carried-in total when *carry)
// when the value can be counted; 0 when not (no reset seen yet, or draws lost): the caller writes a "visible" value.
static inline int oq_report(const oq_walk *w, uint32_t *qa, uint32_t *qb, int *carry) {
    if (!w->armed || w->bad) return 0;
    *qa = w->win_here ? w->win0 : 0; *qb = w->n; *carry = !w->win_here;
    return 1;
}
// The query a chunk's draw got from the walk (binary search of seq[0..n), n = the count when the chunk was cut), or -1.
static inline int oq_find(const oq_walk *w, uint32_t n, uint64_t seq) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2;
        if (w->seq[m] < seq) lo = m + 1; else hi = m;
    }
    return lo < n && w->seq[lo] == seq ? (int)lo : -1;
}

// ---- retiring (in submission order): results are (value, availability) pairs, as vkGetQueryPoolResults gives them with
// VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT. A query never begun (its draw returned early) counts 0.
typedef struct { uint64_t carry; uint64_t fake; } oq_acc;   // the open window's total before this submission; "visible" reports so far
static inline uint64_t oq_sum(const uint64_t *res, uint32_t a, uint32_t b) {
    uint64_t s = 0;
    for (uint32_t i = a; i < b; i++) if (res[2 * i + 1]) s += res[2 * i];
    return s;
}
static inline uint64_t oq_value(const oq_acc *acc, const uint64_t *res, uint32_t qa, uint32_t qb, int carry) {
    return (carry ? acc->carry : 0) + oq_sum(res, qa, qb);
}
// The end of a submission that handed out n queries: the open window's total for the next one.
static inline void oq_job_end(oq_acc *acc, const uint64_t *res, uint32_t last_reset, uint32_t n) {
    if (last_reset != OQ_NONE) acc->carry = oq_sum(res, last_reset, n);
    else acc->carry += oq_sum(res, 0, n);
}
// TOTK_VK_OCCLUSION=visible (and reports gpu cannot count): every report differs from every earlier one and is large.
static inline uint64_t oq_visible(oq_acc *acc) { return (1ull << 20) + (acc->fake++ << 12); }

// [UIT 0052] TOTK_VK_COND_DRAWS (diagnostic): 0 off, 1 draws only (0048), 2 draws and clears.
static inline int cond_mode_parse(const char *e) { return !e ? 0 : *e == '1' ? 1 : *e == '2' ? 2 : 0; }
// 1: the command is skipped. render_off: 0 rendering on, 1 SetRenderEnable(false) (always obeyed), 2 a conditional
// evaluated off (obeyed unless the mode forces this kind of command).
static inline int cond_skip(int render_off, int mode, int is_clear) {
    if (!render_off) return 0;
    if (render_off != 2) return 1;
    return is_clear ? mode < 2 : mode < 1;
}
// [UIT 0052] the short occlusion log line: present n when it is due (every 0 = never)
static inline int occl_log_due(uint64_t n, uint64_t every) { return every && n && n % every == 0; }
