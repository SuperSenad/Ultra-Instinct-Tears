// [UIT 0054] nvn_gpu_write_mark from several threads at once (the retire thread and the translation thread's image
// copy back): every mark is counted (the sequence number is one per mark), each mark is seen by nvn_gpu_written_since
// right after it, and GiB tables made by racing writers are not lost.
// cc -O2 -pthread tools/tests/test_gw_mark.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t/gw && /tmp/t/gw
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "../../src/gfx/nvn/nvn_dirty.h"

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {   // no ranges watched here
    (void)addr; (void)size; (void)pages; (void)max; *page = 4096; return 0;
}
enum { THREADS = 4, MARKS = 20000 };
static int g_bad[THREADS];
static void *run(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < MARKS; i++) {
        // distinct pages per thread, spread over many GiBs (tables made while the others mark)
        uintptr_t a = ((uintptr_t)(i % 512) << 30) + ((uintptr_t)id << 24) + ((uintptr_t)(i / 512) << 12);
        uint64_t before = nvn_gpu_write_now();
        nvn_gpu_write_mark((const void *)a, 4096);
        if (!nvn_gpu_written_since((const void *)a, 4096, before)) g_bad[id]++;
    }
    return NULL;
}
int main(void) {
    uint64_t s0 = nvn_gpu_write_now();
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, run, (void *)(intptr_t)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    int bad = 0;
    for (int i = 0; i < THREADS; i++) bad += g_bad[i];
    uint64_t marks = nvn_gpu_write_now() - s0;
    int lost = 0;   // every page marked is still found (a GiB table made twice would lose one writer's pages)
    for (int id = 0; id < THREADS; id++)
        for (int i = 0; i < MARKS; i++) {
            uintptr_t a = ((uintptr_t)(i % 512) << 30) + ((uintptr_t)id << 24) + ((uintptr_t)(i / 512) << 12);
            if (!nvn_gpu_written_since((const void *)a, 4096, s0)) lost++;
        }
    printf("%llu marks counted of %d, %d not seen right after, %d lost\n", (unsigned long long)marks, THREADS * MARKS, bad, lost);
    if (marks != (uint64_t)THREADS * MARKS || bad || lost) { printf("FAIL\n"); return 1; }
    printf("ok\n");
    return 0;
}
