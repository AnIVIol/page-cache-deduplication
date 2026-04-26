/*
 * multithread_write_test.c
 *
 * Comprehensive multi-threaded write test for the page cache deduplication system.
 *
 * Build:  gcc -Wall -Wextra -O2 -pthread -o multithread_write_test multithread_write_test.c
 * Run as root (needs sysfs access): sudo ./multithread_write_test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>

/* ---------- Configurable parameters ---------- */
#define DEFAULT_NUM_FILES         8
#define DEFAULT_NUM_THREADS       16
#define DEFAULT_PAGES_PER_FILE    32
#define DEFAULT_OPS_PER_THREAD    2000
#define DEFAULT_TEST_DIR          "/tmp/dedup_mt_test"
#define DEFAULT_PAGE_SIZE         4096
#define DEFAULT_DEDUP_WAIT_SEC    3

#define SCANNER_RUN_PATH      "/sys/kernel/dedup_scanner/run"
#define SCANNER_FILE_PATH     "/sys/kernel/dedup_scanner/scan_file"
#define SCANNER_INTERVAL_PATH "/sys/kernel/dedup_scanner/interval"
#define DEDUP_FLUSH_PATH      "/sys/kernel/page_dedup/flush"

/* ---------- Globals ---------- */
static int g_num_files       = DEFAULT_NUM_FILES;
static int g_num_threads     = DEFAULT_NUM_THREADS;
static int g_pages_per_file  = DEFAULT_PAGES_PER_FILE;
static int g_ops_per_thread  = DEFAULT_OPS_PER_THREAD;
static long g_page_size      = DEFAULT_PAGE_SIZE;
static const char *g_dir     = DEFAULT_TEST_DIR;
static int g_verbose         = 0;

static pthread_barrier_t  g_start_barrier;
static pthread_mutex_t    g_log_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* Shadow buffer: per-byte expected value, plus per-byte (timestamp,thread)
 * to record latest writer.  Updated under per-byte spinlock-equivalent
 * serialized via 'last_writer_ts' compare-and-swap.
 *
 * For each file we keep:
 *    bytes[file_idx][offset]            -> expected byte value
 *    ts[file_idx][offset]               -> monotonic timestamp of last write
 *    writer[file_idx][offset]           -> thread id of last writer
 *
 * We also keep a per-file mutex to serialize the (write -> shadow update)
 * critical section, ensuring "last write wins" is consistent between
 * the kernel and our shadow.
 */
typedef struct file_state {
    int          fd;
    char        *path;
    size_t       size;
    uint8_t     *expected;       /* shadow contents */
    pthread_mutex_t lock;        /* serializes writes + shadow updates */
} file_state_t;

static file_state_t *g_files = NULL;

typedef struct thread_arg {
    int tid;
    unsigned int seed;
    long writes_done;
    long boundary_writes;
    long same_page_writes;
} thread_arg_t;

/* ---------- Logging ---------- */
static void vlog(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_log_mutex);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&g_log_mutex);
}

#define LOG(...)   do { vlog("[INFO] " __VA_ARGS__); } while (0)
#define WARN(...)  do { vlog("[WARN] " __VA_ARGS__); } while (0)
#define ERR(...)   do { vlog("[ERROR] " __VA_ARGS__); } while (0)
#define DBG(...)   do { if (g_verbose) vlog("[DBG ] " __VA_ARGS__); } while (0)

#define DIE(fmt, ...) do { ERR(fmt ": %s\n", ##__VA_ARGS__, strerror(errno)); exit(1); } while (0)

/* ---------- Helpers ---------- */
static long get_meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            sscanf(line + strlen(key), " %ld kB", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

static int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        WARN("open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, value, strlen(value));
    int saved = errno;
    close(fd);
    if (w < 0) {
        WARN("write(%s): %s\n", path, strerror(saved));
        return -1;
    }
    return 0;
}

static void drop_caches(void)
{
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "1\n", 2) < 0) WARN("drop_caches: %s\n", strerror(errno));
        close(fd);
    }
}

static void make_dir(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0) return;
    if (mkdir(p, 0755) < 0 && errno != EEXIST)
        DIE("mkdir(%s)", p);
}

/* Generate deterministic content for the initial identical files */
static void fill_identical_pattern(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)((i * 7 + 13) & 0xFF);
}

