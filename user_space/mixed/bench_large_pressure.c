/* bench_op_latency.c - compare op latencies on merged vs unmerged files
 *
 * For each (file_size, share_pct, op), run twice:
 *   "unmerged": files exist, scanner has NOT seen them
 *   "merged"  : scanner has merged them; interval lengthened to avoid
 *               re-merging during measurement.
 *
 * Operations: read (full), write (1000 random 4KB writes), truncate cycle,
 * cp (read+write to a fresh destination).
 *
 * gcc -Wall -Wextra -O2 -o bench_op_latency bench_op_latency.c
 */
#include "bench_common.h"

#define DIR "/tmp/bench_ops"
#define N_RAND_WRITES 1000

static double measure_full_read(int fd, size_t size)
{
    char *buf = malloc(1 << 20);
    if (!buf) return -1;
    double t0 = now_sec();
    size_t off = 0;
    while (off < size) {
        size_t n = size - off; if (n > (1u << 20)) n = (1u << 20);
        ssize_t r = pread(fd, buf, n, off);
        if (r <= 0) break;
        off += r;
    }
    double t = now_sec() - t0;
    free(buf);
    return t;
}

static double measure_random_writes(int fd, size_t size, int n)
{
    char buf[4096];
    memset(buf, 0xCC, sizeof(buf));
    unsigned int seed = 0xC0FFEEu;
    size_t pages = size / PG_SIZE;
    if (pages == 0) return 0;
    double t0 = now_sec();
    for (int i = 0; i < n; i++) {
        off_t off = (off_t)(rand_r(&seed) % pages) * PG_SIZE;
        if (pwrite(fd, buf, sizeof(buf), off) < 0) break;
    }
    fdatasync(fd);
    return now_sec() - t0;
}

static double measure_truncate_cycle(int fd, size_t size)
{
    double t0 = now_sec();
    if (ftruncate(fd, (off_t)(size / 2)) < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0)        return -1;
    return now_sec() - t0;
}

static double measure_cp(const char *src, const char *dst, size_t size)
{
    int sf = open(src, O_RDONLY);                                if (sf < 0) return -1;
    int df = open(dst, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (df < 0) { close(sf); return -1; }
    char *buf = malloc(1 << 20);
    if (!buf) { close(sf); close(df); return -1; }
    double t0 = now_sec();
    size_t off = 0;
    while (off < size) {
        size_t n = size - off; if (n > (1u << 20)) n = (1u << 20);
        ssize_t r = pread(sf, buf, n, off); if (r <= 0) break;
        ssize_t w = pwrite(df, buf, r, off); if (w <= 0) break;
        off += r;
    }
    fdatasync(df);
    double t = now_sec() - t0;
    free(buf); close(sf); close(df);
    unlink(dst);
    return t;
}

static double bench_op_once(long fsize_mb, int share, const char *op, int merged, int interval_ms)
{
    size_t fsize = (size_t)fsize_mb * 1024 * 1024;
    rm_rf_simple(DIR); mkdir_p(DIR);

    char paths[2][256], cp_dst[256];
    int  fds[2];
    for (int i = 0; i < 2; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/f%d.dat", DIR, i);
        fds[i] = create_share_file(paths[i], fsize, (uint32_t)i, share);
        if (fds[i] < 0) return -1;
    }
    snprintf(cp_dst, sizeof(cp_dst), "%s/cp_dst.dat", DIR);

    drop_caches();
    for (int i = 0; i < 2; i++) prewarm_file_fast(fds[i], fsize);

    if (merged) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", interval_ms);
        scanner_interval(buf);
        scanner_register(paths[0]); scanner_register(paths[1]);
        if (scanner_start() < 0) goto fail;
        /* Wait for sweep to catch up */
        int wait_secs = 4 + (int)(fsize_mb / 16);
        if (wait_secs > 30) wait_secs = 30;
        sleep(wait_secs);
        /* Lengthen interval so scanner doesn't re-merge mid-measurement */
        scanner_interval("600000");
    }

    double dur = -1.0;
    if (!strcmp(op, "read"))     dur = measure_full_read(fds[0], fsize);
    else if (!strcmp(op, "cp"))  dur = measure_cp(paths[0], cp_dst, fsize);
    else if (!strcmp(op, "truncate")) dur = measure_truncate_cycle(fds[0], fsize);
    else if (!strcmp(op, "write"))    dur = measure_random_writes(fds[0], fsize, N_RAND_WRITES);

    if (merged) scanner_stop();
fail:
    for (int i = 0; i < 2; i++) close(fds[i]);
    rm_rf_simple(DIR);
    return dur;
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "bench_op_latency.csv";
    FILE *csv = fopen(out, "w");
    if (!csv) DIE("fopen %s", out);
    fprintf(csv, "file_size_mb,share_pct,op,phase,interval_ms,time_ms\n");

    long sizes_mb[]  = { 1, 16, 64, 256 };
    int  shares[]    = { 50, 100 };
    const char *ops[] = { "read", "cp", "truncate", "write" };
    int intervals[]  = { 100, 500 };

    for (size_t a = 0; a < sizeof(sizes_mb)/sizeof(sizes_mb[0]); a++)
      for (size_t b = 0; b < sizeof(shares)/sizeof(shares[0]); b++)
        for (size_t c = 0; c < sizeof(ops)/sizeof(ops[0]); c++) {
            LOGI("\n=== %ldMB share=%d%% op=%s ===\n",
                 sizes_mb[a], shares[b], ops[c]);

            double t_un = bench_op_once(sizes_mb[a], shares[b], ops[c], 0, 0);
            LOGI("  unmerged: %.2f ms\n", t_un * 1000);
            fprintf(csv, "%ld,%d,%s,unmerged,0,%.3f\n",
                    sizes_mb[a], shares[b], ops[c], t_un * 1000);

            for (size_t d = 0; d < sizeof(intervals)/sizeof(intervals[0]); d++) {
                double t_m = bench_op_once(sizes_mb[a], shares[b], ops[c], 1, intervals[d]);
                LOGI("  merged@%dms: %.2f ms (delta %+.1f%%)\n",
                     intervals[d], t_m * 1000,
                     (t_un > 0) ? 100.0 * (t_m - t_un) / t_un : 0);
                fprintf(csv, "%ld,%d,%s,merged,%d,%.3f\n",
                        sizes_mb[a], shares[b], ops[c], intervals[d], t_m * 1000);
            }
            fflush(csv);
        }
    fclose(csv);
    LOGI("\nResults in %s\n", out);
    return 0;
}
