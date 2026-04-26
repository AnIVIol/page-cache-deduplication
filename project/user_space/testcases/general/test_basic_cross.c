/* test_basic_cross.c - verify dedup of identical pages across multiple files
 * gcc -Wall -Wextra -O2 -o test_basic_cross test_basic_cross.c
 */
#include "dedup_test_common.h"

#define DIR     "/tmp/dedup_t12"
#define NFILES  10
#define FSIZE   (256 * 1024)   /* 256 KB each */

int main(void)
{
    const char *T = "1.2-cross-file";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fds[NFILES];
    char paths[NFILES][256];

    for (int i = 0; i < NFILES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/file_%02d.dat", DIR, i);
        fds[i] = create_pattern_file(paths[i], FSIZE, 0xCAFE);
        if (fds[i] < 0) DIE("create %s", paths[i]);
    }

    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(fds[i], FSIZE);

    long mem_before = get_meminfo_kb("Cached:");
    LOGI("Cached before dedup: %ld kB\n", mem_before);

    scanner_interval("100");
    for (int i = 0; i < NFILES; i++)
        if (scanner_register(paths[i]) < 0) FAIL(T, "register failed");
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(4);

    long mem_after = get_meminfo_kb("Cached:");
    long saved_kb  = mem_before - mem_after;
    long expected  = (long)(NFILES - 1) * (FSIZE / 1024);
    LOGI("Cached after dedup: %ld kB (saved %ld kB, ideal %ld kB)\n",
         mem_after, saved_kb, expected);
    int saving_ok = (saved_kb >= expected / 3);

    /* Verify content of every file */
    int content_ok = 1;
    for (int i = 0; i < NFILES; i++) {
        if (verify_pattern(fds[i], FSIZE, 0xCAFE) != 0) {
            LOGE("file %s corrupted\n", paths[i]);
            content_ok = 0;
        }
    }

    scanner_stop();
    for (int i = 0; i < NFILES; i++) close(fds[i]);
    rm_rf_simple(DIR);

    if (!content_ok) FAIL(T, "content corrupted");
    if (!saving_ok)  FAIL(T, "savings %ld < expected %ld kB", saved_kb, expected/3);
    PASS(T);
}
