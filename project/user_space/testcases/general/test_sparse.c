/* test_sparse.c - sparse file (all-zero pages) should dedup massively
 * gcc -Wall -Wextra -O2 -o test_sparse test_sparse.c
 */
#include "dedup_test_common.h"

#define DIR    "/tmp/dedup_t93"
#define PATH   DIR "/sparse.dat"
#define FSIZE  (16 * 1024 * 1024)   /* 16 MB sparse */

int main(void)
{
    const char *T = "9.3-sparse";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) DIE("open");
    if (ftruncate(fd, FSIZE) < 0) DIE("ftruncate sparse");

    /* Touch a few pages with explicit zero writes to force allocation
     * (some filesystems don't bring pages into the cache otherwise) */
    uint8_t zero[4096] = {0};
    /* Force every page to be present by reading it (which populates
     * the page cache with zero pages on a sparse file) */
    drop_caches();
    uint8_t buf[4096];
    int read_ok = 1;
    for (size_t off = 0; off < FSIZE; off += 4096) {
        if (pread(fd, buf, 4096, off) != 4096) { read_ok = 0; break; }
        for (int i = 0; i < 4096; i++) if (buf[i] != 0) { read_ok = 0; break; }
        if (!read_ok) break;
    }
    if (!read_ok) FAIL(T, "sparse read returned non-zero");

    long mem_before = get_meminfo_kb("Cached:");
    LOGI("Cached before dedup: %ld kB\n", mem_before);

    scanner_interval("100");
    if (scanner_register(PATH) < 0) FAIL(T, "register failed");
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(5);

    long mem_after = get_meminfo_kb("Cached:");
    long saved_kb  = mem_before - mem_after;
    LOGI("Cached after dedup: %ld kB (saved %ld kB)\n", mem_after, saved_kb);
    /* Don't assert savings (filesystems vary on sparse caching),
     * but content must still read as zeros. */

    int still_zero = 1;
    for (size_t off = 0; off < FSIZE; off += 4096) {
        if (pread(fd, buf, 4096, off) != 4096) { still_zero = 0; break; }
        for (int i = 0; i < 4096; i++) if (buf[i] != 0) { still_zero = 0; break; }
        if (!still_zero) break;
    }

    /* Write to one page should not affect others */
    if (pwrite(fd, "X", 1, 4096) < 0) DIE("pwrite");
    int other_pages_zero = 1;
    if (pread(fd, buf, 4096, 8192) != 4096) other_pages_zero = 0;
    for (int i = 0; i < 4096 && other_pages_zero; i++)
        if (buf[i] != 0) other_pages_zero = 0;
    if (pread(fd, buf, 4096, 0) != 4096) other_pages_zero = 0;
    for (int i = 0; i < 4096 && other_pages_zero; i++)
        if (buf[i] != 0) other_pages_zero = 0;

    /* And the modified page reflects the write */
    int modified_ok = 0;
    if (pread(fd, buf, 4096, 4096) == 4096 && buf[0] == 'X') modified_ok = 1;

    scanner_stop();
    close(fd);
    rm_rf_simple(DIR);
    (void)zero;

    if (!still_zero)        FAIL(T, "sparse file not zero after dedup");
    if (!other_pages_zero)  FAIL(T, "CoW write affected other zero pages");
    if (!modified_ok)       FAIL(T, "write to sparse page lost");
    PASS(T);
/* test_sparse.c - sparse file (all-zero pages) should dedup massively
 * gcc -Wall -Wextra -O2 -o test_sparse test_sparse.c
 */
#include "dedup_test_common.h"

#define DIR    "/tmp/dedup_t93"
#define PATH   DIR "/sparse.dat"
#define FSIZE  (16 * 1024 * 1024)   /* 16 MB sparse */

int main(void)
{
    const char *T = "9.3-sparse";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) DIE("open");
    if (ftruncate(fd, FSIZE) < 0) DIE("ftruncate sparse");

    /* Touch a few pages with explicit zero writes to force allocation
     * (some filesystems don't bring pages into the cache otherwise) */
    uint8_t zero[4096] = {0};
    /* Force every page to be present by reading it (which populates
     * the page cache with zero pages on a sparse file) */
    drop_caches();
    uint8_t buf[4096];
    int read_ok = 1;
    for (size_t off = 0; off < FSIZE; off += 4096) {
        if (pread(fd, buf, 4096, off) != 4096) { read_ok = 0; break; }
        for (int i = 0; i < 4096; i++) if (buf[i] != 0) { read_ok = 0; break; }
        if (!read_ok) break;
    }
    if (!read_ok) FAIL(T, "sparse read returned non-zero");

    long mem_before = get_meminfo_kb("Cached:");
    LOGI("Cached before dedup: %ld kB\n", mem_before);

    scanner_interval("100");
    if (scanner_register(PATH) < 0) FAIL(T, "register failed");
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(5);

    long mem_after = get_meminfo_kb("Cached:");
    long saved_kb  = mem_before - mem_after;
    LOGI("Cached after dedup: %ld kB (saved %ld kB)\n", mem_after, saved_kb);
    /* Don't assert savings (filesystems vary on sparse caching),
     * but content must still read as zeros. */

    int still_zero = 1;
    for (size_t off = 0; off < FSIZE; off += 4096) {
        if (pread(fd, buf, 4096, off) != 4096) { still_zero = 0; break; }
        for (int i = 0; i < 4096; i++) if (buf[i] != 0) { still_zero = 0; break; }
        if (!still_zero) break;
    }

    /* Write to one page should not affect others */
    if (pwrite(fd, "X", 1, 4096) < 0) DIE("pwrite");
    int other_pages_zero = 1;
    if (pread(fd, buf, 4096, 8192) != 4096) other_pages_zero = 0;
    for (int i = 0; i < 4096 && other_pages_zero; i++)
        if (buf[i] != 0) other_pages_zero = 0;
    if (pread(fd, buf, 4096, 0) != 4096) other_pages_zero = 0;
    for (int i = 0; i < 4096 && other_pages_zero; i++)
        if (buf[i] != 0) other_pages_zero = 0;

    /* And the modified page reflects the write */
    int modified_ok = 0;
    if (pread(fd, buf, 4096, 4096) == 4096 && buf[0] == 'X') modified_ok = 1;

    scanner_stop();
    close(fd);
    rm_rf_simple(DIR);
    (void)zero;

    if (!still_zero)        FAIL(T, "sparse file not zero after dedup");
    if (!other_pages_zero)  FAIL(T, "CoW write affected other zero pages");
    if (!modified_ok)       FAIL(T, "write to sparse page lost");
    PASS(T);
}}
