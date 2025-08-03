#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define DEV_PATH "/dev/nvme0n1"
#define SECTOR_SIZE 4096
#define TOTAL_BYTES (23ULL * 1024 * 1024)
#define START_LBA 2

int main() {
    int fd = open(DEV_PATH, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    void *read_buf = NULL;
    if (posix_memalign(&read_buf, SECTOR_SIZE, TOTAL_BYTES)) {
        perror("posix_memalign");
        close(fd);
        return 1;
    }

    // 4096바이트 단위로 쓰인 데이터라고 가정
    char *expected = malloc(TOTAL_BYTES);
    for (int i = 0; i < TOTAL_BYTES; ++i)
        expected[i] = 65 + i % 23;

    off_t offset = START_LBA * SECTOR_SIZE;
    size_t total_read = 0;

    while (total_read < TOTAL_BYTES) {
        ssize_t r = pread(fd, read_buf, TOTAL_BYTES, offset);
        if (r <= 0) {
            perror("pread");
            break;
        }
        offset += r;
        total_read += r;
    }
    for(int i = 0; i < TOTAL_BYTES; ++i){
        if(expected[i] != ((char*)read_buf)[i]){
            fprintf(stderr, "Mismatch at offset %d\n", i);
            printf("%c != %c\n", expected[i],((char*)read_buf)[i] );
            if(i>524290) exit(1);
        }
    }
    // if (memcmp(read_buf, expected, TOTAL_BYTES) != 0) {
    //     fprintf(stderr, "Mismatch at offset %ld\n", offset);
    // } else {
    //     printf("✅ All data verified successfully.\n");
    // }

    // free(read_buf);
    close(fd);
    return 0;
}