#define _GNU_SOURCE
#include <linux/falloc.h>
#include <fcntl.h>
int main() { int fd=1; return fallocate(fd, FALLOC_FL_KEEP_SIZE, 1, 2); }

