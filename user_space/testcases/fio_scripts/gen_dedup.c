#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGE_SIZE 4096

void shuffle(int *arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

void fill_random(char *buf) {
    for (int i = 0; i < PAGE_SIZE; i++) {
        buf[i] = rand() % 256;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <file_size_MB> <percentage> <groups>\n", argv[0]);
        return 1;
    }

    int file_size_mb = atoi(argv[1]);
    int percentage = atoi(argv[2]);
    int groups = atoi(argv[3]);

    if (percentage * groups > 100) {
        printf("ERROR: percentage * groups must be <= 100\n");
        return 1;
    }

    size_t total_size = file_size_mb * 1024 * 1024;
    int total_pages = total_size / PAGE_SIZE;

    printf("Total pages: %d\n", total_pages);

    int shared_per_group = (percentage * total_pages) / 100;

    printf("Shared pages per group: %d\n", shared_per_group);

    FILE *fp = fopen("output.dat", "wb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    srand(time(NULL));

    // Page index array
    int *pages = malloc(total_pages * sizeof(int));
    for (int i = 0; i < total_pages; i++)
        pages[i] = i;

    shuffle(pages, total_pages);

    char **group_patterns = malloc(groups * sizeof(char *));
    for (int g = 0; g < groups; g++) {
        group_patterns[g] = malloc(PAGE_SIZE);
        fill_random(group_patterns[g]); // each group gets its own content
    }

    // Mark pages
    int *page_group = malloc(total_pages * sizeof(int));
    for (int i = 0; i < total_pages; i++)
        page_group[i] = -1;

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

    printf("[+] File generated: output.dat\n");

    // cleanup
    for (int g = 0; g < groups; g++)
        free(group_patterns[g]);
    free(group_patterns);
    free(page_group);
    free(pages);

    return 0;
}
