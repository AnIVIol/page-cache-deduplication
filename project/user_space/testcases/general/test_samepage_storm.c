/* test_samepage_storm.c - many threads slam the SAME shared page across
 * different files with reads/writes/truncates/cp simultaneously.
 *
 * This is the hardest case for the dedup core because every operation
 * touches the same survivor page.
 *
 * gcc -Wall -Wextra -O2 -pthread -o test_samepage_storm test_samepage_storm.c
 */
#include "dedup_ops_common.h"

#define DIR       "/tmp/dedup_storm"
#define NFILES    8
#define NTHR      20
#define OPS       2000
#define HOTPAGE   3                /* page index every thread targets */

static file_state_t      g_files[NFILES];
static pthread_barrier_t g_bar;

static void *storm(void *arg)
{
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)(time(NULL) ^ tid * 0xC0FFEEu);
    uint8_t sig = (uint8_t)(0x10 + tid);
    long bad = 0;
    pthread_barrier_wait(&g_bar);

    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t base = (off_t)HOTPAGE * 4096;

        switch (rand_r(&seed) % 6) {
        case 0: case 1: {  /* read inside hot page */
            off_t off = base + rand_r(&seed) % 4096;
            size_t len = 1 + rand_r(&seed) % (4096 - (off - base));
            if (op_read(&g_files[f], off, len) == -2) bad++;
            break;
        }
        case 2: case 3: {  /* write inside hot page */
            off_t off = base + rand_r(&seed) % 4096;
            size_t len = 1 + rand_r(&seed) % (4096 - (off - base));
            op_write(&g_files[f], off, len, sig);
            break;
        }
        case 4: {          /* truncate to just below hot page (kills it) */
            op_truncate(&g_files[f], base);
            usleep(500);
            op_truncate(&g_files[f], MAX_FSIZE);  /* extend back, zero-fills */
            break;
        }
        case 5: {          /* cp another file over this one */
            int s; do { s = rand_r(&seed) % NFILES; } while (s == f);
            op_cp(&g_files[s], &g_files[f]);
            break;
        }
        }
    }
    return (void *)(intptr_t)bad;
}

int main(void)
{
    const char *T = "samepage-storm";
    LOGI("=== %s ===\n", T);
    rm_rf_simple(DIR); mkdir_p(DIR);

    for (int i = 0; i < NFILES; i++) {
        char p[256]; snprintf(p, sizeof(p), "%s/f%d.dat", DIR, i);
        if (fs_init(&g_files[i], p) < 0) DIE("fs_init");
    }
    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(g_files[i].fd, g_files[i].size);

    scanner_interval("60");
    register_all(g_files, NFILES);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);

    pthread_barrier_init(&g_bar, NULL, NTHR);
    pthread_t tids[NTHR];
    for (int i = 0; i < NTHR; i++)
        pthread_create(&tids[i], NULL, storm, (void *)(intptr_t)i);

    long bad = 0;
    for (int i = 0; i < NTHR; i++) {
        void *rv; pthread_join(tids[i], &rv); bad += (long)(intptr_t)rv;
    }
    pthread_barrier_destroy(&g_bar);

    int f1 = verify_all(g_files, NFILES, 0);
    int f2 = verify_all(g_files, NFILES, 1);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) fs_destroy(&g_files[i]);
    rm_rf_simple(DIR);

    if (bad)      FAIL(T, "%ld bad reads on hot page", bad);
    if (f1 || f2) FAIL(T, "verify failed (cached=%d ondisk=%d)", f1, f2);
    PASS(T);
}
