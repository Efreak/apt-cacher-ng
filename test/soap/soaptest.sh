#!/bin/bash
test "$http_proxy" || echo NOTE: proxy not set

apt-cache search abc > samplepkgs
cat >body.txt <<HERE 
<?xml version="1.0" encoding="utf-8" ?>
<env:Envelope xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
    xmlns:xsd="http://www.w3.org/2001/XMLSchema"
    xmlns:env="http://schemas.xmlsoap.org/soap/envelope/">
  <env:Body>
    <n1:get_bugs env:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"
        xmlns:n1="Debbugs/SOAP/">
      <keyvalue xmlns:n2="http://schemas.xmlsoap.org/soap/encoding/"
          n2:arrayType="xsd:anyType[4]"
          xsi:type="n2:Array">
        <item xsi:type="xsd:string">severity</item>
        <item n2:arrayType="xsd:anyType[3]"
            xsi:type="n2:Array">
          <item xsi:type="xsd:string">critical</item>
          <item xsi:type="xsd:string">grave</item>
          <item xsi:type="xsd:string">serious</item>
        </item>
        <item xsi:type="xsd:string">package</item>
        <item n2:arrayType="xsd:anyType[$(wc -l < samplepkgs)]"
		xsi:type="n2:Array">
HERE
sed -e 's,[[:space:]].*,</item>,;s,^,<item xsi:type="xsd:string">,' < samplepkgs >> body.txt
cat >> body.txt <<THERE
        </item>
      </keyvalue>
    </n1:get_bugs>
  </env:Body>
</env:Envelope>
THERE

set -e
(
( cat <<HEAD
POST http://bugs.debian.org:80/cgi-bin/soap.cgi HTTP/1.1
Content-Type: text/xml; charset=utf-8
SOAPAction: ""
User-Agent: SOAP4R/1.6.1-SNAPSHOT (2.3.3, ruby 2.1.5 (2014-11-13))
Accept: */*
Date: Fri, 10 Apr 2015 17:09:29 GMT
Host: bugs.debian.org
Connection: close
Content-Length: $(wc -c < body.txt)

HEAD
) | sed -e 's,$,\r,g'
cat body.txt
) | socket localhost 3142 > response.txt

echo Checking redirection response
grep HTTP.*302 response.txt || (echo Err, not redirection; exit 1)
grep "Location: https://bugs.debian.org:443/cgi-bin/soap.cgi" response.txt
./tryssl.sh
#grep 200.*OK response.txt || (echo Err, not 200 ; exit 1)
grep soap.Envelope response.txt
rm -f response.txt samplepkgs body.txt
