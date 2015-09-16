#!/bin/sh
mkdir -p builddir
src=$PWD
# honor user environments if set, otherwise use Release defaults
test -n "$CXXFLAGS" || CMAKEFLAGS=-DCMAKE_BUILD_TYPE=Release

# dev shortcut
if test "$1" = DEBUG
then
	shift
	CMAKEFLAGS="-DCMAKE_BUILD_TYPE=Debug --debug-trycompile --debug-output"
	MAKEFLAGS=VERBOSE=1
	export MAKEFLAGS
fi

cd builddir
if ! cmake $src $CMAKEFLAGS "$@"
then
	echo Configuration failed, please fix the reported issues and run ./distclean.sh
	exit 1
fi
PAR=-j$(nproc) || PAR=-j3
make $PAR
