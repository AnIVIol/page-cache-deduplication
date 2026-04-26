/* test_refcount_stress.c - many files share a single survivor; delete one by one
 * gcc -Wall -Wextra -O2 -o test_refcount_stress test_refcount_stress.c
 */
#include "dedup_test_common.h"

#define DIR     "/tmp/dedup_t65"
#define NFILES  50
#define FSIZE   (64 * 1024)

int main(void)
{
    const char *T = "6.5-refcount-stress";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fds[NFILES];
    char paths[NFILES][256];
    for (int i = 0; i < NFILES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/f_%03d.dat", DIR, i);
        fds[i] = create_pattern_file(paths[i], FSIZE, 0x55AA);
        if (fds[i] < 0) DIE("create");
    }
    drop_caches();
    for (int i = 0; i < NFILES; i++) prewarm_file(fds[i], FSIZE);

    scanner_interval("100");
    for (int i = 0; i < NFILES; i++) scanner_register(paths[i]);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(4);

    /* Unlink half (with fds still open) and verify others still readable */
    int unlink_half_ok = 1;
    for (int i = 0; i < NFILES / 2; i++) {
        if (unlink(paths[i]) < 0) DIE("unlink %s", paths[i]);
    }
    /* Remaining files must still be intact */
    for (int i = NFILES / 2; i < NFILES; i++) {
        if (verify_pattern(fds[i], FSIZE, 0x55AA) != 0) {
            LOGE("after unlink: file %d corrupted\n", i); unlink_half_ok = 0;
        }
    }

    /* Read through the still-open (but unlinked) fds: should still see data */
    int unlinked_fd_read_ok = 1;
    for (int i = 0; i < NFILES / 2; i++) {
        if (verify_pattern(fds[i], FSIZE, 0x55AA) != 0) {
            LOGE("unlinked fd %d corrupted\n", i); unlinked_fd_read_ok = 0;
        }
    }

    /* Close everything (forces last refcount drop on unlinked files) */
    for (int i = 0; i < NFILES; i++) close(fds[i]);

    /* The remaining (still-linked) files should still verify on a fresh open */
    int reopen_ok = 1;
    for (int i = NFILES / 2; i < NFILES; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) { reopen_ok = 0; LOGE("reopen %d failed\n", i); continue; }
        if (verify_pattern(fd, FSIZE, 0x55AA) != 0) {
            LOGE("reopen file %d corrupted\n", i); reopen_ok = 0;
        }
        close(fd);
    }

    scanner_stop();
    rm_rf_simple(DIR);

    if (!unlink_half_ok)        FAIL(T, "remaining files corrupt after unlink");
    if (!unlinked_fd_read_ok)   FAIL(T, "open-but-unlinked fd corrupt");
    if (!reopen_ok)             FAIL(T, "reopened survivors corrupt");
    PASS(T);
}
