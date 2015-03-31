#include <sys/eventfd.h>
int main()
{
   eventfd_t val;
   int fd=eventfd(0,0);
   return eventfd_write(fd, 1) + eventfd_read(fd, &val);
}
