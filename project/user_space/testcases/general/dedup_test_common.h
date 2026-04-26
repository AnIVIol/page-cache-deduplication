/* dedup_test_common.h - shared helpers for dedup tests */
#ifndef DEDUP_TEST_COMMON_H
#define DEDUP_TEST_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>

#define SCANNER_RUN_PATH      "/sys/kernel/dedup_scanner/run"
#define SCANNER_FILE_PATH     "/sys/kernel/dedup_scanner/scan_file"
#define SCANNER_INTERVAL_PATH "/sys/kernel/dedup_scanner/interval"
#define DEDUP_FLUSH_PATH      "/sys/kernel/page_dedup/flush"

#define LOGI(fmt, ...) do { fprintf(stdout, "[INFO ] " fmt, ##__VA_ARGS__); fflush(stdout); } while (0)
#define LOGW(fmt, ...) do { fprintf(stdout, "[WARN ] " fmt, ##__VA_ARGS__); fflush(stdout); } while (0)
#define LOGE(fmt, ...) do { fprintf(stderr, "[ERROR] " fmt, ##__VA_ARGS__); fflush(stderr); } while (0)
#define DIE(fmt, ...) do { LOGE(fmt ": %s\n", ##__VA_ARGS__, strerror(errno)); exit(2); } while (0)

#define PASS(test) do { LOGI("RESULT %s: PASS\n", test); return 0; } while (0)
#define FAIL(test, fmt, ...) do { LOGE("RESULT %s: FAIL - " fmt "\n", test, ##__VA_ARGS__); return 1; } while (0)

static inline long get_meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            sscanf(line + klen, " %ld kB", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

static inline int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOGW("open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, value, strlen(value));
    int saved = errno;
    close(fd);
    if (w < 0) {
        LOGW("write(%s): %s\n", path, strerror(saved));
        return -1;
    }
    return 0;
}

static inline int scanner_register(const char *path) { return write_sysfs(SCANNER_FILE_PATH, path); }
static inline int scanner_start(void) { return write_sysfs(SCANNER_RUN_PATH, "1"); }
static inline int scanner_stop(void) { return write_sysfs(SCANNER_RUN_PATH, "0"); }
static inline int scanner_interval(const char *ms) { return write_sysfs(SCANNER_INTERVAL_PATH, ms); }
static inline int dedup_flush(void) { return write_sysfs(DEDUP_FLUSH_PATH, "1"); }

static inline void drop_caches(void)
{
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd >= 0) { (void)!write(fd, "1\n", 2); close(fd); }
}

/* Fill buffer with a deterministic pattern derived from seed */
static inline void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)((i * 7 + seed * 13) & 0xff);
}

/* Create a file of the given size filled with the seeded pattern */
static inline int create_pattern_file(const char *path, size_t bytes, uint32_t seed)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return -1;
    uint8_t buf[65536];
    size_t off = 0;
    while (off < bytes) {
        size_t n = bytes - off; if (n > sizeof(buf)) n = sizeof(buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)(((off + i) * 7 + seed * 13) & 0xff);
        ssize_t w = pwrite(fd, buf, n, off);
        if (w < 0) { close(fd); return -1; }
        off += w;
    }
    fsync(fd);
    return fd;
}

/* Read entire file into page cache (returns 0 on success) */
static inline int prewarm_file(int fd, size_t bytes)
{
    uint8_t buf[65536];
    size_t off = 0;
    while (off < bytes) {
        size_t n = bytes - off; if (n > sizeof(buf)) n = sizeof(buf);
        ssize_t r = pread(fd, buf, n, off);
        if (r <= 0) return -1;
        off += r;
    }
    return 0;
}

static inline int verify_pattern(int fd, size_t bytes, uint32_t seed)
{
    uint8_t buf[65536], exp[65536];
    size_t off = 0;
    while (off < bytes) {
        size_t n = bytes - off; if (n > sizeof(buf)) n = sizeof(buf);
        ssize_t r = pread(fd, buf, n, off);
        if (r <= 0) return -1;
        for (ssize_t i = 0; i < r; i++)
            exp[i] = (uint8_t)(((off + i) * 7 + seed * 13) & 0xff);
        if (memcmp(buf, exp, r) != 0) return -2;
        off += r;
    }
    return 0;
}

static inline int rm_rf_simple(const char *dir)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf -- '%s' 2>/dev/null", dir);
    return system(cmd);
}

static inline int mkdir_p(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0) return 0;
    return mkdir(dir, 0755);
}

#endif
