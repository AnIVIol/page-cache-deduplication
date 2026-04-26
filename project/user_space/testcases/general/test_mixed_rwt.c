/* test_mixed_rwt.c - readers + writers + truncators on deduped files
 * gcc -Wall -Wextra -O2 -pthread -o test_mixed_rwt test_mixed_rwt.c
 *
 * Truncate forces unmerge of trailing pages and may create CoW for the
 * partial-page boundary. This test verifies neither the truncating
 * file nor sibling files get corrupted.
 */
#include "dedup_ops_common.h"

#define DIR         "/tmp/dedup_mrwt"
#define NFILES      8
#define NREADERS    8
#define NWRITERS    4
#define NTRUNC      2
#define OPS         800

static file_state_t      g_files[NFILES];
static pthread_barrier_t g_bar;

static void *reader(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)(time(NULL) ^ tid * 0xA1u);
    pthread_barrier_wait(&g_bar);
    long bad = 0;
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = rand_r(&seed) % MAX_FSIZE;
        size_t len = 1 + rand_r(&seed) % 8192;
        if (op_read(&g_files[f], off, len) == -2) bad++;
    }
    return (void *)(intptr_t)bad;
}

static void *writer(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 3) ^ tid * 0xB2u);
    uint8_t sig = (uint8_t)(0x60 + tid);
    pthread_barrier_wait(&g_bar);
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = rand_r(&seed) % MAX_FSIZE;
        size_t len = 1 + rand_r(&seed) % 4096;
        op_write(&g_files[f], off, len, sig);
    }
    return NULL;
}

static void *truncator(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 7) ^ tid * 0xC3u);
    pthread_barrier_wait(&g_bar);
    /* Fewer truncates than reads/writes; rate limited */
    int n = OPS / 20;
    for (int i = 0; i < n; i++) {
        int f = rand_r(&seed) % NFILES;
        /* Truncate to a page-aligned size in [0, MAX_FSIZE] */
        size_t pages = MAX_FSIZE / 4096;
        size_t newsz = (rand_r(&seed) % (pages + 1)) * 4096;
        op_truncate(&g_files[f], newsz);
        usleep(2000 + rand_r(&seed) % 8000);
    }
    return NULL;
}

int main(void)
{
    const char *T = "mixed-rwt";
    LOGI("=== %s ===\n", T);
    rm_rf_simple(DIR); mkdir_p(DIR);

    for (int i = 0; i < NFILES; i++) {
        char p[256]; snprintf(p, sizeof(p), "%s/f%d.dat", DIR, i);
        if (fs_init(&g_files[i], p) < 0) DIE("fs_init");
    }
    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(g_files[i].fd, g_files[i].size);

    scanner_interval("100");
    register_all(g_files, NFILES);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);

    pthread_barrier_init(&g_bar, NULL, NREADERS + NWRITERS + NTRUNC);
    pthread_t r[NREADERS], w[NWRITERS], t[NTRUNC];
    for (int i = 0; i < NREADERS; i++) pthread_create(&r[i], NULL, reader,    (void *)(intptr_t)i);
    for (int i = 0; i < NWRITERS; i++) pthread_create(&w[i], NULL, writer,    (void *)(intptr_t)i);
    for (int i = 0; i < NTRUNC;   i++) pthread_create(&t[i], NULL, truncator, (void *)(intptr_t)i);

    long bad = 0;
    for (int i = 0; i < NREADERS; i++) { void *rv; pthread_join(r[i], &rv); bad += (long)(intptr_t)rv; }
    for (int i = 0; i < NWRITERS; i++) pthread_join(w[i], NULL);
    for (int i = 0; i < NTRUNC;   i++) pthread_join(t[i], NULL);
    pthread_barrier_destroy(&g_bar);

    int f1 = verify_all(g_files, NFILES, 0);
    int f2 = verify_all(g_files, NFILES, 1);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) fs_destroy(&g_files[i]);
    rm_rf_simple(DIR);

    if (bad)      FAIL(T, "%ld readers saw corruption during truncate", bad);
    if (f1 || f2) FAIL(T, "post-test verify failed (cached=%d ondisk=%d)", f1, f2);
    PASS(T);
}
