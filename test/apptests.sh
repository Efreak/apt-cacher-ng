#!/bin/bash
set -e
xv=$(cat VERSION)
head -n1 ChangeLog
head -n1 ChangeLog | grep "$xv"

bash -x test/cmdline/apt-cacher-ng.sh
. test/include/start.sh $(mktemp -d) builddir/apt-cacher-ng
(cd test/soap && bash -x soaptest.sh)
finish_acng
