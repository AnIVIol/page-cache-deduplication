/*
 * bench_truncate.c
 *
 * C port of bench_truncate.sh — measure truncate() latency with and without
 * page-cache deduplication, across a matrix of (size, share, groups).
 *
 * Three phases per config:
 *   Phase 1 — baseline truncate, scanner OFF
 *   Phase 2 — truncate after the scanner has deduplicated the files
 *   Phase 3 — truncate while a concurrent fio read load runs on file_0
 *
 * Build: gcc -Wall -Wextra -O2 -o bench_truncate bench_truncate.c
 * Run  : sudo ./bench_truncate [report.csv]
 *
 * Env overrides (same names as the shell script):
 *   GEN_BIN=./gen_files
 *   SCANNER_KO=../../modules/scanner.ko
 *   WORK_DIR=/tmp/bench_truncate
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Buffer sizes chosen so PATHBUF > DIRBUF + filename suffix.
 * Avoids -Wformat-truncation warnings without needing extra checks. */
#define DIRBUF      1024
#define PATHBUF     (DIRBUF + 64)
#define CMDBUF      8192

/* ---------- Configuration matrix ---------- */
static const int SIZES[]       = { 64, 128, 256 };
static const int SHARES[]      = { 0, 30, 60, 90 };
static const int GROUPS_LIST[] = { 1, 3 };
#define N_SIZES   (sizeof(SIZES)        / sizeof(SIZES[0]))
#define N_SHARES  (sizeof(SHARES)       / sizeof(SHARES[0]))
#define N_GROUPS  (sizeof(GROUPS_LIST)  / sizeof(GROUPS_LIST[0]))

#define RUNS         1
#define NUM_FILES    4
#define FIO_RUNTIME  10        /* seconds of background fio load in Phase 3 */

/* Defaults; can be overridden via env vars or argv */
static const char *gen_bin     = "./gen_files";
static const char *scanner_ko  = "../modules/scanner.ko";
static const char *report_path = "truncate_report.csv";
static const char *work_dir    = "/tmp/bench_truncate";

static FILE *report_fp = NULL;
static volatile sig_atomic_t g_stop = 0;

#define LOG(fmt, ...)  do { fprintf(stdout, fmt, ##__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(fmt, ...)  do { fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); } while (0)

/* ---------- timing ---------- */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------- sysfs / proc helpers ---------- */
static int write_file_str(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t w = write(fd, value, strlen(value));
    int e = errno;
    close(fd);
    return (w < 0) ? (errno = e, -1) : 0;
}

static int sysctl_drop_caches(void)
{
    sync();
    return write_file_str("/proc/sys/vm/drop_caches", "3\n");
}

static int scanner_register(const char *p) {
    return write_file_str("/sys/kernel/dedup_scanner/scan_file", p);
}
static int scanner_set_run(const char *v) {
    return write_file_str("/sys/kernel/dedup_scanner/run", v);
}
static int scanner_set_interval(const char *v) {
    return write_file_str("/sys/kernel/dedup_scanner/interval", v);
}

/* /proc/meminfo: Unevictable: NNN kB */
static long unevict_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "Unevictable: %ld kB", &val) == 1) break;
    fclose(f);
    return val;
}

/* ---------- module load/unload ---------- */
static void scanner_start(void)
{
    char cmd[CMDBUF];
    snprintf(cmd, sizeof(cmd), "insmod '%s' 2>/dev/null", scanner_ko);
    (void)system(cmd);                           /* OK if already loaded */
    scanner_set_interval("200");
    scanner_set_run("1");
}

static void scanner_stop(void)
{
    /* Best-effort; ignore errors so this is safe to call from cleanup paths. */
    (void)scanner_set_run("0");
    sleep(1);
    (void)system("rmmod scanner 2>/dev/null");
}

/* ---------- file ops ---------- */
static int warm_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char *buf = malloc(1u << 20);                /* 1 MB */
    if (!buf) { close(fd); return -1; }
    while (read(fd, buf, 1u << 20) > 0) ;
    free(buf);
    close(fd);
    return 0;
}

/* Run gen_files via shell. */
static int gen_files_cmd(int n, int share, int groups, int sizeMB, const char *dir)
{
    char cmd[CMDBUF];
    snprintf(cmd, sizeof(cmd),
             "'%s' %d %d %d %d '%s' >/dev/null",
             gen_bin, n, share, groups, sizeMB, dir);
    return system(cmd);
}

