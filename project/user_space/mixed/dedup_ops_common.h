/* dedup_ops_common.h - operation framework for concurrent dedup tests
 *
 * Provides:
 *   - file_state_t : tracks fd, current size, and a per-byte SHADOW buffer
 *     for what we believe the file contents should be.
 *   - op_read / op_write / op_truncate / op_cp : each operation atomically
 *     mutates the file AND the shadow under per-file mutex(es), so that
 *     after all threads stop, verify_all() can compare actual file bytes
 *     against the shadow and prove correctness.
 *
 * Lock ordering for op_cp: lock the file_state_t with the LOWER address
 * first to avoid deadlock between two cp threads.
 */
#ifndef DEDUP_OPS_COMMON_H
#define DEDUP_OPS_COMMON_H

#include "dedup_test_common.h"
#include <pthread.h>
#include <stdint.h>

#define MAX_FSIZE   (256 * 1024)
#define INIT_FSIZE  (256 * 1024)
#define INIT_SEED   0x4242

typedef struct file_state {
    int             fd;
    char            path[256];
    pthread_mutex_t lock;
    size_t          size;       /* current logical file size */
    size_t          cap;        /* shadow buffer capacity */
    uint8_t        *expected;   /* SHADOW: bytes [0,size) are valid */
} file_state_t;

static inline int fs_init(file_state_t *fs, const char *path)
{
    memset(fs, 0, sizeof(*fs));
    snprintf(fs->path, sizeof(fs->path), "%s", path);
    fs->cap  = MAX_FSIZE;
    fs->size = INIT_FSIZE;
    fs->expected = calloc(1, fs->cap);
    if (!fs->expected) return -1;
    fill_pattern(fs->expected, fs->size, INIT_SEED);

    fs->fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fs->fd < 0) { free(fs->expected); return -1; }

    size_t off = 0;
    while (off < fs->size) {
        ssize_t w = pwrite(fs->fd, fs->expected + off, fs->size - off, off);
        if (w <= 0) { close(fs->fd); free(fs->expected); return -1; }
        off += w;
    }
    fsync(fs->fd);
    pthread_mutex_init(&fs->lock, NULL);
    return 0;
}

static inline void fs_destroy(file_state_t *fs)
{
    if (fs->fd >= 0) { close(fs->fd); fs->fd = -1; }
    free(fs->expected); fs->expected = NULL;
    pthread_mutex_destroy(&fs->lock);
}

/* All op_*() return: 0 ok, -1 I/O error, -2 data mismatch (read only) */

static inline int op_read(file_state_t *fs, off_t off, size_t len)
{
    pthread_mutex_lock(&fs->lock);
    if (fs->size == 0 || (size_t)off >= fs->size) {
        pthread_mutex_unlock(&fs->lock); return 0;
    }
    if (off + len > fs->size) len = fs->size - off;
    if (len == 0) { pthread_mutex_unlock(&fs->lock); return 0; }

    uint8_t *buf = malloc(len);
    if (!buf) { pthread_mutex_unlock(&fs->lock); return -1; }
    ssize_t r = pread(fs->fd, buf, len, off);
    int rc = 0;
    if (r != (ssize_t)len) {
        rc = -1;
    } else if (memcmp(buf, fs->expected + off, len) != 0) {
        for (size_t i = 0; i < len; i++)
            if (buf[i] != fs->expected[off + i]) {
                LOGE("MISMATCH %s off=%ld+%zu got=0x%02x exp=0x%02x\n",
                     fs->path, (long)off, i, buf[i], fs->expected[off + i]);
                break;
            }
        rc = -2;
    }
    free(buf);
    pthread_mutex_unlock(&fs->lock);
    return rc;
}

static inline int op_write(file_state_t *fs, off_t off, size_t len, uint8_t val)
{
    pthread_mutex_lock(&fs->lock);
    if (fs->size == 0 || (size_t)off >= fs->size) {
        pthread_mutex_unlock(&fs->lock); return 0;
    }
    if (off + len > fs->size) len = fs->size - off;
    if (len == 0) { pthread_mutex_unlock(&fs->lock); return 0; }

    uint8_t *buf = malloc(len);
    if (!buf) { pthread_mutex_unlock(&fs->lock); return -1; }
    memset(buf, val, len);
    ssize_t w = pwrite(fs->fd, buf, len, off);
    int rc = 0;
    if (w == (ssize_t)len) memcpy(fs->expected + off, buf, len);
    else rc = -1;
    free(buf);
    pthread_mutex_unlock(&fs->lock);
    return rc;
}

