#include <sys/eventfd.h>
#include <stdint.h>
int main()
{
   uint64_t val;
   int fd=eventfd(0,0);
   return eventfd_write(fd, 1) + eventfd_read(fd, &val);
}
