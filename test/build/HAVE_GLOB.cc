
#include <glob.h>
int main(int argc, char **argv) { glob_t p; return glob(*argv, 0, 0, &p); }
