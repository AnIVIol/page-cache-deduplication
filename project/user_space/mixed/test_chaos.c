/* test_chaos_all.c - readers + writers + truncators + cp threads, all together
 * gcc -Wall -Wextra -O2 -pthread -o test_chaos_all test_chaos_all.c
 *
 * Maximum-contention test: every kind of operation runs at once over a
 * shared pool of deduped files. This is the catch-all integration test.
 */
#include "dedup_ops_common.h"

#define DIR         "/tmp/dedup_chaos"
#define NFILES      10
#define NREADERS    10
#define NWRITERS    6
#define NTRUNC      2
#define NCOPY       2
#define OPS         600

static file_state_t      g_files[NFILES];
static pthread_barrier_t g_bar;

static void *reader(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)(time(NULL) ^ tid * 0x11u);
    pthread_barrier_wait(&g_bar);
    long bad = 0;
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = rand_r(&seed) % MAX_FSIZE;
        size_t len = 1 + rand_r(&seed) % 8192;
        /* Mix in some boundary-spanning reads */
        if ((i & 7) == 0) {
            int page = 1 + rand_r(&seed) % ((MAX_FSIZE / 4096) - 1);
            off = page * 4096 - 8 + (rand_r(&seed) % 16);
            len = 8 + rand_r(&seed) % 24;
            if (off < 0) off = 0;
        }
        if (op_read(&g_files[f], off, len) == -2) bad++;
    }
    return (void *)(intptr_t)bad;
}

static void *writer(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 2) ^ tid * 0x22u);
    uint8_t sig = (uint8_t)(0xA0 + tid);
    pthread_barrier_wait(&g_bar);
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off; size_t len;
        if ((i & 7) == 0) {
            /* Boundary-spanning write */
            int page = 1 + rand_r(&seed) % ((MAX_FSIZE / 4096) - 1);
            off = page * 4096 - 8 + (rand_r(&seed) % 16);
            len = 8 + rand_r(&seed) % 24;
            if (off < 0) off = 0;
        } else {
            off = rand_r(&seed) % MAX_FSIZE;
            len = 1 + rand_r(&seed) % 4096;
        }
        op_write(&g_files[f], off, len, sig);
    }
    return NULL;
}

static void *truncator(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 5) ^ tid * 0x33u);
    pthread_barrier_wait(&g_bar);
    int n = OPS / 25;
    for (int i = 0; i < n; i++) {
        int f = rand_r(&seed) % NFILES;
        size_t newsz;
        switch (rand_r(&seed) % 4) {
        case 0: newsz = 0; break;
        case 1: newsz = MAX_FSIZE; break;
        case 2: newsz = (rand_r(&seed) % (MAX_FSIZE / 4096 + 1)) * 4096; break;
        default: newsz = rand_r(&seed) % MAX_FSIZE; break;  /* unaligned */
        }
        op_truncate(&g_files[f], newsz);
        usleep(3000 + rand_r(&seed) % 10000);
    }
    return NULL;
}

static void *copier(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 8) ^ tid * 0x44u);
    pthread_barrier_wait(&g_bar);
    int n = OPS / 30;
    for (int i = 0; i < n; i++) {
        int s = rand_r(&seed) % NFILES;
        int d; do { d = rand_r(&seed) % NFILES; } while (d == s);
        op_cp(&g_files[s], &g_files[d]);
        usleep(5000 + rand_r(&seed) % 15000);
    }
    return NULL;
}

int main(void)
{
    const char *T = "chaos-all";
    LOGI("=== %s ===\n", T);
    rm_rf_simple(DIR); mkdir_p(DIR);

    for (int i = 0; i < NFILES; i++) {
        char p[256]; snprintf(p, sizeof(p), "%s/f%d.dat", DIR, i);
        if (fs_init(&g_files[i], p) < 0) DIE("fs_init");
    }
    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(g_files[i].fd, g_files[i].size);

    scanner_interval("80");
    register_all(g_files, NFILES);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);

    int total = NREADERS + NWRITERS + NTRUNC + NCOPY;
    pthread_barrier_init(&g_bar, NULL, total);
    pthread_t r[NREADERS], w[NWRITERS], t[NTRUNC], c[NCOPY];

    for (int i = 0; i < NREADERS; i++) pthread_create(&r[i], NULL, reader,    (void *)(intptr_t)i);
    for (int i = 0; i < NWRITERS; i++) pthread_create(&w[i], NULL, writer,    (void *)(intptr_t)i);
    for (int i = 0; i < NTRUNC;   i++) pthread_create(&t[i], NULL, truncator, (void *)(intptr_t)i);
    for (int i = 0; i < NCOPY;    i++) pthread_create(&c[i], NULL, copier,    (void *)(intptr_t)i);

    long bad = 0;
    for (int i = 0; i < NREADERS; i++) { void *rv; pthread_join(r[i], &rv); bad += (long)(intptr_t)rv; }
    for (int i = 0; i < NWRITERS; i++) pthread_join(w[i], NULL);
    for (int i = 0; i < NTRUNC;   i++) pthread_join(t[i], NULL);
    for (int i = 0; i < NCOPY;    i++) pthread_join(c[i], NULL);
    pthread_barrier_destroy(&g_bar);

    LOGI("Chaos phase done. Verifying...\n");
    int f1 = verify_all(g_files, NFILES, 0);
    LOGI("Re-verifying after drop_caches (forces rebuild from disk)...\n");
    int f2 = verify_all(g_files, NFILES, 1);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) fs_destroy(&g_files[i]);
    rm_rf_simple(DIR);

    if (bad)      FAIL(T, "%ld reads observed corruption during chaos", bad);
    if (f1 || f2) FAIL(T, "verify failed (cached=%d ondisk=%d)", f1, f2);
    PASS(T);
}

