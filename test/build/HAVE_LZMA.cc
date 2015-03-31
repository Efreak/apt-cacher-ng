#include <lzma.h>
lzma_stream t;
int main()
{
   return lzma_stream_decoder (&t, 32000000, LZMA_TELL_UNSUPPORTED_CHECK | LZMA_CONCATENATED);
}
