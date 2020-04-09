#include <sys/sendfile.h>
int main(int argc, char **argv) { off_t yes(3); return (int) sendfile(1, 2, &yes, 4); }
