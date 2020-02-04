#!/bin/bash

# little hack to open URLs and look for certain keywords there. Broken mirrors
# usually don't return anything or 404 pages, but when keywords appear there,
# they are expected to be working.

# works with subshell parallelization and reasonable timeout

set -x
set -a

if test "$1" = DO ; then
   x="$2"
   x=$(echo $x | sed -e 's,/\+,/,g ; s,http:/,http://, ; s,/$,,')
   apx=$BASHPID.$RANDOM$RANDOM
   export ufile=$tempdir/url.$apx
   export ffile=$tempdir/log.$apx
   echo "$x$sfx" > $ffile
   wget -q -t 2 -O- --timeout=23 "$x$sfx" 2>>$ffile | grep -q "$testkey" && echo $x > $ufile || echo FAILED >> $ffile
   exit 0
fi

listfile="$1"
src="$2"
tempdir=${3:-$(mktemp -d)}
sfx=${4:-/dists}
# Debian is easy, Ubuntu is bad... no symbolic name. Try to match well known release names
testkey=${5:-'updates\|stable\|unstable\|security\|backports\|feisty\|gutsy\|hardy\|intrepid\|jaunty\|karmic\|lucid\|maverick\|depper\|natty\|oneiric\|dapper\|edgy\|precise\|quantal\|raring\|saucy\|trusty\|vivid\|wily\|xenial\|yakkety\|zesty\|artful\|bionic\|cosmic\|disco'}

mkdir -p $tempdir
rm -f $tempdir/url.* $tempdir/log.*

test -s "$src" || exit 1

xargs -n1 -P 30 /bin/bash $0 DO < $src

cat $tempdir/url.* | sed -e 's,$,/,;s,//$,/,' | sort -u > $listfile

echo $listfile created.