/* Replicate file_0 -> file_1..file_(n-1). */
static int replicate_master(const char *dir, int n)
{
    char src[PATHBUF], dst[PATHBUF], cmd[CMDBUF];
    snprintf(src, sizeof(src), "%s/file_0.bin", dir);
    for (int i = 1; i < n; i++) {
        snprintf(dst, sizeof(dst), "%s/file_%d.bin", dir, i);
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src, dst);
        if (system(cmd) != 0) return -1;
    }
    return 0;
}

static void warm_and_register_all(const char *dir, int n)
{
    char path[PATHBUF], real[PATH_MAX];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "%s/file_%d.bin", dir, i);
        warm_file(path);
        if (realpath(path, real)) scanner_register(real);
        else                       scanner_register(path);
    }
}

/* Returns latency in ms, or -1.0 on error. */
static double time_truncate(const char *path, off_t newsize)
{
    double t0 = now_sec();
    int rc = truncate(path, newsize);
    double t1 = now_sec();
    if (rc != 0) return -1.0;
    return (t1 - t0) * 1000.0;
}

/* ---------- background fio ---------- */
static pid_t run_fio_bg(const char *file, int sizeMB, int runtime)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }

        char file_arg[PATHBUF + 16], size_arg[64], runtime_arg[64];
        snprintf(file_arg,    sizeof(file_arg),    "--filename=%s", file);
        snprintf(size_arg,    sizeof(size_arg),    "--size=%dM",    sizeMB);
        snprintf(runtime_arg, sizeof(runtime_arg), "--runtime=%d",  runtime);

        execlp("fio", "fio",
               "--name=stress",
               file_arg,
               "--rw=randread",
               "--bs=4k",
               size_arg,
               "--ioengine=sync",
               "--direct=0",
               "--numjobs=4",
               "--time_based",
               runtime_arg,
               "--group_reporting",
               (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* ---------- CSV emit ---------- */
static void emit_row(int size, int share, int groups, const char *phase,
                     int run, double latency_ms, int errors)
{
    long ue = unevict_kb();
    if (latency_ms < 0)
        fprintf(report_fp, "%d,%d,%d,%s,%d,NA,%d,%ld\n",
                size, share, groups, phase, run, errors, ue);
    else
        fprintf(report_fp, "%d,%d,%d,%s,%d,%.6f,%d,%ld\n",
                size, share, groups, phase, run, latency_ms, errors, ue);
    fflush(report_fp);
}

/* ---------- per-config runner ---------- */
static void run_config(int size, int share, int groups)
{
    char dir[DIRBUF], cmd[CMDBUF];
    snprintf(dir, sizeof(dir), "%s/%d_%d_%d", work_dir, size, share, groups);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    (void)system(cmd);
    mkdir(dir, 0755);

    off_t halfsize = (off_t)size * 1024 * 1024 / 2;

    LOG("===================================================\n");
    LOG("[*] SIZE=%dMB  SHARE=%d%%  GROUPS=%d\n", size, share, groups);
    LOG("===================================================\n");

    /* ============== PHASE 1 — baseline ============== */
    LOG("[*] Phase 1 (baseline truncate, dedup OFF)\n");
    int errs = 0;
    for (int r = 1; r <= RUNS && !g_stop; r++) {
        gen_files_cmd(1, share, groups, size, dir);
        sysctl_drop_caches();
        char path[PATHBUF];
        snprintf(path, sizeof(path), "%s/file_0.bin", dir);
        warm_file(path);

        double t = time_truncate(path, halfsize);
        if (t >= 0) emit_row(size, share, groups, "baseline", r, t, 0);
        else        { emit_row(size, share, groups, "baseline", r, -1, 1); errs++; }
    }
    LOG("  Phase 1 errors: %d\n", errs);

    /* ============== PHASE 2 — post-dedup ============== */
    LOG("[*] Phase 2 (truncate after dedup)\n");
    gen_files_cmd(1, share, groups, size, dir);
    replicate_master(dir, NUM_FILES);

    scanner_start();
    sysctl_drop_caches();
    warm_and_register_all(dir, NUM_FILES);
    sleep(3);                                  /* let dedup settle */
    LOG("  Unevictable after dedup: %ld kB\n", unevict_kb());

    errs = 0;
    for (int r = 1; r <= RUNS && !g_stop; r++) {
        char path[PATHBUF];
        snprintf(path, sizeof(path), "%s/file_%d.bin", dir, r - 1);
        double t = time_truncate(path, halfsize);
        if (t >= 0) emit_row(size, share, groups, "dedup", r, t, 0);
        else        { emit_row(size, share, groups, "dedup", r, -1, 1); errs++; }
    }
    LOG("  Phase 2 errors: %d\n", errs);
    sleep(3);
    scanner_stop();

    /* ============== PHASE 3 — fio + truncate stress ============== */
    LOG("[*] Phase 3 (truncate under concurrent fio read load)\n");
    gen_files_cmd(1, share, groups, size, dir);
    replicate_master(dir, NUM_FILES);

    scanner_start();
    sysctl_drop_caches();
    warm_and_register_all(dir, NUM_FILES);
    sleep(3);
    LOG("  Unevictable after dedup: %ld kB\n", unevict_kb());

    char file0[PATHBUF];
    snprintf(file0, sizeof(file0), "%s/file_0.bin", dir);
    pid_t fio_pid = run_fio_bg(file0, size, FIO_RUNTIME);

    errs = 0;
    for (int r = 1; r <= RUNS && !g_stop; r++) {
        int target = ((r - 1) % (NUM_FILES - 1)) + 1;   /* cycles 1..N-1 */
        char path[PATHBUF];
        snprintf(path, sizeof(path), "%s/file_%d.bin", dir, target);
        double t = time_truncate(path, halfsize);
        if (t >= 0) emit_row(size, share, groups, "stress", r, t, 0);
        else        { emit_row(size, share, groups, "stress", r, -1, 1); errs++; }
        sleep(1);
    }

    if (fio_pid > 0) { int st; waitpid(fio_pid, &st, 0); }
    LOG("  Phase 3 errors: %d\n", errs);

    scanner_stop();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    (void)system(cmd);
}

/* ---------- sanity helpers ---------- */
static int file_executable(const char *p) { return access(p, X_OK) == 0; }
static int file_exists    (const char *p) { return access(p, F_OK) == 0; }
static int command_exists(const char *cmd) {
    char check[256];
    snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", cmd);
    return system(check) == 0;
}

static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
    /* Env overrides like the bash version */
    const char *e;
    if ((e = getenv("GEN_BIN")))    gen_bin     = e;
    if ((e = getenv("SCANNER_KO"))) scanner_ko  = e;
    if ((e = getenv("WORK_DIR")))   work_dir    = e;
    if (argc > 1)                   report_path = argv[1];

    if (!file_executable(gen_bin)) {
        ERR("ERROR: '%s' not found or not executable. Build with 'make gen_files'.\n", gen_bin);
        return 1;
    }
    if (!file_exists(scanner_ko)) {
        ERR("ERROR: '%s' not found. Set SCANNER_KO=/path/to/scanner.ko if needed.\n", scanner_ko);
        return 1;
    }
    if (!command_exists("fio")) {
        ERR("ERROR: fio is not installed.\n");
        return 1;
    }
    if (geteuid() != 0) {
        ERR("ERROR: must be run as root (try: sudo %s).\n", argv[0]);
        return 1;
    }

    mkdir(work_dir, 0755);

    report_fp = fopen(report_path, "w");
    if (!report_fp) { perror("fopen report"); return 1; }
    fprintf(report_fp, "size_MB,share_pct,groups,phase,run,latency_ms,errors,unevict_kB\n");
    fflush(report_fp);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    for (size_t a = 0; a < N_SIZES  && !g_stop; a++)
      for (size_t b = 0; b < N_SHARES && !g_stop; b++)
        for (size_t c = 0; c < N_GROUPS && !g_stop; c++) {
            int sz = SIZES[a], sh = SHARES[b], gr = GROUPS_LIST[c];
            if (sh * gr > 100) continue;       /* invalid combo */
            run_config(sz, sh, gr);
        }

    fclose(report_fp);
    scanner_stop();
    rmdir(work_dir);                           /* may fail if non-empty: ignored */

    LOG("===================================================\n");
    LOG("[+] DONE. Results saved in %s\n", report_path);
    LOG("===================================================\n");
    LOG("    Tail of dmesg (check for warnings/oops):\n");
    LOG("---------------------------------------------------\n");
    (void)system("dmesg | tail -n 20");
    return 0;
}
