#!/bin/sh
(cat head.txt ; stat --printf 'Content-Length: %s' body.txt ; echo -ne '\r\n\r\n' ; cat body.txt )| socket localhost 3142
