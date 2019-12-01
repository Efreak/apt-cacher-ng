#!/bin/sh
mkdir -p builddir
src=$PWD
# honor user environments if set, otherwise use Release defaults
test -n "$CXXFLAGS" || CMAKEFLAGS=-DCMAKE_BUILD_TYPE=Release

# dev shortcuts
while : ; do
   case "$1" in
      DEBUG)
         shift
         CMAKEFLAGS="$CMAKEFLAGS -DCMAKE_BUILD_TYPE=Debug -DDEBUG=ON -DCMAKE_VERBOSE_MAKEFILE=ON --debug-trycompile --debug-output"
         ;;
      VERBOSE)
         shift
         CMAKEFLAGS="$CMAKEFLAGS -DCMAKE_VERBOSE_MAKEFILE=ON"
         ;;
      *)
         break 2;
         ;;
   esac
done

cd builddir
if ! cmake $src $CMAKEFLAGS "$@"
then
	echo Configuration failed, please fix the reported issues and run ./distclean.sh
	exit 1
fi
PAR=-j$(nproc 2>/dev/null) || PAR=-j3
make $PAR
