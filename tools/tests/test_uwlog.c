// [UIT 0335] vkb_uwlog.h: CPU reads of uniform memory see the GPU writes before their position in the submission.
#include <pthread.h>
#include <stdio.h>
#include "vkb_uwlog.h"
static int bad;
#define CHK(c) do { if (!(c)) { printf("line %d: %s\n", __LINE__, #c); bad = 1; } } while (0)
static uwlog L;
typedef struct { uint64_t ptr; uint32_t size; } pair;
static uint64_t rd_ptr(uwl_view *v, uint64_t seq, uint64_t base_ptr) {   // a c6+0 read over a stale base
    uint8_t b[12]; memcpy(b, &base_ptr, 8); memset(b + 8, 0, 4);
    uwl_read(&L, v, seq, 0x10c0354700, b, 12);
    uint64_t p; memcpy(&p, b, 8); return p;
}
static void *reader(void *arg) {   // a recording thread: its own positions, in order
    uwl_view v = {0};
    int from = *(int *)arg;
    for (int d = from; d < from + 100; d++) if (rd_ptr(&v, 4 * (uint64_t)d + 3, 0xdead) != 0x103e000000ull + (uint64_t)d * 0x1000) bad = 1;
    uwl_view_free(&v);
    return NULL;
}
int main(void) {
    uwl_view v = {0};
    uwl_reset(&L);
    // the Depths pattern: c6 rewritten (48 bytes) before every ground draw, at positions 2, 6, 10, ...
    for (int d = 0; d < 441; d++) {
        uint8_t w[48] = {0};
        uint64_t p = 0x103e000000ull + (uint64_t)d * 0x1000; uint32_t sz = 1000000u + (uint32_t)d;
        memcpy(w, &p, 8); memcpy(w + 8, &sz, 4);
        CHK(uwl_add(&L, 4 * (uint64_t)d + 2, 0x10c0354700, 48, w));
    }
    CHK(rd_ptr(&v, 1, 0xdead) == 0xdead);                          // before the first write: the base
    CHK(rd_ptr(&v, 3, 0xdead) == 0x103e000000ull);                 // draw 0
    CHK(rd_ptr(&v, 4 * 200 + 3, 0xdead) == 0x103e000000ull + 200 * 0x1000ull);
    CHK(rd_ptr(&v, 4 * 440 + 3, 0xdead) == 0x103e000000ull + 440 * 0x1000ull);
    CHK(rd_ptr(&v, 7, 0xdead) == 0x103e000000ull + 0x1000);       // a reader going back (another chunk): rebuilt
    { uint8_t b[16]; memset(b, 0xAA, 16);                            // bytes not written keep the base
      size_t got = uwl_read(&L, &v, 10000, 0x10c03546f8, b, 16);
      CHK(got == 8 && b[0] == 0xAA && b[7] == 0xAA && b[8] == 0x00 && b[9] == 0x80); }
    { uint8_t big[300]; for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;   // across a page edge
      CHK(uwl_add(&L, 5000, 0x2000f0, 300, big));
      uint8_t b[4] = {0}; CHK(uwl_read(&L, &v, 5000, 0x2000f0 + 254, b, 4) == 4 && b[0] == 254 && b[3] == 1); }
    // several readers while the writer has finished
    int from[4] = {0, 100, 200, 300}; pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, reader, &from[i]);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    // a new submission: nothing from the old one
    uwl_reset(&L);
    CHK(rd_ptr(&v, 4 * 200 + 3, 0xdead) == 0xdead);
    // limits: a too big write is not kept
    static uint8_t huge[UWL_WMAX + 1];
    CHK(!uwl_add(&L, 1, 0x1000, sizeof huge, huge) && L.over == 1);
    CHK(!uwl_add(&L, 1, 0x1000, 8, NULL));
    uwl_view_free(&v); free(L.e); free(L.b);
    if (!bad) printf("test_uwlog: ok\n");
    return bad;
}
