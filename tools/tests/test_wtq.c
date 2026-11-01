// [UIT 0256] TOTK_VK_WRITETEXELS_QUEUED: small WriteTexels queued to the translation thread instead of x_drain + write.
//  1. wtq_pick: truth table; wtq_src_bytes: equal to the highest byte mtl_write_texels reads (brute force over its loops).
//  2. The queue (a copy of hle_nvn.c's x_push / x_thread: XQ jobs, one mutex, one condition), a game thread and the
//     translation thread for real: the game submits (the translation thread reads the "texture memory" when it
//     replays) and writes texels, then reuses its source buffer at once. Old: drain, then write on the game thread.
//     New: the write is a job holding a copy of the data. Each submission must see exactly the writes the game made
//     before submitting it (the old order); the new way must not wait.
//  TOTK_PROBE_NEGATIVE=1: the write lands in memory at once without the queue -> must FAIL (queued frames see it early).
//  TOTK_PROBE_NEGATIVE=2: the job keeps a pointer to the caller's buffer instead of a copy -> must FAIL.
#include "../../src/gfx/nvn/nvn_wtq.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks, g_neg;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 8) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void pure(void) {
    for (int on = 0; on < 2; on++) for (int as = 0; as < 2; as++) for (int x = 0; x < 2; x++)
        for (uint64_t b = 0; b <= WTQ_LEVEL_MAX + 1; b += (b < 4 ? 1 : WTQ_LEVEL_MAX / 2 - 1)) {
            int want = on && as && !x && b && b <= WTQ_LEVEL_MAX;
            CHECK(wtq_pick(on, as, x, b) == want, "pick %d %d %d %llu", on, as, x, (unsigned long long)b);
        }
    CHECK(wtq_pick(1, 1, 0, WTQ_LEVEL_MAX) && !wtq_pick(1, 1, 0, WTQ_LEVEL_MAX + 1), "cap edge");
    srand(7);
    for (int k = 0; k < 20000; k++) {
        size_t wb = 1 + (size_t)rand() % 64; uint32_t by = 1 + (uint32_t)rand() % 9, nz = 1 + (uint32_t)rand() % 5;
        size_t row = wb + (size_t)rand() % 16, img = row * by + (size_t)rand() % 32;
        size_t hi = 0;   // the loops of mtl_write_texels: src + z * img + y * row, wb bytes
        for (uint32_t z = 0; z < nz; z++) for (uint32_t y = 0; y < by; y++) { size_t e = z * img + y * row + wb; if (e > hi) hi = e; }
        CHECK(wtq_src_bytes(wb, by, nz, row, img) == hi, "src bytes wb %zu by %u nz %u row %zu img %zu", wb, by, nz, row, img);
    }
    CHECK(wtq_src_bytes(0, 1, 1, 4, 4) == 0 && wtq_src_bytes(4, 0, 1, 4, 4) == 0, "empty");
}

// ---- the queue model ----
enum { XQ = 64, NOPS = 20000, TEXB = 16 };
enum { J_SUBMIT = 1, J_CALL };
typedef struct { int kind; uint64_t sub; void (*fn)(void *); void *arg; } job;
static job q[XQ];
static uint64_t q_in, q_done;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned char texmem[TEXB];            // the game memory under the texture (written by whoever does the write)
static unsigned char *seen;                   // seen[sub]: texmem[0] when the translation thread replayed submission sub
static unsigned char *expect;                 // the writes the game made before that submission (program order)
static int stop;

static uint64_t push(job j) {
    pthread_mutex_lock(&mu);
    while (q_in - q_done >= XQ) pthread_cond_wait(&cv, &mu);
    q[q_in % XQ] = j; uint64_t no = ++q_in;
    pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return no;
}
static void drain(uint64_t *waits) {
    pthread_mutex_lock(&mu); uint64_t want = q_in;
    if (q_done < want) ++*waits;
    while (q_done < want) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}
static void *xthread(void *u) {
    (void)u;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (q_done == q_in && !stop) pthread_cond_wait(&cv, &mu);
        if (q_done == q_in && stop) { pthread_mutex_unlock(&mu); return NULL; }
        job j = q[q_done % XQ];
        pthread_mutex_unlock(&mu);
        if (j.kind == J_SUBMIT) { for (volatile int s = 0; s < (int)(j.sub % 7) * 40; s++) {} seen[j.sub] = texmem[0]; }   // replay reads the texture
        else j.fn(j.arg);
        pthread_mutex_lock(&mu); q_done++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    }
}
typedef struct { size_t n; const unsigned char *ref; unsigned char src[]; } wjob;
static void write_mem(const unsigned char *src, size_t n) { memcpy(texmem, src, n); }
static void wjob_run(void *p) { wjob *w = p; write_mem(g_neg == 2 ? w->ref : w->src, w->n); free(w); }

static void run(int queued, unsigned seed, uint64_t *waits) {
    memset(texmem, 0, sizeof texmem); q_in = q_done = 0; stop = 0;
    seen = calloc(NOPS + 1, 1); expect = calloc(NOPS + 1, 1);
    pthread_t t; pthread_create(&t, NULL, xthread, NULL);
    unsigned char buf[TEXB], val = 0; uint64_t sub = 0;
    for (int i = 0; i < NOPS; i++) {
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 4) { expect[++sub] = val; push((job){J_SUBMIT, sub, NULL, NULL}); continue; }   // the game submits a frame
        val++; memset(buf, val, sizeof buf);                                                             // WriteTexels(val)
        if (!queued) { drain(waits); write_mem(buf, sizeof buf); }
        else if (g_neg == 1) write_mem(buf, sizeof buf);
        else {
            wjob *w = malloc(sizeof *w + sizeof buf); w->n = sizeof buf; w->ref = buf; memcpy(w->src, buf, sizeof buf);
            push((job){J_CALL, 0, wjob_run, w});
        }
        memset(buf, 0xEE, sizeof buf);   // the caller reuses its buffer once the call returns
    }
    pthread_mutex_lock(&mu); stop = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_join(t, NULL);
    int bad = 0;
    for (uint64_t s = 1; s <= sub; s++) if (seen[s] != expect[s]) bad++;
    CHECK(bad == 0, "%s seed %u: %d of %llu submissions saw other texels than the old order", queued ? "queued" : "old", seed, bad, (unsigned long long)sub);
    free(seen); free(expect);
}

int main(void) {
    const char *ng = getenv("TOTK_PROBE_NEGATIVE"); g_neg = ng ? atoi(ng) : 0;
    pure();
    uint64_t wold = 0, wnew = 0;
    for (unsigned s = 1; s <= 12; s++) { run(0, s, &wold); run(1, s, &wnew); }
    CHECK(wnew == 0, "the queued way waited %llu times", (unsigned long long)wnew);
    printf("  game-thread waits for the queue over 12 runs x %d operations: old %llu, queued %llu\n", NOPS, (unsigned long long)wold, (unsigned long long)wnew);
    printf("test_wtq%s: %d checks, %d failed\n", g_neg ? " (negative probe)" : "", checks, fails);
    return fails != 0;
}
