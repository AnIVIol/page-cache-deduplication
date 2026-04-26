#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <file> <offset> <text>\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    off_t offset = atoll(argv[2]);
    char *text = argv[3];

    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        perror("lseek");
        close(fd);
        return 1;
    }

    ssize_t written = write(fd, text, strlen(text));
    if (written < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    printf("Wrote %ld bytes at offset %lld\n", written, (long long)offset);

    close(fd);
    return 0;
}
