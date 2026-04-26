/* test_ordered_phases.c - run operations in specific phase orderings,
 * verifying after each phase. Catches bugs that only surface in
 * particular orderings (e.g., merge-after-cp, truncate-after-merge).
 *
 * gcc -Wall -Wextra -O2 -pthread -o test_ordered_phases test_ordered_phases.c
 */
#include "dedup_ops_common.h"

#define DIR        "/tmp/dedup_phases"
#define NFILES     8
#define MAX_THR    16

static file_state_t      g_files[NFILES];
static pthread_barrier_t g_bar;
static volatile int      g_stop         = 0;
static volatile int      g_allowed_ops  = 0;   /* volatile: workers re-read each loop */

enum { OP_R = 1, OP_W = 2, OP_T = 4, OP_C = 8 };

static void *worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)(time(NULL) ^ tid * 0xDEADBEEFu);
    uint8_t sig = (uint8_t)(0x40 + tid);
    long bad = 0;

    pthread_barrier_wait(&g_bar);     /* one of NTHR+1 rendezvous participants */

    while (!g_stop) {
        int allowed = g_allowed_ops;
        if (allowed == 0) { usleep(1000); continue; }

        int ops[4]; int nops = 0;
        if (allowed & OP_R) ops[nops++] = OP_R;
        if (allowed & OP_W) ops[nops++] = OP_W;
        if (allowed & OP_T) ops[nops++] = OP_T;
        if (allowed & OP_C) ops[nops++] = OP_C;
        int op = ops[rand_r(&seed) % nops];

        switch (op) {
        case OP_R: {
            int f = rand_r(&seed) % NFILES;
            off_t off = rand_r(&seed) % MAX_FSIZE;
            size_t len = 1 + rand_r(&seed) % 4096;
            if (op_read(&g_files[f], off, len) == -2) bad++;
            break;
        }
        case OP_W: {
            int f = rand_r(&seed) % NFILES;
            off_t off = rand_r(&seed) % MAX_FSIZE;
            size_t len = 1 + rand_r(&seed) % 4096;
            op_write(&g_files[f], off, len, sig);
            break;
        }
        case OP_T: {
            int f = rand_r(&seed) % NFILES;
            size_t newsz = (rand_r(&seed) % (MAX_FSIZE / 4096 + 1)) * 4096;
            op_truncate(&g_files[f], newsz);
            usleep(2000);
            break;
        }
        case OP_C: {
            int s = rand_r(&seed) % NFILES;
            int d; do { d = rand_r(&seed) % NFILES; } while (d == s);
            op_cp(&g_files[s], &g_files[d]);
            usleep(3000);
            break;
        }
        }
    }
    return (void *)(intptr_t)bad;
}

struct phase { const char *name; int ops; int seconds; };
static const struct phase phases[] = {
    { "P1 writes-only",       OP_W,                       2 },
    { "P2 cp-only",           OP_C,                       2 },
    { "P3 reads+writes",      OP_R | OP_W,                3 },
    { "P4 writes+cp",         OP_W | OP_C,                3 },
    { "P5 writes+trunc",      OP_W | OP_T,                3 },
    { "P6 cp+trunc",          OP_C | OP_T,                3 },
    { "P7 all-together",      OP_R | OP_W | OP_T | OP_C,  5 },
};
#define NPHASES (sizeof(phases)/sizeof(phases[0]))

int main(void)
{
    const char *T = "ordered-phases";
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

    int NTHR = MAX_THR;
    /* FIX: barrier participants = workers + main (which also calls wait) */
    pthread_barrier_init(&g_bar, NULL, NTHR + 1);
    pthread_t tids[MAX_THR];
    long bad_total = 0;

    for (int i = 0; i < NTHR; i++)
        pthread_create(&tids[i], NULL, worker, (void *)(intptr_t)i);

    g_allowed_ops = 0;
    pthread_barrier_wait(&g_bar);   /* now NTHR + 1 participants matches init */

    int phase_failed = 0;
    for (size_t pi = 0; pi < NPHASES; pi++) {
        LOGI(">>> %s : ops=0x%x duration=%ds\n",
             phases[pi].name, phases[pi].ops, phases[pi].seconds);
        g_allowed_ops = phases[pi].ops;
        sleep(phases[pi].seconds);
        g_allowed_ops = 0;
        usleep(50 * 1000);          /* let in-flight ops settle */

        int f = verify_all(g_files, NFILES, 0);
        if (f) { LOGE("Phase %s : %d files corrupted\n", phases[pi].name, f); phase_failed++; }
        else   { LOGI("Phase %s : verify OK\n", phases[pi].name); }
        sleep(1);
    }

    /* Tell workers to exit. They wake from usleep(1000) within 1ms,
     * see g_stop, and return. */
    g_stop = 1;

    LOGI("Joining %d workers...\n", NTHR);
    for (int i = 0; i < NTHR; i++) {
        void *rv;
        pthread_join(tids[i], &rv);
        bad_total += (long)(intptr_t)rv;
    }
    LOGI("All workers joined.\n");
    pthread_barrier_destroy(&g_bar);

    LOGI("Final cross-check after drop_caches...\n");
    int final_fail = verify_all(g_files, NFILES, 1);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) fs_destroy(&g_files[i]);
    rm_rf_simple(DIR);

    if (bad_total)    FAIL(T, "%ld reads observed corruption", bad_total);
    if (phase_failed) FAIL(T, "%d phases failed verification", phase_failed);
    if (final_fail)   FAIL(T, "%d files differ from shadow on final disk verify", final_fail);
    PASS(T);
}
