#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define PAGE_SIZE 4096

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("Usage: %s <num_files> <num_pages>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    int num_pages = atoi(argv[2]);

    size_t file_size = (size_t)PAGE_SIZE * num_pages;

    char *buf = malloc(file_size);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    memset(buf, 'A', file_size);

    for (size_t i = 7; i < file_size; i += 8) {
        buf[i] = '\n';
    }

    for (int i = 0; i < n; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "new_file%d", i);

        int fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open");
            continue;
        }

        ssize_t written = write(fd, buf, file_size);
        if (written != file_size) {
            perror("write");
        }

        close(fd);
    }

    free(buf);

    printf("Created %d files of %d pages each (%zu bytes each).\n",
           n, num_pages, file_size);

    return 0;
}
