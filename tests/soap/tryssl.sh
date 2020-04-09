#!/bin/sh
set -x
: should work
wget -O response.txt --post-file=body.txt https://bugs.debian.org/cgi-bin/soap.cgi --header "Content-Type: text/xml; charset=utf-8"
: should fail
wget -O/dev/null --post-file=body.txt https://bugs.debian.org:80/cgi-bin/soap.cgi --header "Content-Type: text/xml; charset=utf-8" && exit 1 ||:

