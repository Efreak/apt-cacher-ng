#!/bin/bash

# little hack to open URLs and look for certain keywords there. Broken mirrors
# usually don't return anything or 404 pages, but when keywords appear there,
# they are expected to be working.

# works with subshell parallelization and reasonable timeout

set -x

listfile="$1"
src="$2"
tempdir=${3:-$(mktemp -d)}
sfx=${4:-/dists}
# Debian is easy, Ubuntu is bad... no symbolic name. Try to match well known release names
testkey=${5:-'updates\|stable\|unstable\|security\|backports\|feisty\|gutsy\|hardy\|intrepid\|jaunty\|karmic\|lucid\|maverick\|depper\|natty\|oneiric\|dapper\|edgy\|precise\|quantal\|raring\|saucy\|trusty'}

mkdir -p $tempdir
rm -f $tempdir/url.* $tempdir/log.*

test -s "$src" || exit 1

a=0
for x in `cat $src` ; do
   x=$(echo $x | sed -e 's,/\+,/,g ; s,http:/,http://, ; s,/$,,')
   a=$(( $a + 1 ))
   export ufile=$tempdir/url.$a
   export ffile=$tempdir/log.$a
   echo "$x$sfx" > $ffile
   ( if wget -q -t 1 -O- --timeout=30 "$x$sfx" 2>>$ffile | grep -q "$testkey" ;
then
   echo $x > $ufile
else
   echo FAILED >> $ffile
fi
) &
   sleep 0.2
done

wait

cat $tempdir/url.* | sed -e 's,$,/,;s,//$,/,' | sort -u > $listfile

echo $listfile created.
