#include <pthread.h>
#include <errno.h>
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

void* stop_it(void *)
{
   pthread_mutex_lock(&mx);
   pthread_cond_broadcast(&cond);
   pthread_mutex_unlock(&mx);
   return NULL;
}

int main(void)
{
   // no problem with small type
   const struct timespec timeout = {9223372036854775807L, 12345};
   if(sizeof(timeout.tv_sec) < 8)
      return 0;
   for(int i=0;i<30;++i)
   {
      pthread_mutex_lock(&mx);
      pthread_t thr;
      if(pthread_create(&thr,0,stop_it,0))
         return -1;
      switch(pthread_cond_timedwait(&cond, &mx, &timeout))
      {
         case EINTR:
            continue; // loaded system, or killed by user?
         case 0:
            return 0; // correctly awaken = good
         default:
            return 1; // buggy implementation
      }
      pthread_mutex_unlock(&mx);
      pthread_join(thr, 0);
   }
   return -2;
}
