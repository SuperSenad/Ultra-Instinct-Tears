// [UIT 0362] uit_logline (src/runtime/logline.h): a line is written whole. 8 threads log at once into a buffered file
// (as Play with log now is) and into an unbuffered one; every line read back must be one complete line (its own
// prefix, its own text, its own end), none empty, none joined. Long lines (over the 1 KiB stack buffer) come out whole.
// -DSPLIT uses the old two-call form (fprintf, then fputc('\n')) and must fail (torn or empty lines), on a machine
// with more than one CPU.
//   cc -O2 -pthread tools/tests/test_logline.c -o /tmp/t/logline && /tmp/t/logline
#include "../../src/runtime/logline.h"
#include <pthread.h>
#include <string.h>

#ifdef SPLIT
#define LOG(...) (fprintf(stderr, "[t] " __VA_ARGS__), fputc('\n', stderr))
#else
#define LOG(...) uit_logline("[t] " __VA_ARGS__)
#endif
enum { THREADS = 8, LINES = 20000 };
static char longtext[3000];

static void *writer(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < LINES; i++) {
        if (i % 1000 == 7) LOG("thread %d line %d long %s end", id, i, longtext);
        else LOG("thread %d line %d end", id, i);
    }
    return NULL;
}

static int run(const char *path, int buffered) {
    if (!freopen(path, "wb", stderr)) return 1;
    if (buffered) setvbuf(stderr, NULL, _IOFBF, 1 << 20);
    else setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, writer, (void *)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    fflush(stderr);
    FILE *f = fopen(path, "rb");
    static char line[8192];
    int count[THREADS] = {0}, bad = 0, n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        int id, i, used = 0;
        size_t len = strlen(line);
        if (len < 2 || line[len - 1] != '\n') { bad++; continue; }
        line[len - 1] = 0;
        if (sscanf(line, "[t] thread %d line %d %n", &id, &i, &used) != 2 || id < 0 || id >= THREADS) { bad++; continue; }
        const char *rest = line + used;
        int ok = !strcmp(rest, "end") ||
                 (!strncmp(rest, "long ", 5) && !strncmp(rest + 5, longtext, sizeof longtext - 1) && !strcmp(rest + 5 + sizeof longtext - 1, " end"));
        if (!ok || strstr(rest, "[t]")) { bad++; continue; }
        count[id]++;
    }
    fclose(f);
    int missing = 0;
    for (int i = 0; i < THREADS; i++) missing += count[i] != LINES;
    printf("%s (%s): %d lines, %d torn or empty, %d threads with lines missing\n", path, buffered ? "buffered" : "unbuffered",
           n, bad, missing);
    return bad || missing || n != THREADS * LINES;
}

int main(void) {
    memset(longtext, 'x', sizeof longtext - 1);
    int fail = run("/tmp/t/logline-buffered.txt", 1) | run("/tmp/t/logline-unbuffered.txt", 0);
    printf("%s\n", fail ? "FAIL" : "all ok");
    return fail;
}
