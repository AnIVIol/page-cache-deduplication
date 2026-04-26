/* test_write_cow.c - verify writes unmerge only the modified file
 * gcc -Wall -Wextra -O2 -o test_write_cow test_write_cow.c
 */
#include "dedup_test_common.h"

#define DIR    "/tmp/dedup_t13"
#define FA     DIR "/a.dat"
#define FB     DIR "/b.dat"
#define FSIZE  (1 * 1024 * 1024)

int main(void)
{
    const char *T = "1.3-write-cow";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fda = create_pattern_file(FA, FSIZE, 0x1111);
    int fdb = create_pattern_file(FB, FSIZE, 0x1111);
    if (fda < 0 || fdb < 0) DIE("create");

    drop_caches();
    prewarm_file(fda, FSIZE);
    prewarm_file(fdb, FSIZE);

    scanner_interval("100");
    scanner_register(FA);
    scanner_register(FB);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);

    long cached_after_dedup = get_meminfo_kb("Cached:");
    LOGI("Cached after dedup: %ld kB\n", cached_after_dedup);

    /* Write only to A, page 0 */
    const char *msg = "MODIFIED_BY_TEST_1_3";
    if (pwrite(fda, msg, strlen(msg), 0) < 0) DIE("pwrite");
    fsync(fda);

    /* Verify A reflects write, B unchanged */
    char buf[64];
    if (pread(fda, buf, strlen(msg), 0) != (ssize_t)strlen(msg)) DIE("pread A");
    int a_ok = (memcmp(buf, msg, strlen(msg)) == 0);

    int b_ok = (verify_pattern(fdb, FSIZE, 0x1111) == 0);

    /* Other pages of A still original */
    int a_other_ok = 1;
    uint8_t buf2[4096], exp[4096];
    if (pread(fda, buf2, 4096, 4096) != 4096) DIE("pread A page1");
    for (int i = 0; i < 4096; i++) exp[i] = (uint8_t)(((4096 + i) * 7 + 0x1111 * 13) & 0xff);
    if (memcmp(buf2, exp, 4096) != 0) a_other_ok = 0;

    long cached_after_write = get_meminfo_kb("Cached:");
    LOGI("Cached after write: %ld kB (delta %+ld kB)\n",
         cached_after_write, cached_after_write - cached_after_dedup);

    scanner_stop();
    close(fda); close(fdb);
    rm_rf_simple(DIR);

    if (!a_ok)        FAIL(T, "A doesn't reflect write");
    if (!b_ok)        FAIL(T, "B was modified by write to A");
    if (!a_other_ok)  FAIL(T, "non-written pages of A corrupted");
    PASS(T);
}
