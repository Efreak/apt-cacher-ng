#!/bin/sh
# This fetchs the live Gentoo mirrors list
# robbat2@gentoo.org - 2013/Dec/03
OUTFILE=gentoo_mirrors
wget --save-headers -q http://www.gentoo.org/main/en/mirrors3.xml -O - \
	| ( sed -n \
	-e '/^[A-Z]/{s,^,#,g;p}' \
	-e '/<mirrorgroup/{s,^,\n#,g;p}' \
	-e '/<name/{s,^,#,g;p}' \
	-e '/<uri/{/protocol="http"/{s/.*<uri[^>]\+>//g;s/<\/uri>//g;p}}' \
  ; echo -e "\n# Default GeoDNS routed access\nhttp://distfiles.gentoo.org/" ; \
  ) | gzip -9 >${OUTFILE}.gz
