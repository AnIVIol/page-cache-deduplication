#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define BUF_SIZE 4096

void parse_allowed(const char *input, int allowed[256]) {
    for (int i = 0; i < 256; i++) allowed[i] = 0;

    for (int i = 0; input[i]; i++) {
        if (input[i] == '\\') {
            i++;
            if (!input[i]) break;

            switch (input[i]) {
                case 'n': allowed['\n'] = 1; break;
                case 't': allowed['\t'] = 1; break;
                case 'r': allowed['\r'] = 1; break;
                case '\\': allowed['\\'] = 1; break;
                case '0': allowed['\0'] = 1; break;
                default:
                    allowed[(unsigned char)input[i]] = 1;
            }
        } else {
            allowed[(unsigned char)input[i]] = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <file> <allowed_chars>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    const char *allowed_str = argv[2];

    int allowed[256];
    parse_allowed(allowed_str, allowed);

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char buf[BUF_SIZE];
    ssize_t n;
    off_t offset = 0;

    while ((n = read(fd, buf, BUF_SIZE)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (!allowed[c]) {
                printf("Offset %lld: 0x%02x", (long long)(offset + i), c);

                if (c >= 32 && c <= 126)
                    printf(" ('%c')", c);

                printf("\n");
            }
        }
        offset += n;
    }

    if (n < 0) {
        perror("read");
    }

    close(fd);
    return 0;
}
