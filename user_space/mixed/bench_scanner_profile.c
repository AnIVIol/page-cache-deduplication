/* bench_scanner_profile.c - take a fine-grained CPU/mem profile of the
 * dedup_scanner kthread across a sweep, for many configurations.
 *
 *  - Samples scanner CPU% in 1-second windows for the whole sweep
 *  - Records min/avg/peak CPU%
 *  - Records slab delta over the run
 *  - Records survivor count (unevictable_delta / 4kB)
 *  - Records sweep convergence time (when Cached stops decreasing)
 *
 * gcc -Wall -Wextra -O2 -o bench_scanner_profile bench_scanner_profile.c
 */
#include "bench_common.h"

#define DIR "/tmp/bench_scanprof"

static int profile_one(long fsize_mb, int n_files, int share, int interval_ms,
                       int max_secs, FILE *csv)
{
    size_t fsize = (size_t)fsize_mb * 1024 * 1024;
    rm_rf_simple(DIR); mkdir_p(DIR);

    char paths[32][256];
    int  fds[32];
    for (int i = 0; i < n_files; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/f%d.dat", DIR, i);
        fds[i] = create_share_file(paths[i], fsize, (uint32_t)i, share);
        if (fds[i] < 0) return -1;
    }
    drop_caches();
    for (int i = 0; i < n_files; i++) prewarm_file_fast(fds[i], fsize);

    meminfo_snap_t before; meminfo_snapshot(&before);

    char b[16]; snprintf(b, sizeof(b), "%d", interval_ms);
    scanner_interval(b);
    for (int i = 0; i < n_files; i++) scanner_register(paths[i]);
    if (scanner_start() < 0) return -1;

    pid_t spid = find_kthread_pid("dedup_scanner");
    if (spid <= 0) { LOGW("Could not find dedup_scanner kthread\n"); return -1; }

    /* Per-second CPU% sampling, also tracking convergence */
    double cpu_min = 1e9, cpu_max = 0, cpu_sum = 0;
    int    samples = 0;
    long   prev_cached = before.cached, stable = 0;
    int    converged_at = -1;

    for (int t = 0; t < max_secs; t++) {
        double pct = sample_cpu_pct(spid, 1);
        if (pct < 0) break;
        if (pct < cpu_min) cpu_min = pct;
        if (pct > cpu_max) cpu_max = pct;
        cpu_sum += pct; samples++;

        meminfo_snap_t cur; meminfo_snapshot(&cur);
        if (labs(cur.cached - prev_cached) < 64) stable++;   /* < 64 kB drift */
        else stable = 0;
        prev_cached = cur.cached;
        if (converged_at < 0 && stable >= 3) converged_at = t + 1;

        
        LOGI("  t=%ds cpu=%.2f%% cached=%ldkB stable=%lds\n",
             t+1, pct, cur.cached, stable);

        if (converged_at >= 0 && t > converged_at + 1) break;
    }

    meminfo_snap_t after; meminfo_snapshot(&after);
    double cpu_avg = (samples > 0) ? cpu_sum / samples : -1;

    long saved      = before.cached - after.cached;
    long slab_delta = after.slab - before.slab;
    long survivors_kb = after.unevictable - before.unevictable;

    LOGI("==> %ldMB x %d files share=%d%% interval=%dms\n",
         fsize_mb, n_files, share, interval_ms);
    LOGI("    saved=%ldkB cpu(min/avg/max)=%.2f/%.2f/%.2f%% "
         "slab_delta=%ldkB survivors~%ldkB converged@%ds\n",
         saved, cpu_min, cpu_avg, cpu_max,
         slab_delta, survivors_kb, converged_at);

    fprintf(csv, "%ld,%d,%d,%d,%ld,%.2f,%.2f,%.2f,%ld,%ld,%d,%d\n",
            fsize_mb, n_files, share, interval_ms,
            saved, cpu_min, cpu_avg, cpu_max,
            slab_delta, survivors_kb, samples, converged_at);
    fflush(csv);

    scanner_stop();
    for (int i = 0; i < n_files; i++) close(fds[i]);
    rm_rf_simple(DIR);
    return 0;
}


int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "bench_scanprof.csv";
    FILE *csv = fopen(out, "w");
    if (!csv) DIE("fopen %s", out);
    fprintf(csv, "file_size_mb,n_files,share_pct,interval_ms,saved_kb,"
                 "cpu_min_pct,cpu_avg_pct,cpu_max_pct,"
                 "slab_delta_kb,survivors_kb,samples,converged_sec\n");

    struct cfg { long mb; int n; int share; int iv; int max_s; };
    struct cfg cfgs[] = {
        /* small */
        {  1, 4, 100,  50, 15 },
        {  1, 4,  50,  50, 15 },
        /* medium */
        { 16, 4, 100,  50, 30 },
        { 16, 4,  50,  50, 30 },
        { 16, 4, 100, 200, 30 },
        { 16, 4, 100,1000, 60 },
        /* large */
        { 64, 4, 100,  50, 60 },
        { 64, 4,  50,  50, 60 },
        { 64, 4, 100, 200, 60 },
        /* very large */
        {256, 2, 100, 200, 90 },
        {256, 2,  50, 200, 90 },
    };

    for (size_t i = 0; i < sizeof(cfgs)/sizeof(cfgs[0]); i++) {
        LOGI("\n[%zu/%zu] === %ldMB x %d, share=%d%%, iv=%dms (max %ds) ===\n",
             i+1, sizeof(cfgs)/sizeof(cfgs[0]),
             cfgs[i].mb, cfgs[i].n, cfgs[i].share, cfgs[i].iv, cfgs[i].max_s);
        profile_one(cfgs[i].mb, cfgs[i].n, cfgs[i].share, cfgs[i].iv,
                    cfgs[i].max_s, csv);
    }
    fclose(csv);
    LOGI("\nResults in %s\n", out);
    return 0;
}
