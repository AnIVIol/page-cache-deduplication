/* bench_savings.c - measure dedup memory savings as a function of:
 *   file_size  x  num_files  x  share_pct  x  scanner_interval
 *
 * Also samples scanner kthread CPU% and slab delta during the sweep.
 * Outputs a CSV ready for plotting/spreadsheets.
 *
 * gcc -Wall -Wextra -O2 -o bench_savings bench_savings.c
 */
#include "bench_common.h"

#define DIR "/tmp/bench_savings"

/* Default sweep matrix - edit or override at runtime if desired */
static const long file_sizes_mb[]  = { 1, 16, 64, 256 };
static const int  share_pcts[]     = { 0, 25, 50, 75, 100 };
static const int  intervals_ms[]   = { 50, 200, 1000 };
static const int  num_files        = 4;

#define ARRLEN(a) (sizeof(a)/sizeof((a)[0]))

static int run_one(long fsize_mb, int share, int interval_ms, FILE *csv)
{
    long fsize = fsize_mb * 1024L * 1024L;

    rm_rf_simple(DIR); mkdir_p(DIR);

    int  fds[16];   char paths[16][256];
    LOGI("Creating %d x %ldMB files (share=%d%%)...\n", num_files, fsize_mb, share);
    double t_create0 = now_sec();
    for (int i = 0; i < num_files; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/f%d.dat", DIR, i);
        fds[i] = create_share_file(paths[i], fsize, (uint32_t)i, share);
        if (fds[i] < 0) { LOGE("create"); return -1; }
    }
    LOGI("File creation: %.2fs\n", now_sec() - t_create0);

    drop_caches();
    LOGI("Prewarming page cache...\n");
    double t_pw = now_sec();
    for (int i = 0; i < num_files; i++) prewarm_file_fast(fds[i], fsize);
    LOGI("Prewarm: %.2fs\n", now_sec() - t_pw);

    meminfo_snap_t before;  meminfo_snapshot(&before);

    char buf[16]; snprintf(buf, sizeof(buf), "%d", interval_ms);
    scanner_interval(buf);
    for (int i = 0; i < num_files; i++) scanner_register(paths[i]);
    if (scanner_start() < 0) { LOGE("scanner start"); return -1; }

    pid_t spid = find_kthread_pid("dedup_scanner");
    LOGI("Scanner pid=%d interval=%dms\n", spid, interval_ms);

    /* Wait long enough for at least 1 full sweep over all data.
     * Scanner reads ~ NUM_FILES*fsize bytes; budget ~32MB/s pessimistically. */
    int wait_secs = 5 + (int)((long)num_files * fsize_mb / 32);
    if (wait_secs > 60) wait_secs = 60;

    double cpu_pct = (spid > 0) ? sample_cpu_pct(spid, wait_secs) : -1.0;

    meminfo_snap_t after;  meminfo_snapshot(&after);

    long pages_per_file  = fsize / PG_SIZE;
    long shared_pages    = pages_per_file * share / 100;
    long ideal_savings   = shared_pages * (num_files - 1) * (PG_SIZE / 1024);
    long saved_kb        = before.cached - after.cached;
    double pct_of_ideal  = (ideal_savings > 0) ?
                           (100.0 * (double)saved_kb / (double)ideal_savings) : 0.0;

    LOGI("cached %ld -> %ld kB | saved %ld kB (%.1f%% of ideal %ld kB) | "
         "scanner CPU %.2f%% | slab delta %ld kB\n",
         before.cached, after.cached, saved_kb, pct_of_ideal,
         ideal_savings, cpu_pct, after.slab - before.slab);

    fprintf(csv, "%ld,%d,%d,%d,%d,%ld,%ld,%ld,%ld,%.2f,%.2f,%ld,%ld\n",
            fsize_mb, num_files, share, interval_ms, wait_secs,
            before.cached, after.cached, saved_kb, ideal_savings,
            pct_of_ideal, cpu_pct,
            after.slab - before.slab,
            after.unevictable - before.unevictable);
    fflush(csv);

    scanner_stop();
    for (int i = 0; i < num_files; i++) close(fds[i]);
    rm_rf_simple(DIR);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "bench_savings.csv";
    FILE *csv = fopen(out, "w");
    if (!csv) DIE("fopen %s", out);
    fprintf(csv, "file_size_mb,num_files,share_pct,interval_ms,wait_sec,"
                 "cached_before_kb,cached_after_kb,saved_kb,ideal_kb,"
                 "pct_of_ideal,scanner_cpu_pct,slab_delta_kb,unevictable_delta_kb\n");

    int total = (int)(ARRLEN(file_sizes_mb) * ARRLEN(share_pcts) * ARRLEN(intervals_ms));
    int n = 0;
    for (size_t a = 0; a < ARRLEN(file_sizes_mb); a++)
      for (size_t b = 0; b < ARRLEN(share_pcts);   b++)
        for (size_t c = 0; c < ARRLEN(intervals_ms); c++) {
            n++;
            LOGI("\n[%d/%d] === fsize=%ldMB share=%d%% interval=%dms ===\n",
                 n, total, file_sizes_mb[a], share_pcts[b], intervals_ms[c]);
            run_one(file_sizes_mb[a], share_pcts[b], intervals_ms[c], csv);
        }
    fclose(csv);
    LOGI("\nResults written to %s\n", out);
    return 0;
}
