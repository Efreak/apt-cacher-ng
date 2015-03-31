#include <sys/mman.h>
int testme(void *addr, size_t length, int advice) { return posix_madvise(addr, length, advice); } int main(int,char**){return testme(0,0,0);}

