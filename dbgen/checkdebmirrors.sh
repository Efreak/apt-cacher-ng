#!/bin/bash -x

listfile=goodmirrors.txt
badfile=badmirrors.txt
wd=$(mktemp -d)

src=$wd/src
./apt-cacher-ng -c conf logdir=/tmp debug=-42 | grep __debrep | cut -f1 -d_ > $src

a=0
for x in `cat $src` ; do
   x=$(echo $x | sed -e 's,/\+,/,g ; s,http:/,http://, ; s,/$,,')
   a=$(( $a + 1 ))
   wget -q -t 1 -O- --timeout=10 $x/dists/ | grep -q 'stable' && echo $x > $wd/url.$a || echo $x > $wd/bad.$a &
   sleep 0.2
done

wait

cat $wd/url.* | sed -e 's,$,/,;s,//$,/,' | sort -u > $listfile
cat $wd/bad.* | sed -e 's,$,/,;s,//$,/,' | sort -u > $badfile

echo $listfile created.