/* Truncate to new_size (capped at MAX_FSIZE). On extension, the new
 * tail region is zero-filled in both the file (POSIX guarantee) and
 * our shadow. */
static inline int op_truncate(file_state_t *fs, size_t new_size)
{
    if (new_size > MAX_FSIZE) new_size = MAX_FSIZE;
    pthread_mutex_lock(&fs->lock);
    if (ftruncate(fs->fd, new_size) < 0) {
        pthread_mutex_unlock(&fs->lock); return -1;
    }
    if (new_size > fs->size)
        memset(fs->expected + fs->size, 0, new_size - fs->size);
    fs->size = new_size;
    pthread_mutex_unlock(&fs->lock);
    return 0;
}

/* Copy src -> dst (entire file). Locks both in pointer order. */
static inline int op_cp(file_state_t *src, file_state_t *dst)
{
    if (src == dst) return 0;
    file_state_t *first  = (src < dst) ? src : dst;
    file_state_t *second = (src < dst) ? dst : src;
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);

    int rc = 0;
    if (ftruncate(dst->fd, src->size) < 0) { rc = -1; goto out; }

    uint8_t buf[65536];
    size_t off = 0;
    while (off < src->size) {
        size_t n = src->size - off; if (n > sizeof(buf)) n = sizeof(buf);
        ssize_t r = pread(src->fd, buf, n, off);
        if (r != (ssize_t)n) { rc = -1; goto out; }
        ssize_t w = pwrite(dst->fd, buf, n, off);
        if (w != (ssize_t)n) { rc = -1; goto out; }
        off += n;
    }
    /* Mirror to dst shadow */
    memcpy(dst->expected, src->expected, src->size);
    dst->size = src->size;
out:
    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    return rc;
}

/* Compare actual file content to shadow.  Optionally drops caches first. */
static inline int verify_all(file_state_t *files, int n, int drop_first)
{
    if (drop_first) { sync(); drop_caches(); }
    int failed = 0;
    for (int i = 0; i < n; i++) {
        file_state_t *fs = &files[i];
        pthread_mutex_lock(&fs->lock);
        struct stat st;
        if (fstat(fs->fd, &st) < 0) {
            LOGE("%s: fstat: %s\n", fs->path, strerror(errno));
            failed++; pthread_mutex_unlock(&fs->lock); continue;
        }
        if ((size_t)st.st_size != fs->size) {
            LOGE("%s: size mismatch (kernel=%ld shadow=%zu)\n",
                 fs->path, (long)st.st_size, fs->size);
            failed++; pthread_mutex_unlock(&fs->lock); continue;
        }
        if (fs->size == 0) {
            LOGI("%s: OK (empty)\n", fs->path);
            pthread_mutex_unlock(&fs->lock); continue;
        }
        uint8_t *buf = malloc(fs->size);
        if (!buf) { failed++; pthread_mutex_unlock(&fs->lock); continue; }
        size_t off = 0;
        int io_ok = 1;
        while (off < fs->size) {
            ssize_t r = pread(fs->fd, buf + off, fs->size - off, off);
            if (r <= 0) { io_ok = 0; break; }
            off += r;
        }
        if (!io_ok) { LOGE("%s: read failed\n", fs->path); failed++; }
        else if (memcmp(buf, fs->expected, fs->size) != 0) {
            size_t mm = 0, first_at = 0; int found = 0;
            for (size_t k = 0; k < fs->size; k++) {
                if (buf[k] != fs->expected[k]) {
                    if (!found) { first_at = k; found = 1; }
                    mm++;
                }
            }
            LOGE("%s: %zu mismatches (first @%zu got=0x%02x exp=0x%02x)\n",
                 fs->path, mm, first_at, buf[first_at], fs->expected[first_at]);
            failed++;
        } else {
            LOGI("%s: OK (size %zu)\n", fs->path, fs->size);
        }
        free(buf);
        pthread_mutex_unlock(&fs->lock);
    }
    return failed;
}

/* Convenience: register a list of files with the scanner. */
static inline int register_all(file_state_t *files, int n)
{
    for (int i = 0; i < n; i++)
        if (scanner_register(files[i].path) < 0) return -1;
    return 0;
}

#endif /* DEDUP_OPS_COMMON_H */
