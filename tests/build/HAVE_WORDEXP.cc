
#include <wordexp.h>
int main(int argc, char **argv) { wordexp_t p; return wordexp(*argv, &p, 0); }
