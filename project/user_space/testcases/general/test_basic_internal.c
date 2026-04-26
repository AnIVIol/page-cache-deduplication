/* test_basic_internal.c - verify dedup of duplicate pages within ONE file
 * gcc -Wall -Wextra -O2 -o test_basic_internal test_basic_internal.c
 */
#include "dedup_test_common.h"

#define DIR     "/tmp/dedup_t11"
#define PATH    DIR "/single.dat"
#define NPAGES  256
#define PSIZE   4096
#define FSIZE   (NPAGES * PSIZE)

int main(void)
{
    const char *T = "1.1-single-file";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) DIE("open");

    /* Page i: even pages = pattern A, odd pages = pattern B.
     * Only 2 unique pages, expect ~62 pages saved. */
    uint8_t *page = malloc(PSIZE);
    if (!page) DIE("malloc");
    for (int i = 0; i < NPAGES; i++) {
        memset(page, (i & 1) ? 0xBB : 0xAA, PSIZE);
        if (pwrite(fd, page, PSIZE, (off_t)i * PSIZE) != PSIZE) DIE("pwrite");
    }
    fsync(fd);
    free(page);

    drop_caches();
    if (prewarm_file(fd, FSIZE) < 0) DIE("prewarm");

    long mem_before = get_meminfo_kb("Cached:");
    LOGI("Cached before dedup: %ld kB\n", mem_before);

    scanner_interval("100");
    if (scanner_register(PATH) < 0) FAIL(T, "register failed - module loaded?");
    if (scanner_start() < 0) FAIL(T, "scanner start failed");
    sleep(3);

    long mem_after = get_meminfo_kb("Cached:");
    long saved_kb = mem_before - mem_after;
    LOGI("Cached after dedup: %ld kB (saved %ld kB)\n", mem_after, saved_kb);

    /* Expected savings ~ (NPAGES - 2) * 4 KB = 248 KB. Allow loose tolerance. */
    long expected = (NPAGES - 2) * (PSIZE / 1024);
    int saving_ok = (saved_kb >= expected / 3);
    LOGI("Expected savings >= %ld kB (1/3 of ideal %ld kB): %s\n",
         expected / 3, expected, saving_ok ? "OK" : "INSUFFICIENT");

    /* Verify file content unchanged */
    int content_ok = 1;
    uint8_t *buf = malloc(PSIZE);
    for (int i = 0; i < NPAGES; i++) {
        if (pread(fd, buf, PSIZE, (off_t)i * PSIZE) != PSIZE) { content_ok = 0; break; }
        uint8_t exp = (i & 1) ? 0xBB : 0xAA;
        for (int j = 0; j < PSIZE; j++)
            if (buf[j] != exp) { content_ok = 0; break; }
        if (!content_ok) { LOGE("page %d corrupt\n", i); break; }
    }
    free(buf);

    scanner_stop();
    close(fd);
    rm_rf_simple(DIR);

    if (!content_ok) FAIL(T, "content corrupted after dedup");
    if (!saving_ok)  FAIL(T, "insufficient memory savings");
    PASS(T);
}
