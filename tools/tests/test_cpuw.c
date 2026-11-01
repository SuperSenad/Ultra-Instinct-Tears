// [UIT 0143] Box test for src/gfx/vkb_cpuw.h (vkb_image.cpu_written between WriteTexels and the uploading thread):
// a "game" thread writes generation g into a 64-word texture memory and marks; an "uploader" thread takes the flag and
// copies the memory, as tex_upload does. However the two interleave, after the game thread's last mark the uploader
// must end with the last generation (no lost write), and every copy it takes must be at least the generation of the
// mark it consumed. 200 rounds of 20000 writes; also under TSan (the memory words are relaxed atomics standing for
// guest memory, so the only ordering is the flag's).
// cc -O2 -pthread -Isrc/gfx tools/tests/test_cpuw.c -o /tmp/t/cpuw && /tmp/t/cpuw
#include <pthread.h>
#include <stdio.h>
#include "vkb_cpuw.h"

enum { WORDS = 64, WRITES = 20000, ROUNDS = 200 };
static unsigned mem[WORDS], marked;   // marked: the generation of the last mark (game thread), read after a take
static int flag, done;
static unsigned bad, lost;

static void *game(void *p) {
    (void)p;
    for (unsigned g = 1; g <= WRITES; g++) {
        for (int i = 0; i < WORDS; i++) __atomic_store_n(&mem[i], g, __ATOMIC_RELAXED);
        __atomic_store_n(&marked, g, __ATOMIC_RELAXED);
        vkb_cpuw_mark(&flag);
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static unsigned copy_min(void) {
    unsigned m = ~0u;
    for (int i = 0; i < WORDS; i++) { unsigned v = __atomic_load_n(&mem[i], __ATOMIC_RELAXED); if (v < m) m = v; }
    return m;
}
static void *uploader(void *p) {
    unsigned *last = p;
    for (;;) {
        int fin = __atomic_load_n(&done, __ATOMIC_ACQUIRE);
        if (vkb_cpuw_peek(&flag) && vkb_cpuw_take(&flag)) {
            unsigned want = __atomic_load_n(&marked, __ATOMIC_RELAXED), got = copy_min();
            if (got < want) __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
            *last = got;
        }
        if (fin && !vkb_cpuw_peek(&flag)) break;
    }
    return NULL;
}
int main(void) {
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < WORDS; i++) mem[i] = 0;
        flag = 0; done = 0; marked = 0;
        unsigned last = 0;
        pthread_t a, b;
        pthread_create(&b, NULL, uploader, &last);
        pthread_create(&a, NULL, game, NULL);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        if (last != WRITES) lost++;
    }
    printf("%d rounds of %d marked writes: %u copies older than the mark they took, %u rounds ending on an old copy\n",
           ROUNDS, WRITES, bad, lost);
    printf(bad || lost ? "FAILED\n" : "ok\n");
    return bad || lost;
}
