/* test_thousand_files.c - massive cross-file dedup
 * gcc -Wall -Wextra -O2 -o test_thousand_files test_thousand_files.c
 */
#include "dedup_test_common.h"

#define DIR     "/tmp/dedup_t81"
#define NFILES  1000
#define FSIZE   (16 * 1024)   /* 16 KB each => 16 MB raw, ~16 KB after dedup */

int main(void)
{
    const char *T = "8.1-thousand-files";
    LOGI("=== %s ===\n", T);
    rm_rf_simple(DIR);
    mkdir_p(DIR);

    char (*paths)[64] = calloc(NFILES, sizeof(*paths));
    int  *fds         = calloc(NFILES, sizeof(int));
    if (!paths || !fds) DIE("calloc");

    LOGI("Creating %d files...\n", NFILES);
    for (int i = 0; i < NFILES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/f_%04d.dat", DIR, i);
        fds[i] = create_pattern_file(paths[i], FSIZE, 0xBEEF);
        if (fds[i] < 0) DIE("create %d", i);
    }

    drop_caches();
    LOGI("Prewarming page cache...\n");
    for (int i = 0; i < NFILES; i++) prewarm_file(fds[i], FSIZE);

    long mem_before = get_meminfo_kb("Cached:");
    LOGI("Cached before dedup: %ld kB\n", mem_before);

    scanner_interval("50");
    LOGI("Registering %d files (this may take a moment)...\n", NFILES);
    for (int i = 0; i < NFILES; i++) {
        if (scanner_register(paths[i]) < 0) FAIL(T, "register failed at %d", i);
    }
    if (scanner_start() < 0) FAIL(T, "scanner start");

    LOGI("Waiting 30s for dedup sweep...\n");
    sleep(30);

    long mem_after = get_meminfo_kb("Cached:");
    long saved_kb  = mem_before - mem_after;
    long ideal_kb  = (long)(NFILES - 1) * (FSIZE / 1024);
    LOGI("Cached after dedup: %ld kB (saved %ld kB, ideal %ld kB)\n",
         mem_after, saved_kb, ideal_kb);
    int saving_ok = (saved_kb >= ideal_kb / 3);

    /* Sample 50 random files for content correctness */
    LOGI("Sampling 50 random files for content correctness...\n");
    int content_ok = 1;
    srand(time(NULL));
    for (int i = 0; i < 50; i++) {
        int idx = rand() % NFILES;
        if (verify_pattern(fds[idx], FSIZE, 0xBEEF) != 0) {
            LOGE("file %d corrupted\n", idx);
            content_ok = 0;
        }
    }

    scanner_stop();
    for (int i = 0; i < NFILES; i++) close(fds[i]);
    free(paths); free(fds);
    rm_rf_simple(DIR);

    if (!content_ok) FAIL(T, "content corruption");
    if (!saving_ok)  FAIL(T, "savings %ld kB < threshold %ld kB", saved_kb, ideal_kb/3);
    PASS(T);
}