/* ---------- Setup phase ---------- */
static void setup_files(void)
{
    char path[512];
    g_files = calloc(g_num_files, sizeof(file_state_t));
    if (!g_files) DIE("calloc");

    size_t fsize = (size_t)g_pages_per_file * g_page_size;
    uint8_t *pattern = malloc(fsize);
    if (!pattern) DIE("malloc pattern");
    fill_identical_pattern(pattern, fsize);

    make_dir(g_dir);

    for (int i = 0; i < g_num_files; i++) {
        snprintf(path, sizeof(path), "%s/file_%03d.dat", g_dir, i);
        g_files[i].path = strdup(path);
        g_files[i].size = fsize;
        g_files[i].expected = malloc(fsize);
        if (!g_files[i].expected) DIE("malloc expected");
        memcpy(g_files[i].expected, pattern, fsize);

        int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fd < 0) DIE("open(%s)", path);

        size_t off = 0;
        while (off < fsize) {
            ssize_t w = pwrite(fd, pattern + off, fsize - off, off);
            if (w < 0) DIE("pwrite setup");
            off += w;
        }
        fsync(fd);
        g_files[i].fd = fd;

        if (pthread_mutex_init(&g_files[i].lock, NULL) != 0)
            DIE("pthread_mutex_init");
    }
    free(pattern);

    /* Force pages into page cache by reading them */
    for (int i = 0; i < g_num_files; i++) {
        uint8_t buf[8192];
        for (size_t off = 0; off < fsize; off += sizeof(buf)) {
            size_t n = fsize - off; if (n > sizeof(buf)) n = sizeof(buf);
            if (pread(g_files[i].fd, buf, n, off) < 0) DIE("pread setup");
        }
    }

    LOG("Created %d files of %zu bytes each at %s\n",
        g_num_files, fsize, g_dir);
}

static int start_scanner_and_register(void)
{
    /* Set fast scan interval */
    if (write_sysfs(SCANNER_INTERVAL_PATH, "100") < 0) {
        WARN("Could not set scanner interval (module loaded?)\n");
        return -1;
    }
    /* Register all files */
    for (int i = 0; i < g_num_files; i++) {
        if (write_sysfs(SCANNER_FILE_PATH, g_files[i].path) < 0) {
            WARN("Failed to register %s\n", g_files[i].path);
            return -1;
        }
    }
    if (write_sysfs(SCANNER_RUN_PATH, "1") < 0) {
        WARN("Failed to start scanner\n");
        return -1;
    }
    LOG("Scanner started; waiting %d sec for dedup\n", DEFAULT_DEDUP_WAIT_SEC);
    sleep(DEFAULT_DEDUP_WAIT_SEC);
    return 0;
}

static void stop_scanner(void)
{
    if (write_sysfs(SCANNER_RUN_PATH, "0") < 0)
        WARN("Failed to stop scanner\n");
    else
        LOG("Scanner stopped (cleanup_scanner_state ran)\n");
}

/* ---------- Worker threads ---------- */

/* Three flavors of writes to maximize coverage */
enum write_kind {
    WRITE_RANDOM = 0,
    WRITE_BOUNDARY,
    WRITE_SAMEPAGE,
    WRITE_KIND_MAX
};

static void do_write(thread_arg_t *ta, int file_idx,
                     off_t offset, size_t len, uint8_t value)
{
    file_state_t *fs = &g_files[file_idx];
    if (offset < 0 || (size_t)offset >= fs->size) return;
    if (offset + len > fs->size) len = fs->size - offset;
    if (len == 0) return;

    uint8_t *buf = malloc(len);
    if (!buf) return;
    memset(buf, value, len);

    /* Critical section: write file + shadow atomically with respect to
     * other threads on this file.  Real dedup CoW operates per-page
     * inside the kernel; this lock only orders our shadow vs. our writes.
     */
    pthread_mutex_lock(&fs->lock);
    ssize_t w = pwrite(fs->fd, buf, len, offset);
    if (w < 0) {
        pthread_mutex_unlock(&fs->lock);
        free(buf);
        WARN("tid=%d pwrite(file=%d off=%ld len=%zu): %s\n",
             ta->tid, file_idx, (long)offset, len, strerror(errno));
        return;
    }
    /* Mirror to shadow */
    memset(fs->expected + offset, value, w);
    pthread_mutex_unlock(&fs->lock);
    free(buf);

    ta->writes_done++;
    DBG("tid=%d wrote file=%d off=%ld len=%zu val=0x%02x\n",
        ta->tid, file_idx, (long)offset, (size_t)w, value);
}

