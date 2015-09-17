#!/bin/sh
mkdir -p builddir
src=$PWD
# honor user environments if set, otherwise use Release defaults
test -n "$CXXFLAGS" || CMAKEFLAGS=-DCMAKE_BUILD_TYPE=Release

# dev shortcut
if test "$1" = DEBUG
then
	shift
	CMAKEFLAGS="-DCMAKE_BUILD_TYPE=Debug -DCMAKE_VERBOSE_MAKEFILE=ON --debug-trycompile --debug-output"
fi

cd builddir
if ! cmake $src $CMAKEFLAGS "$@"
then
	echo Configuration failed, please fix the reported issues and run ./distclean.sh
	exit 1
fi
PAR=-j$(nproc) || PAR=-j3
make $PAR
