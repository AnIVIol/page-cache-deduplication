/*
 * gen_files.c — multi-file generator with controllable redundancy.
 *
 * Same core logic as gen_dedup.c and generate.c in other test suites (shuffle / fill_random / page-group
 * marking). Adds:
 *   - num_files: how many files to write into <output_dir>
 *   - groups:    keep the groups feature from generate.c
 *
 * Usage: ./gen_files <num_files> <percentage> <groups> <size_MB> <output_dir>
 *
 * Example: ./gen_files 4 30 2 128 /tmp/work
 *   creates /tmp/work/file_0.bin .. file_3.bin, each 128 MB,
 *   30% of pages duplicated within each of 2 groups.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define PAGE_SIZE 4096

static void shuffle(int *arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static void fill_random(char *buf) {
    for (int i = 0; i < PAGE_SIZE; i++) {
        buf[i] = rand() % 256;
    }
}

static int generate_one(const char *path, int file_size_mb,
                        int percentage, int groups)
{
    if (percentage * groups > 100) {
        fprintf(stderr, "ERROR: percentage * groups must be <= 100\n");
        return 1;
    }

    size_t total_size = (size_t)file_size_mb * 1024 * 1024;
    int total_pages = total_size / PAGE_SIZE;
    int shared_per_group = (percentage * total_pages) / 100;

    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); return 1; }

    int *pages = malloc(total_pages * sizeof(int));
    for (int i = 0; i < total_pages; i++) pages[i] = i;
    shuffle(pages, total_pages);

    char **group_patterns = malloc(groups * sizeof(char *));
    for (int g = 0; g < groups; g++) {
        group_patterns[g] = malloc(PAGE_SIZE);
        fill_random(group_patterns[g]);
    }

    int *page_group = malloc(total_pages * sizeof(int));
    for (int i = 0; i < total_pages; i++) page_group[i] = -1;

    int idx = 0;
    for (int g = 0; g < groups; g++) {
        for (int j = 0; j < shared_per_group; j++) {
            if (idx >= total_pages) break;
            page_group[pages[idx++]] = g;
        }
    }

    char buffer[PAGE_SIZE];
    for (int i = 0; i < total_pages; i++) {
        if (page_group[i] == -1) {
            fill_random(buffer);
        } else {
            memcpy(buffer, group_patterns[page_group[i]], PAGE_SIZE);
        }
        fwrite(buffer, 1, PAGE_SIZE, fp);
    }

    fclose(fp);

    for (int g = 0; g < groups; g++) free(group_patterns[g]);
    free(group_patterns);
    free(page_group);
    free(pages);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 6) {
        printf("Usage: %s <num_files> <percentage> <groups> <size_MB> <output_dir>\n",
               argv[0]);
        return 1;
    }

    int num_files  = atoi(argv[1]);
    int percentage = atoi(argv[2]);
    int groups     = atoi(argv[3]);
    int size_mb    = atoi(argv[4]);
    const char *dir = argv[5];

    if (num_files <= 0 || percentage < 0 || groups <= 0 || size_mb <= 0) {
        fprintf(stderr, "ERROR: all numeric args must be positive\n");
        return 1;
    }

    /* Make sure output dir exists. */
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mkdir(dir, 0755) != 0) {
            perror("mkdir");
            return 1;
        }
    }

    srand(time(NULL));

    char path[1024];
    for (int i = 0; i < num_files; i++) {
        snprintf(path, sizeof(path), "%s/file_%d.bin", dir, i);
        if (generate_one(path, size_mb, percentage, groups) != 0)
            return 1;
    }

    printf("[+] Generated %d files in %s (size=%dMB, %d%% x %d groups)\n",
           num_files, dir, size_mb, percentage, groups);
    return 0;
}
