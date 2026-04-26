/* bench_common.h - utilities for dedup benchmarks
 *  - share-controlled file generation (variable %shared pages)
 *  - /proc/[pid]/stat CPU sampling (for the dedup_scanner kthread)
 *  - /proc/meminfo snapshots (Cached, Swap, Slab, Unevictable, ...)
 *  - high-precision timing
 */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include "dedup_test_common.h"
#include <pthread.h>
#include <dirent.h>
#include <sys/sysinfo.h>

#define PG_SIZE 4096

/* ---------- timing ---------- */
static inline double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------- file generation with controlled %shared pages ---------- */
/* First share_pct% of pages are identical across all files (will dedup);
 * remaining pages are file-unique. Uses 1MB write chunks for speed.
 * Returns open fd on success (caller closes). */
static inline int create_share_file(const char *path, size_t size,
                                    uint32_t file_id, int share_pct)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return -1;

    const size_t CHUNK_PAGES = 256;       /* 1 MB chunks */
    const size_t CHUNK_BYTES = CHUNK_PAGES * PG_SIZE;
    uint8_t *chunk = malloc(CHUNK_BYTES);
    if (!chunk) { close(fd); return -1; }

    size_t pages  = size / PG_SIZE;
    size_t shared = (pages * (size_t)share_pct) / 100;

    size_t p = 0;
    while (p < pages) {
        size_t batch = CHUNK_PAGES;
        if (p + batch > pages) batch = pages - p;
        for (size_t k = 0; k < batch; k++) {
            uint8_t *pg = chunk + k * PG_SIZE;
            if (p + k < shared) {
                /* identical content across files */
                for (size_t i = 0; i < PG_SIZE; i++)
                    pg[i] = (uint8_t)((i * 7 + 0xAA) & 0xff);
            } else {
                /* file-unique content */
                uint64_t key = (uint64_t)file_id * 0x9E3779B1ULL +
                               (uint64_t)(p + k) * 0x1234567ULL;
                for (size_t i = 0; i < PG_SIZE; i++)
                    pg[i] = (uint8_t)((i * 7 + key) & 0xff);
            }
        }
        ssize_t w = pwrite(fd, chunk, batch * PG_SIZE, p * PG_SIZE);
        if (w != (ssize_t)(batch * PG_SIZE)) {
            free(chunk); close(fd); return -1;
        }
        p += batch;
    }
    fsync(fd);
    free(chunk);
    return fd;
}

/* ---------- fast page-cache prewarm ---------- */
static inline int prewarm_file_fast(int fd, size_t bytes)
{
    posix_fadvise(fd, 0, bytes, POSIX_FADV_WILLNEED);
    char *buf = malloc(1 << 20);
    if (!buf) return -1;
    size_t off = 0;
    while (off < bytes) {
        size_t n = bytes - off; if (n > (1u << 20)) n = (1u << 20);
        ssize_t r = pread(fd, buf, n, off);
        if (r <= 0) { free(buf); return -1; }
        off += r;
    }
    free(buf);
    return 0;
}

/* ---------- /proc inspection ---------- */
static inline pid_t find_kthread_pid(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    pid_t found = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[280], comm[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
	FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strchr(comm, '\n'); if (nl) *nl = 0;
            if (strcmp(comm, name) == 0) { found = (pid_t)atoi(de->d_name); fclose(f); break; }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Read utime+stime from /proc/[pid]/stat (in ticks). */
static inline int read_proc_cpu(pid_t pid, unsigned long *utime, unsigned long *stime)
{
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    char *p = strrchr(line, ')');         /* skip past comm in parens */
    if (!p) return -1;
    p++;
    int field = 3;                        /* next is field 3 (state) */
    while (*p && field < 14) { if (*p == ' ') field++; p++; }
    return (sscanf(p, "%lu %lu", utime, stime) == 2) ? 0 : -1;
}

/* Sample CPU% over 'seconds' wall seconds. */
static inline double sample_cpu_pct(pid_t pid, int seconds)
{
    unsigned long u1, s1, u2, s2;
    if (read_proc_cpu(pid, &u1, &s1) < 0) return -1.0;
    sleep(seconds);
    if (read_proc_cpu(pid, &u2, &s2) < 0) return -1.0;
    long hz = sysconf(_SC_CLK_TCK);
    double cpu_secs = (double)((u2 + s2) - (u1 + s1)) / (double)hz;
    return (cpu_secs / (double)seconds) * 100.0;
}

/* ---------- /proc/meminfo snapshot ---------- */
typedef struct {
    long mem_free, mem_avail, cached, buffers;
    long slab, sreclaimable, sunreclaim;
    long unevictable, swap_used;
} meminfo_snap_t;

static inline int meminfo_snapshot(meminfo_snap_t *s)
{
    memset(s, 0, sizeof(*s));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long swap_total = 0, swap_free = 0;
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "MemFree: %ld kB",      &s->mem_free);
        sscanf(line, "MemAvailable: %ld kB", &s->mem_avail);
        sscanf(line, "Cached: %ld kB",       &s->cached);
        sscanf(line, "Buffers: %ld kB",      &s->buffers);
        sscanf(line, "Slab: %ld kB",         &s->slab);
        sscanf(line, "SReclaimable: %ld kB", &s->sreclaimable);
        sscanf(line, "SUnreclaim: %ld kB",   &s->sunreclaim);
        sscanf(line, "Unevictable: %ld kB",  &s->unevictable);
        sscanf(line, "SwapTotal: %ld kB",    &swap_total);
        sscanf(line, "SwapFree: %ld kB",     &swap_free);
    }
    fclose(f);
    s->swap_used = swap_total - swap_free;
    return 0;
}

#endif /* BENCH_COMMON_H */