static void *worker(void *arg)
{
    thread_arg_t *ta = arg;
    pthread_barrier_wait(&g_start_barrier);

    /* Each thread has a "signature" byte to make collisions interesting */
    uint8_t sig = (uint8_t)(0x40 + ta->tid);

    for (int op = 0; op < g_ops_per_thread; op++) {
        int file_idx = rand_r(&ta->seed) % g_num_files;
        enum write_kind kind = rand_r(&ta->seed) % WRITE_KIND_MAX;

        off_t offset;
        size_t len;
        size_t fsize = g_files[file_idx].size;

        switch (kind) {
        case WRITE_BOUNDARY: {
            /* Land within +/- 8 bytes of a random page boundary */
            int page = 1 + rand_r(&ta->seed) % (g_pages_per_file - 1);
            offset = (off_t)page * g_page_size - 8 +
                     (rand_r(&ta->seed) % 16);
            len = 4 + rand_r(&ta->seed) % 24;     /* 4..27 bytes */
            ta->boundary_writes++;
            break;
        }
        case WRITE_SAMEPAGE: {
            /* Hammer page 0 of a random file from many threads */
            offset = (rand_r(&ta->seed) % g_page_size);
            len = 1 + rand_r(&ta->seed) % 64;
            ta->same_page_writes++;
            break;
        }
        case WRITE_RANDOM:
        default:
            offset = rand_r(&ta->seed) % fsize;
            len = 1 + rand_r(&ta->seed) % 4096;
            break;
        }
        do_write(ta, file_idx, offset, len, sig);
    }
    return NULL;
}

/* ---------- Verification ---------- */
static int verify_file(int idx)
{
    file_state_t *fs = &g_files[idx];
    uint8_t *buf = malloc(fs->size);
    if (!buf) DIE("malloc verify");

    /* Drop user-side caching effects, but the page cache still serves it */
    fsync(fs->fd);

    size_t off = 0;
    while (off < fs->size) {
        ssize_t r = pread(fs->fd, buf + off, fs->size - off, off);
        if (r < 0) DIE("pread verify");
        if (r == 0) break;
        off += r;
    }

    int ok = 1;
    size_t mismatches = 0;
    for (size_t i = 0; i < fs->size && mismatches < 16; i++) {
        if (buf[i] != fs->expected[i]) {
            if (mismatches < 4) {
                ERR("file %d byte %zu: got 0x%02x expected 0x%02x\n",
                    idx, i, buf[i], fs->expected[i]);
            }
            mismatches++;
            ok = 0;
        }
    }
    if (!ok)
        ERR("file %d: %zu mismatches detected\n", idx, mismatches);
    free(buf);
    return ok;
}

/* ---------- Main ---------- */
static void usage(const char *p)
{
    printf("Usage: %s [opts]\n"
           "  -f N    number of files (default %d)\n"
           "  -t N    number of threads (default %d)\n"
           "  -p N    pages per file (default %d)\n"
           "  -o N    operations per thread (default %d)\n"
           "  -d DIR  test directory (default %s)\n"
           "  -v      verbose\n",
           p, DEFAULT_NUM_FILES, DEFAULT_NUM_THREADS,
           DEFAULT_PAGES_PER_FILE, DEFAULT_OPS_PER_THREAD,
           DEFAULT_TEST_DIR);
}

