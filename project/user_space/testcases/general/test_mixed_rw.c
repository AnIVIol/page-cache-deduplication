/* test_mixed_rw.c - concurrent readers + writers on deduped files
 * gcc -Wall -Wextra -O2 -pthread -o test_mixed_rw test_mixed_rw.c
 *
 * Goal: prove that under heavy concurrent read/write, no reader observes
 *       partial/corrupt data, and final shadow matches actual file content.
 */
#include "dedup_ops_common.h"

#define DIR        "/tmp/dedup_mrw"
#define NFILES     8
#define NREADERS   12
#define NWRITERS   6
#define OPS        1500

static file_state_t      g_files[NFILES];
static pthread_barrier_t g_bar;

static void *reader(void *arg)
{
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)(time(NULL) ^ tid * 0x9E3779B1u);
    pthread_barrier_wait(&g_bar);
    long bad = 0;
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = rand_r(&seed) % MAX_FSIZE;
        size_t len = 1 + rand_r(&seed) % 8192;
        int rc = op_read(&g_files[f], off, len);
        if (rc == -2) bad++;
    }
    return (void *)(intptr_t)bad;
}

static void *writer(void *arg)
{
    int tid = (int)(intptr_t)arg;
    unsigned int seed = (unsigned)((time(NULL) << 4) ^ tid * 0x9E3779B1u);
    uint8_t sig = (uint8_t)(0x80 + tid);
    pthread_barrier_wait(&g_bar);
    for (int i = 0; i < OPS; i++) {
        int f = rand_r(&seed) % NFILES;
        off_t off = rand_r(&seed) % MAX_FSIZE;
        size_t len = 1 + rand_r(&seed) % 4096;
        op_write(&g_files[f], off, len, sig);
    }
    return NULL;
}

int main(void)
{
    const char *T = "mixed-rw";
    LOGI("=== %s ===\n", T);
    rm_rf_simple(DIR); mkdir_p(DIR);

    for (int i = 0; i < NFILES; i++) {
        char p[256]; snprintf(p, sizeof(p), "%s/f%d.dat", DIR, i);
        if (fs_init(&g_files[i], p) < 0) DIE("fs_init");
    }
    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(g_files[i].fd, g_files[i].size);

    long m0 = get_meminfo_kb("Cached:");
    scanner_interval("100");
    if (register_all(g_files, NFILES) < 0) FAIL(T, "register");
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);
    LOGI("Dedup savings: %ld kB\n", m0 - get_meminfo_kb("Cached:"));

    pthread_barrier_init(&g_bar, NULL, NREADERS + NWRITERS);
    pthread_t r[NREADERS], w[NWRITERS];
    for (int i = 0; i < NREADERS; i++) pthread_create(&r[i], NULL, reader, (void *)(intptr_t)i);
    for (int i = 0; i < NWRITERS; i++) pthread_create(&w[i], NULL, writer, (void *)(intptr_t)i);

    long bad_reads = 0;
    for (int i = 0; i < NREADERS; i++) { void *rv; pthread_join(r[i], &rv); bad_reads += (long)(intptr_t)rv; }
    for (int i = 0; i < NWRITERS; i++) pthread_join(w[i], NULL);
    pthread_barrier_destroy(&g_bar);

    LOGI("Verifying via existing fds...\n");
    int f1 = verify_all(g_files, NFILES, 0);
    LOGI("Verifying after drop_caches (forces re-read from disk)...\n");
    int f2 = verify_all(g_files, NFILES, 1);

    scanner_stop();
    for (int i = 0; i < NFILES; i++) fs_destroy(&g_files[i]);
    rm_rf_simple(DIR);

    if (bad_reads) FAIL(T, "%ld concurrent reads observed corruption", bad_reads);
    if (f1 || f2)  FAIL(T, "post-test verify failed (cached=%d, ondisk=%d)", f1, f2);
    PASS(T);
}
