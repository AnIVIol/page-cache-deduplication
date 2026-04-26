/* test_concurrent_read.c - many threads read shared deduped pages
 * gcc -Wall -Wextra -O2 -pthread -o test_concurrent_read test_concurrent_read.c
 */
#include "dedup_test_common.h"
#include <pthread.h>

#define DIR     "/tmp/dedup_t21"
#define NFILES  4
#define FSIZE   (256 * 1024)
#define NTHR    16
#define ITERS   2000

static int g_fds[NFILES];
static volatile int g_fail = 0;
static pthread_barrier_t g_bar;

static void *reader(void *arg)
{
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ tid * 0x9E3779B1u);
    uint8_t buf[4096], exp[4096];

    pthread_barrier_wait(&g_bar);

    for (int i = 0; i < ITERS && !g_fail; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = (rand_r(&seed) % (FSIZE / 4096)) * 4096;
        if (pread(g_fds[f], buf, 4096, off) != 4096) {
            LOGE("pread tid=%d failed: %s\n", tid, strerror(errno));
            g_fail = 1; break;
        }
        for (int j = 0; j < 4096; j++)
            exp[j] = (uint8_t)(((off + j) * 7 + 0x3333 * 13) & 0xff);
        if (memcmp(buf, exp, 4096) != 0) {
            LOGE("tid=%d data mismatch file=%d off=%ld\n", tid, f, (long)off);
            g_fail = 1; break;
        }
    }
    return NULL;
}

int main(void)
{
    const char *T = "2.1-concurrent-read";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    char paths[NFILES][256];
    for (int i = 0; i < NFILES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/f%d.dat", DIR, i);
        g_fds[i] = create_pattern_file(paths[i], FSIZE, 0x3333);
        if (g_fds[i] < 0) DIE("create");
    }
    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(g_fds[i], FSIZE);

    scanner_interval("100");
    for (int i = 0; i < NFILES; i++) scanner_register(paths[i]);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);

    pthread_barrier_init(&g_bar, NULL, NTHR);
    pthread_t tids[NTHR];
    for (int i = 0; i < NTHR; i++)
        pthread_create(&tids[i], NULL, reader, (void *)(intptr_t)i);
    for (int i = 0; i < NTHR; i++)
        pthread_join(tids[i], NULL);
    pthread_barrier_destroy(&g_bar);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) close(g_fds[i]);
    rm_rf_simple(DIR);

    if (g_fail) FAIL(T, "concurrent reader observed corruption");
    PASS(T);
}