static volatile sig_atomic_t g_stop = 0;
static void sig_handler(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "f:t:p:o:d:vh")) != -1) {
        switch (c) {
        case 'f': g_num_files      = atoi(optarg); break;
        case 't': g_num_threads    = atoi(optarg); break;
        case 'p': g_pages_per_file = atoi(optarg); break;
        case 'o': g_ops_per_thread = atoi(optarg); break;
        case 'd': g_dir            = optarg;       break;
        case 'v': g_verbose        = 1;            break;
        case 'h': default: usage(argv[0]); return 0;
        }
    }
    g_page_size = sysconf(_SC_PAGESIZE);
    signal(SIGINT, sig_handler);

    LOG("=== Page Cache Dedup Multi-threaded Write Test ===\n");
    LOG("files=%d threads=%d pages/file=%d ops/thread=%d page_sz=%ld\n",
        g_num_files, g_num_threads, g_pages_per_file,
        g_ops_per_thread, g_page_size);

    /* ---- Setup ---- */
    drop_caches();
    long mem_before = get_meminfo_kb("Cached:");
    LOG("Cached before setup: %ld kB\n", mem_before);

    setup_files();
    long mem_after_setup = get_meminfo_kb("Cached:");
    LOG("Cached after setup: %ld kB (delta=%+ld kB)\n",
        mem_after_setup, mem_after_setup - mem_before);

    int scanner_ok = (start_scanner_and_register() == 0);
    long mem_after_dedup = get_meminfo_kb("Cached:");
    LOG("Cached after dedup wait: %ld kB (delta vs setup=%+ld kB)\n",
        mem_after_dedup, mem_after_dedup - mem_after_setup);

    if (scanner_ok) {
        long expected_savings_kb =
            (long)(g_num_files - 1) * g_pages_per_file * (g_page_size / 1024);
        LOG("Expected savings if fully deduped: ~%ld kB\n", expected_savings_kb);
        if (mem_after_setup - mem_after_dedup <
            (expected_savings_kb * 30) / 100) {
            WARN("Memory savings smaller than expected; dedup may not have run\n");
        } else {
            LOG("Memory savings indicate deduplication is active\n");
        }
    }

    /* ---- Spawn workers ---- */
    pthread_barrier_init(&g_start_barrier, NULL, g_num_threads + 1);
    pthread_t *tids = calloc(g_num_threads, sizeof(pthread_t));
    thread_arg_t *args = calloc(g_num_threads, sizeof(thread_arg_t));
    if (!tids || !args) DIE("calloc threads");

    for (int i = 0; i < g_num_threads; i++) {
        args[i].tid  = i;
        args[i].seed = (unsigned int)(time(NULL) ^ (i * 2654435761U));
        if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0)
            DIE("pthread_create");
    }

    LOG("Releasing %d workers...\n", g_num_threads);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_barrier_wait(&g_start_barrier);

    for (int i = 0; i < g_num_threads; i++)
        pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) +
                  (t1.tv_nsec - t0.tv_nsec) / 1e9;

    long total = 0, boundary = 0, samepage = 0;
    for (int i = 0; i < g_num_threads; i++) {
        total    += args[i].writes_done;
        boundary += args[i].boundary_writes;
        samepage += args[i].same_page_writes;
    }
    LOG("Workers done: %ld writes (%ld boundary, %ld same-page) in %.2fs (%.0f w/s)\n",
        total, boundary, samepage, secs, total / secs);

    /* ---- Verification ---- */
    LOG("Verifying file contents...\n");
    int failed = 0;
    for (int i = 0; i < g_num_files; i++) {
        if (!verify_file(i)) failed++;
    }

    long mem_after_writes = get_meminfo_kb("Cached:");
    LOG("Cached after writes: %ld kB (delta vs dedup=%+ld kB)\n",
        mem_after_writes, mem_after_writes - mem_after_dedup);
    if (scanner_ok && mem_after_writes <= mem_after_dedup) {
        WARN("Cached did not increase after writes; expected unmerges\n");
    } else if (scanner_ok) {
        LOG("Cached increased -> CoW unmerges occurred\n");
    }

    /* ---- Cleanup ---- */
    if (scanner_ok) stop_scanner();

    for (int i = 0; i < g_num_files; i++) {
        close(g_files[i].fd);
        unlink(g_files[i].path);
        free(g_files[i].path);
        free(g_files[i].expected);
        pthread_mutex_destroy(&g_files[i].lock);
    }
    free(g_files);
    free(tids);
    free(args);
    pthread_barrier_destroy(&g_start_barrier);

    LOG("=== RESULT: %s (%d/%d files OK) ===\n",
        failed == 0 ? "PASS" : "FAIL",
        g_num_files - failed, g_num_files);
    return failed == 0 ? 0 : 1;
}
