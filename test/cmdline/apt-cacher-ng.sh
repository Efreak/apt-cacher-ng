#!/bin/bash
set -e
set -x
exe=build/apt-cacher-ng
test -x $exe
TMP=$(mktemp -d)
PORT=$(( 11222 + $RANDOM / 2 ))
$exe foreground=1 logdir=$TMP CacheDir=$TMP port:$PORT &
sleep 1
kill $!

if $exe badOption:1 logdir=$TMP CacheDir=$TMP port:$PORT ; then
	echo "Accepts unknown options"
	exit 1
fi


pidsBefore=$(pidof apt-cacher-ng)
$exe foreground=0 logdir=$TMP CacheDir:$TMP port:$PORT pidfile=$TMP/x.pid
sleep 1
pidsAfter=$(pidof apt-cacher-ng)
test $(echo $pidsAfter | wc -w) -eq $(( $(echo $pidsBefore | wc -w)  + 1 ))
for x in $pidsAfter ; do
	if echo $pidsBefore | grep $x ; then 
		:
	else
		test $(cat $TMP/x.pid) -eq $x
		kill $x
		break
	fi
done

$exe -h > $TMP/y.txt
grep -i usage $TMP/y.txt
grep -i help $TMP/y.txt
grep -i See $TMP/y.txt

BECURL=http://www.example.org $exe > $TMP/dump
grep illustrative $TMP/dump
! REFSUM=/dev/nada12341254324534125ljksaldfkj2345 GETSUM=sans-serif $exe
export REFSUM=$(sha1sum $TMP/dump | cut -c1-40)
GETSUM=$TMP/dump $exe

test "c2Fucy1zZXJpZg==" = $(TOBASE64=sans-serif $exe)

mkdir -p $TMP/srv
touch $TMP/srv/foo.deb
$exe foreground=0 logdir=$TMP CacheDir:$TMP port:$PORT pidfile=$TMP/x.pid ExTreshold:0 -e
! test -e $TMP/srv/foo.deb

$exe --retest http://srv/foo.deb
$exe --retest http://srv/foo.deb | grep FILE_SOLID
$exe --retest http://srv/InRelease | grep FILE_VOLAT
$exe --retest http://srv/InRelease.html | grep NOMATCH
$exe "VfilePatternEx:.*html" --retest http://srv/InRelease.html | grep VOLATIL
$exe "PfilePatternEx:.*html" --retest http://srv/InRelease.html | grep SOLID

$exe -p > $TMP/vars
PRINTCFGVAR=useragent $exe | grep Apt-Cacher-NG

rm -rf $TMP
