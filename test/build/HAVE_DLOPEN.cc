#include <dlfcn.h>
int main(int argc, char **argv)
{
  return dlsym(dlopen(*argv, RTLD_LAZY), *argv) ?0:1;
}
