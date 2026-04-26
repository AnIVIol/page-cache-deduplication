/* test_mixed_rwc.c - readers + writers + cp threads
 * gcc -Wall -Wextra -O2 -pthread -o test_mixed_rwc test_mixed_rwc.c
 *
 * Copy threads pick (src, dst) randomly and replicate src -> dst.
 * This creates re-deduplication opportunities (dst may match other files
 * after copy) and forces unmerge of dst's previous pages.
 */
#include "dedup_ops_common.h"

#define DIR         "/tmp/dedup_mrwc"
#define NFILES      8
#define NREADERS    8
#define NWRITERS    4
#define NCOPY       2
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
    uint8_t sig = (uint8_t)(0x70 + tid);
    pthread_barrier_wait(&g_bar);
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = rand_r(&seed) % MAX_FSIZE;
        size_t len = 1 + rand_r(&seed) % 4096;
        op_write(&g_files[f], off, len, sig);
    }
    return NULL;
}

static void *copier(void *arg) {
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 9) ^ tid * 0xD4u);
    pthread_barrier_wait(&g_bar);
    int n = OPS / 30;
    for (int i = 0; i < n; i++) {
        int s = rand_r(&seed) % NFILES;
        int d;
        do { d = rand_r(&seed) % NFILES; } while (d == s);
        op_cp(&g_files[s], &g_files[d]);
        usleep(3000 + rand_r(&seed) % 12000);
    }
    return NULL;
}

int main(void)
{
    const char *T = "mixed-rwc";
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

    pthread_barrier_init(&g_bar, NULL, NREADERS + NWRITERS + NCOPY);
    pthread_t r[NREADERS], w[NWRITERS], c[NCOPY];
    for (int i = 0; i < NREADERS; i++) pthread_create(&r[i], NULL, reader, (void *)(intptr_t)i);
    for (int i = 0; i < NWRITERS; i++) pthread_create(&w[i], NULL, writer, (void *)(intptr_t)i);
    for (int i = 0; i < NCOPY;    i++) pthread_create(&c[i], NULL, copier, (void *)(intptr_t)i);

    long bad = 0;
    for (int i = 0; i < NREADERS; i++) { void *rv; pthread_join(r[i], &rv); bad += (long)(intptr_t)rv; }
    for (int i = 0; i < NWRITERS; i++) pthread_join(w[i], NULL);
    for (int i = 0; i < NCOPY;    i++) pthread_join(c[i], NULL);
    pthread_barrier_destroy(&g_bar);

    int f1 = verify_all(g_files, NFILES, 0);
    int f2 = verify_all(g_files, NFILES, 1);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) fs_destroy(&g_files[i]);
    rm_rf_simple(DIR);

    if (bad)      FAIL(T, "%ld bad reads under cp+writes", bad);
    if (f1 || f2) FAIL(T, "verify failed (cached=%d ondisk=%d)", f1, f2);
    PASS(T);
}
