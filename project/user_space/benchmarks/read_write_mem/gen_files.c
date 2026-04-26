#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define PAGE_SIZE 4096

void fill_random(char *buf) {
    for (int i = 0; i < PAGE_SIZE; i++) {
        buf[i] = rand() % 256;
    }
}
void shuffle(int *arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: %s <num_files> <shared_percent> <size_mb> <output_dir>\n", argv[0]);
        return 1;
    }

    int num_files = atoi(argv[1]);
    int percentage = atoi(argv[2]);
    int size_mb = atoi(argv[3]);
    char *out_dir = argv[4];

    size_t total_size = (size_t)size_mb * 1024 * 1024;
    int total_pages = total_size / PAGE_SIZE;
    int shared_pages = (percentage * total_pages) / 100;

    printf("Total pages: %d\n", total_pages);
    printf("Shared pages: %d\n", shared_pages);

    srand(time(NULL));

    mkdir(out_dir, 0755);

    // 🔥 ONE shared pattern for ALL shared pages
    char shared_buf[PAGE_SIZE];
    fill_random(shared_buf);

    char buffer[PAGE_SIZE];

    for (int f = 0; f < num_files; f++) {

        char path[512];
        snprintf(path, sizeof(path), "%s/file_%d.bin", out_dir, f);

        FILE *fp = fopen(path, "wb");
        if (!fp) {
            perror("fopen");
            return 1;
        }


        // create indices
	int *indices = malloc(total_pages * sizeof(int));
	for (int i = 0; i < total_pages; i++)
    		indices[i] = i;

	// shuffle using your function
	shuffle(indices, total_pages);

	// mark shared pages
	int *is_shared = calloc(total_pages, sizeof(int));
	for (int i = 0; i < shared_pages; i++)
    		is_shared[indices[i]] = 1;  
        
	
	for (int i = 0; i < total_pages; i++) {
            if (is_shared[i]) {
                // same content across:
                // - all shared pages
                // - all files
                memcpy(buffer, shared_buf, PAGE_SIZE);
            } else {
                fill_random(buffer);
            }

            fwrite(buffer, 1, PAGE_SIZE, fp);
        }

	free(indices);
	free(is_shared);
        fclose(fp);
        printf("[+] Generated %s\n", path);
    }

    return 0;
}
