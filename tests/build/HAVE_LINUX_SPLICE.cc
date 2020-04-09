#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
int main() { off_t sin(12), sout(34); return splice(0, &sin, 1, &sout, 12, SPLICE_F_MORE); }

