#!/bin/bash
set -e
set -x
exe=builddir/apt-cacher-ng
tool="builddir/acngtool -c conf"
test -x $exe
test "$TMP" || TMP=$(mktemp -d)

# basic stuff not needing network activity

test "c2Fucy1zZXJpZg==" = $($tool encb64 sans-serif)

$tool retest http://srv/foo.deb
$tool retest http://srv/foo.deb | grep FILE_SOLID
$tool retest http://srv/InRelease | grep FILE_VOLAT
$tool retest http://srv/InRelease.html | grep NOMATCH
$tool retest /connectivity-check.html | grep FILE_VOLAT
$tool retest http://srv/InRelease.html "VfilePatternEx:.*html" | grep VOLATIL
$tool retest http://srv/InRelease.html "PfilePatternEx:.*html" | grep SOLID

$exe -h > $TMP/y.txt
grep -i usage $TMP/y.txt
grep -i help $TMP/y.txt
grep -i See $TMP/y.txt

valgrind $tool curl http://www.example.org > $TMP/dump
grep illustrative $TMP/dump

PORT=$(( 11222 + $RANDOM / 2 ))

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

#! REFSUM=/dev/nada12341254324534125ljksaldfkj2345 GETSUM=sans-serif $exe
#export REFSUM=$(sha1sum $TMP/dump | cut -c1-40)
#GETSUM=$TMP/dump $exe

# test expiration
mkdir -p $TMP/srv
touch $TMP/srv/foo.deb
$exe foreground=0 logdir=$TMP CacheDir:$TMP port:$PORT pidfile=$TMP/x.pid ExTreshold:0 -e
! test -e $TMP/srv/foo.deb

$tool cfgdump > $TMP/vars
$tool printvar useragent | grep Apt-Cacher-NG

$exe foreground=1 logdir=$TMP CacheDir:$TMP port:$PORT pidfile=$TMP/x.pid &
export xpid=$!
sleep 1
test $xpid -gt 0
kill -0 $xpid
trap "kill $xpid" 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
export http_proxy=http://localhost:$PORT

rm -rf $TMP
