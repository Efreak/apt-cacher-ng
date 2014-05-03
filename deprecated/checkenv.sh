#!/bin/sh -e

echo Checking build environment... >&2

if test -n "$1"; then
   CXX="$1"
   shift
fi

if test -z "$CXX"; then
   echo CXX not set as environment variable or first parameter
   exit 1
fi

CFGLOG=config.log
rm -f "$CFGLOG"
rm -f link.flags

if test -r .config.preset ; then
    . .config.preset
fi

runtest() {
	result="#define $1"
	shift
	echo Testing source: >> "$CFGLOG"
	cat checkenv.cc >> "$CFGLOG"
	echo Testing call:  "$CXX -c -o checkenv.o checkenv.cc $LF $@ 2>>$CFGLOG" >> "$CFGLOG"
	if $CXX -o checkenv_testbin checkenv.cc $LF "$@" 2>>$CFGLOG ; then
		echo "$result"
    echo "$LF" >> link.flags
	fi
  LF=""
}

rm -f checksuf.cc checksuf checksuf.exe

echo 'int main() {return 0;}' > checkenv.cc
"$CXX" -o checksuf checkenv.cc
EXEPRE='./'
if test -e checksuf.exe; then
   EXESUF=.exe
elif test -e checksuf ; then
   EXESUF=''
else
   echo "Compiler didn't create binary, cannot continue" >&2
fi

echo '#include <zlib.h>
z_stream t; int main(){ return inflateInit2(&t, 42); }
' > checkenv.cc
LF=-lz runtest HAVE_ZLIB "$@"

echo '
#include <bzlib.h>
bz_stream t; int main(){ return BZ2_bzDecompressInit(&t, 1, 0); }
' > checkenv.cc
LF=-lbz2 runtest HAVE_LIBBZ2 "$@"

echo '
#include <lzma.h>
lzma_stream st = LZMA_STREAM_INIT; 
int main(){return lzma_stream_decoder (&st, 32000, LZMA_TELL_UNSUPPORTED_CHECK | LZMA_CONCATENATED);}
' > checkenv.cc
LF=-llzma runtest HAVE_LZMA "$@"

echo '#include <tr1/memory>
int main() { return NULL != std::tr1::shared_ptr<int>(new int(1)); }
' > checkenv.cc
runtest HAVE_TR1_MEMORY "$@"

echo '#include <boost/smart_ptr.hpp>
int main() { return NULL != boost::shared_ptr<int>(new int(1)); }
' > checkenv.cc
runtest HAVE_BOOST_SMARTPTR "$@"

echo '#include <wordexp.h>
    int main(int argc, char **argv)
       {
          wordexp_t p;
          return wordexp(argv[0], &p, 0);

       }
' > checkenv.cc
runtest HAVE_WORDEXP "$@"

echo '#include <glob.h>
    int main(int argc, char **argv)
       {
          glob_t p;
          return glob(argv[0], 0, 0, &p);

       }
' > checkenv.cc
runtest HAVE_GLOB "$@"

echo '
       #define _XOPEN_SOURCE 600
       #include <fcntl.h>

       int testme(int fd, off_t offset, off_t len, int)
       {
          return posix_fadvise(fd, offset, len, POSIX_FADV_SEQUENTIAL);
       }
       int main(int,char**){return testme(0,0,0,0);}
' > checkenv.cc
runtest HAVE_FADVISE "$@"

echo '
#include <sys/mman.h>
int testme(void *addr, size_t length, int advice) {
   return posix_madvise(addr, length, advice);
}
int main(int,char**){return testme(0,0,0);}

' > checkenv.cc
runtest HAVE_MADVISE "$@"

echo '
#define _GNU_SOURCE
#include <linux/falloc.h>
#include <fcntl.h>
int main()
{
   int fd=1;
   return fallocate(fd, FALLOC_FL_KEEP_SIZE, 1, 2);
}
' > checkenv.cc
runtest HAVE_LINUX_FALLOCATE "$@"

echo '#include <sys/sendfile.h>
int main(int argc, char **argv)
{
   off_t yes(3);
   return (int) sendfile(1, 2, &yes, 4);
}
' > checkenv.cc
runtest HAVE_LINUX_SENDFILE "$@"

if test "$WORDS_BIGENDIAN" ; then
    echo '#define WORDS_BIGENDIAN  // preset by user'
elif test "$WORDS_LITTLEENDIAN" ; then
    echo '#define WORDS_LITTLEENDIAN  // preset by user'
else
    echo '#include <stdio.h>
    int main()
    {
        union { long l; char c[sizeof (long)]; } u;
        u.l = 1;
        printf("#define WORDS_%sENDIAN", 
        (u.c[sizeof (long) - 1] != 1) // return 0 on big endian
        ? "LITTLE" : "BIG" );
        return 0;
    }
    ' > checkenv.cc
    echo Testing call:  "$CXX -o testendian checkenv.cc $@ 2>>$CFGLOG" >> "$CFGLOG"
    $CXX -o testendian checkenv.cc "$@" 2>>$CFGLOG
    ${EXEPRE}testendian${EXESUF}
    echo
fi

if test "$SIZEOF_LONG" ; then
    echo "#define SIZEOF_LONG $SIZEOF_LONG"
else
    echo '
    #include <stdio.h>
    int main()
    {
        printf("#define SIZEOF_LONG %u", sizeof(long));
        return 0;
    }
    ' > checkenv.cc
    echo Testing call:  "$CXX -o getlongsize checkenv.cc $@ 2>>$CFGLOG" >> "$CFGLOG"
    rm -f getlongsize
    rm -f getlongsize.exe
    $CXX -o getlongsize checkenv.cc "$@" 2>>$CFGLOG
    ${EXEPRE}getlongsize${EXESUF}
    echo
fi


if test "$SIZEOF_INT" ; then
    echo "#define SIZEOF_INT $SIZEOF_INT"
else
    echo '
    #include <stdio.h>
    int main()
    {
        printf("#define SIZEOF_INT %u", sizeof(int));
        return 0;
    }
    ' > checkenv.cc
    echo Testing call:  "$CXX -o getlongsize checkenv.cc $@ 2>>$CFGLOG" >> "$CFGLOG"
    rm -f getlongsize
    rm -f getlongsize.exe
    $CXX -o getlongsize checkenv.cc "$@" 2>>$CFGLOG
    ${EXEPRE}getlongsize${EXESUF}
    echo
fi


exit 0
