#define _GNU_SOURCE
#include <fcntl.h>
int main() { loff_t sin(12), sout(34); return splice(0, &sin, 1, &sout, 12, SPLICE_F_MORE); }

