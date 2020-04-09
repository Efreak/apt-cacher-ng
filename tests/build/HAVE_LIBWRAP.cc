#include <tcpd.h>
int main() 
{
   request_info req;
   request_init(&req, RQ_FILE, 0, 0); fromhost(&req);
   return !hosts_access(&req);
}

