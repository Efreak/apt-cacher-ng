#!/bin/bash
set -e
xv=$(cat VERSION)
head -n1 ChangeLog
head -n1 ChangeLog | grep "$xv"

bash -x test/cmdline/apt-cacher-ng.sh
. test/include/start.sh $(mktemp -d) builddir/apt-cacher-ng
(cd test/soap && bash -x soaptest.sh)
finish_acng

# build test with all profiles
rm -rf tmp/buildtest-*
for pro in Debug Release MinSizeRel ; do
	mkdir -p tmp/buildtest-$pro
	(cd tmp/buildtest-$pro && cmake ../.. -DCMAKE_BUILD_TYPE=$pro && make -j$(nproc))
done

