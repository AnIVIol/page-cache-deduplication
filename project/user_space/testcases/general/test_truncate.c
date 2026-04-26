/* test_truncate.c - truncate file with shared pages
 * gcc -Wall -Wextra -O2 -o test_truncate test_truncate.c
 */
#include "dedup_test_common.h"

#define DIR    "/tmp/dedup_t14"
#define FA     DIR "/a.dat"
#define FB     DIR "/b.dat"
#define FSIZE  (256 * 1024)

int main(void)
{
    const char *T = "1.4-truncate";
    LOGI("=== %s ===\n", T);
    mkdir_p(DIR);

    int fda = create_pattern_file(FA, FSIZE, 0x2222);
    int fdb = create_pattern_file(FB, FSIZE, 0x2222);
    if (fda < 0 || fdb < 0) DIE("create");

    drop_caches();
    prewarm_file(fda, FSIZE);
    prewarm_file(fdb, FSIZE);

    scanner_interval("100");
    scanner_register(FA);
    scanner_register(FB);
    if (scanner_start() < 0) FAIL(T, "scanner start");
    sleep(3);

    /* Truncate A to half size */
    if (ftruncate(fda, FSIZE / 2) < 0) DIE("ftruncate halve");

    /* Restore size */
    if (ftruncate(fda, FSIZE) < 0) DIE("ftruncate restore");

    /* New tail should be zero */
    uint8_t buf[4096];
    int tail_zero_ok = 1;
    if (pread(fda, buf, sizeof(buf), FSIZE - sizeof(buf)) != sizeof(buf)) DIE("pread tail");
    for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != 0) { tail_zero_ok = 0; break; }

    /* B must still be intact */
    int b_ok = (verify_pattern(fdb, FSIZE, 0x2222) == 0);

    /* Truncate to 0 then back */
    if (ftruncate(fda, 0) < 0) DIE("trunc 0");
    if (ftruncate(fda, FSIZE) < 0) DIE("re-extend");

    /* B again must still be intact */
    int b_ok2 = (verify_pattern(fdb, FSIZE, 0x2222) == 0);

    /* Quick dmesg sanity (best-effort, just inform user) */
    LOGI("(Inspect dmesg manually for warnings/oops)\n");

    scanner_stop();
    close(fda); close(fdb);
    rm_rf_simple(DIR);

    if (!tail_zero_ok) FAIL(T, "extended region not zero");
    if (!b_ok || !b_ok2) FAIL(T, "sibling B corrupted by A truncate");
    PASS(T);
}
