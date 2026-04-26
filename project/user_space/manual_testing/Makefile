CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGETS = generate_files write_offset write_test

all: $(TARGETS)

generate_files: generate_files.c
	$(CC) $(CFLAGS) -o generate_files generate_files.c

write_offset: write_offset.c
	$(CC) $(CFLAGS) -o write_offset write_offset.c

write_test: write_test.c
	$(CC) $(CFLAGS) -o write_test write_test.c

clean:
	rm -f $(TARGETS)
