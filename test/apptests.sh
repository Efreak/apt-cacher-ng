#!/bin/bash
set -e
bash -x test/cmdline/apt-cacher-ng.sh
. test/include/start.sh $(mktemp -d) build/apt-cacher-ng
(cd test/soap && bash -x soaptest.sh)
finish_acng
