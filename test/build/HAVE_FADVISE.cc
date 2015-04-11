#ifdef _XOPEN_SOURCE
#undef _XOPEN_SOURCE
#endif
#define _XOPEN_SOURCE 600
#include <fcntl.h>
int testme(int fd, off_t offset, off_t len, int) { return posix_fadvise(fd, offset, len, POSIX_FADV_SEQUENTIAL); }; int main(int,char**){return testme(0,0,0,0);}

