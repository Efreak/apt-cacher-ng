#!/bin/sh
#
# someone please do this right, it's a draft and mostly trial-n-error
#
# this can be set to alternative string for epel
FEDDEFAULT="repo.fedora-[234]..arch"
FEDSTRING="${FEDSTRING:-$FEDDEFAULT}"
if ! test -d "$DBTMP" ; then echo '$DBTMP not a directory, aborting' ; exit 1 ; fi
burl='https://mirrors.fedoraproject.org/mirrorlist?ip=0.0.0.0&'
wget -q "$burl""repo=rawhide&arch=invalid" -O "$DBTMP/repohints.txt"
rm -f "$DBTMP/meta.xml.txt"
cat "$DBTMP/repohints.txt" | grep "$FEDSTRING" | while read x y ; do
   #echo heh? "$x $y"
   case "$x---$y" in 
      "#---repo="*)
         echo "arch: $y"
         wget -q "$burl$y" -O- >> "$DBTMP/meta.xml.txt"
         ;;
   esac
done
#grep '<url.*http:' "$DBTMP/meta.xml.txt" | sed -e 's,.*>http:,http:,;s,\(development\|releases\).*,,' | sort -u  
grep 'http:' "$DBTMP/meta.xml.txt" | sort -u  

