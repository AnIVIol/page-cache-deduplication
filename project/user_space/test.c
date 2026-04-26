#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define MAX_PAGES 131072 

struct pfn_request {
    int fd;
    unsigned long *user_pfns;
    size_t max_pfns;
    size_t count;
};
#define GET_FILE_PFNS _IOWR('p', 1, struct pfn_request)

void print_cache_usage(const char *stage) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    char line[256];
    char memfree[256] = "MemFree: N/A\n", cached[256] = "Cached: N/A\n";
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemFree:", 8) == 0) strcpy(memfree, line);
        else if (strncmp(line, "Cached:", 7) == 0) strcpy(cached, line);
    }
    fclose(fp);
    printf("[%s]\n  -> %s  -> %s", stage, memfree, cached);
}

void read_file_fully(int fd) {
    char buffer[4096 * 16];
    lseek(fd, 0, SEEK_SET);
    while (read(fd, buffer, sizeof(buffer)) > 0);
}

/* New helper to dump PFNs only if requested */
void print_pfn_map(int ioctl_fd, int fd, const char *filename, unsigned long *pfns_buf) {
    struct pfn_request req = { .fd = fd, .user_pfns = pfns_buf, .max_pfns = MAX_PAGES };
    if (ioctl(ioctl_fd, GET_FILE_PFNS, &req) < 0) return;
    
    printf("\n--- Full PFN Map: %s ---\n", filename);
    for (size_t i = 0; i < req.count; i++) {
        printf("  Offset %5zu -> PFN %lu\n", i, pfns_buf[i]);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: sudo %s <X> <file1> ... <fileN> <verbose_flag>\n", argv[0]);
        printf("X            : Pages to merge into 1\n");
        printf("verbose_flag : 1 = print all PFNs, 0 = skip PFN maps\n");
        return 1;
    }

    // The last argument determines if we print the full maps
    int verbose = atoi(argv[argc - 1]);
    int num_files = argc - 3; // X, files..., verbose
    size_t X = strtoull(argv[1], NULL, 10);

    int *fds = malloc(num_files * sizeof(int));
    unsigned long *pfns = malloc(MAX_PAGES * sizeof(unsigned long));
    char *sysfs_buf = malloc(4096);

    for (int i = 0; i < num_files; i++) {
        fds[i] = open(argv[i + 2], O_RDONLY);
        if (fds[i] < 0) { perror("Failed to open file"); return 1; }
    }

    /* 1. Preparation */
    system("sync; echo 3 > /proc/sys/vm/drop_caches");
    print_cache_usage("Baseline");

    for (int i = 0; i < num_files; i++) {
        printf("Reading %s into Page Cache...\n", argv[i + 2]);
        read_file_fully(fds[i]);
    }

    int ioctl_fd = open("/dev/pfn_getter", O_RDWR);
    int sysfs_fd = open("/sys/kernel/page_dedup/merge_pfns", O_WRONLY);
    if (ioctl_fd < 0 || sysfs_fd < 0) { perror("Kernel interfaces missing"); return 1; }

    /* 2. Optional Pre-Merge PFN Printout */
    if (verbose) {
        printf("\n=== PRE-MERGE MAPPINGS ===\n");
        for (int i = 0; i < num_files; i++) 
            print_pfn_map(ioctl_fd, fds[i], argv[i + 2], pfns);
    }
    print_cache_usage("Before Merging");
    /* 3. Merging Logic */
    for (int i = 0; i < num_files; i++) {
        struct pfn_request req = { .fd = fds[i], .user_pfns = pfns, .max_pfns = MAX_PAGES };
        if (ioctl(ioctl_fd, GET_FILE_PFNS, &req) < 0) continue;

        for (size_t chunk_start = 0; chunk_start < req.count; chunk_start += X) {
            size_t chunk_size = (chunk_start + X <= req.count) ? X : (req.count - chunk_start);
            if (chunk_size < 2) break;

            unsigned long survivor_pfn = pfns[chunk_start];
            int len = snprintf(sysfs_buf, 4096, "%lu", survivor_pfn);
            for (size_t v = 1; v < chunk_size; v++) {
                char temp[32];
                int tlen = snprintf(temp, sizeof(temp), " %lu", pfns[chunk_start + v]);
                if (len + tlen >= 4090) break;
                strcpy(sysfs_buf + len, temp);
                len += tlen;
            }
            sysfs_buf[len++] = '\n';
            write(sysfs_fd, sysfs_buf, len);
        }
    }

    /* 4. Optional Post-Merge PFN Printout */
    if (verbose) {
        printf("\n=== POST-MERGE MAPPINGS ===\n");
        for (int i = 0; i < num_files; i++) 
            print_pfn_map(ioctl_fd, fds[i], argv[i + 2], pfns);
    }

    printf("\n");
    print_cache_usage("Final State");

    /* Cleanup */
    for (int i = 0; i < num_files; i++) close(fds[i]);
    close(ioctl_fd); close(sysfs_fd);
    free(fds); free(pfns); free(sysfs_buf);
    return 0;
}
