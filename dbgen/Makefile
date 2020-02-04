# Mirror list/database update instructions
# This Make hackery runs either directly on web input or uses multiple steps in
# case where the input data is uncertain and the validity needs to be
# double-checked (by probing/crawling). To avoid pointless scans, the data is
# fingerprinted and the fingerprints are stored in the repository.
#
# XXX: this actually became too wild and most code should be moved into regular shell scripts.
#

DBTMP=tmp
export DBTMP
CONFDIR=conf
export CONFDIR
DEBMASTERLIST=https://salsa.debian.org/mirror-team/masterlist/raw/master/Mirrors.masterlist
ARCHJSON=https://www.archlinux.de/mirrors/datatables


# Main rule to be called to update all databases. Does two things, first get
# and pre-filter raw data, second: check the sources and generate final lists,
# skipping dead mirrors.
gendbs: $(CONFDIR) clean_db_tmp get_deb_input get_ubu_input get_fedora_input get_sl_input get_sfnet_input build_gentoo_mirrors
	$(MAKE) $(CONFDIR)/deb_mirrors.gz
	$(MAKE) $(CONFDIR)/ubuntu_mirrors $(CONFDIR)/cygwin_mirrors $(CONFDIR)/archlx_mirrors $(CONFDIR)/fedora_mirrors $(CONFDIR)/epel_mirrors $(CONFDIR)/sl_mirrors $(CONFDIR)/sfnet_mirrors

clean_db_tmp:
	rm -rf "$(DBTMP)"

$(CONFDIR):
	mkdir -p "$(CONFDIR)"

$(DBTMP):
	mkdir -p "$(DBTMP)"

# the get_* targets are intended to fetch data, but only change the key file (prerequisite for other rules) if the data has really changed
# this is the first stage of the gendbs target. 
get_deb_input: $(DBTMP)
	wget -q -O $(DBTMP)/dsnap '$(DEBMASTERLIST)'
	#grep file.line.text $(DBTMP)/dsnap_raw | sed -e 's,.*>,,' > $(DBTMP)/dsnap
	md5sum $(DBTMP)/dsnap > $(DBTMP)/sig-debian
	cmp sig-debian $(DBTMP)/sig-debian 2>/dev/null || cp $(DBTMP)/sig-debian sig-debian

# some country-TLDed mirrors are not listed in the mirror list, adding them manually
get_ubu_input:
	w3m -dump http://www.iana.org/domains/root/db > $(DBTMP)/aaa
	perl -pe 'if(/^\.(\w\w)\s/) { $$_="http://".lc($$1).".archive.ubuntu.com/ubuntu\n";} else {undef $$_}' < $(DBTMP)/aaa > $(DBTMP)/bbb
	wget -q -O $(DBTMP)/ccc 'https://wiki.ubuntu.com/Mirrors?action=show&redirect=Archive'
	wget -q -O $(DBTMP)/ccc2 'https://launchpad.net/ubuntu/+archivemirrors'
	cat $(DBTMP)/bbb $(DBTMP)/ccc $(DBTMP)/ccc2 | tr -d ' ' | tr -d '\t' | sed -e 's,",\n,g' | grep ^http: | sort -u > $(DBTMP)/usnap
	md5sum $(DBTMP)/usnap > $(DBTMP)/sig-ubuntu
	cmp sig-ubuntu $(DBTMP)/sig-ubuntu 2>/dev/null || cp $(DBTMP)/sig-ubuntu sig-ubuntu 

get_fedora_input:
	wget -q -O $(DBTMP)/fsnap_raw https://admin.fedoraproject.org/mirrormanager/mirrors/EPEL/7/x86_64
	tr '"' '\n' < $(DBTMP)/fsnap_raw | grep -iE '^http.*(linux|epel)/?$$' > $(DBTMP)/fsnap_
	bash fedorasrcs.sh >> $(DBTMP)/fsnap_
	cat $(DBTMP)/fsnap_ extra/fedora_mirrors $(CONFDIR)/fedora_mirrors | sort -u > $(DBTMP)/fsnap
	md5sum $(DBTMP)/fsnap > $(DBTMP)/sig-fsnap
	cmp sig-fsnap $(DBTMP)/sig-fsnap 2>/dev/null || cp $(DBTMP)/sig-fsnap sig-fsnap

get_sl_input:
	wget -q -O $(DBTMP)/sl_raw http://www.scientificlinux.org/downloads/sl-mirrors/
	cat extra/sl_mirrors $(DBTMP)/sl_raw $(CONFDIR)/sl_mirrors | tr '"' '\n' | grep -iE '^http.*(scientific|linux|scientific-linux)/?$$' | sort -u > $(DBTMP)/slsnap
	md5sum $(DBTMP)/slsnap > $(DBTMP)/sig-slsnap
	cmp sig-slsnap $(DBTMP)/sig-slsnap 2>/dev/null || cp $(DBTMP)/sig-slsnap sig-slsnap

get_sfnet_input: $(CONFDIR)/sfnet_mirrors

$(CONFDIR)/sfnet_mirrors: $(CONFDIR)
	wget -q -O $(DBTMP)/sf_raw http://sourceforge.net/p/forge/documentation/Mirrors/
	perl -pe 's,<td,\n<td,g' < $(DBTMP)/sf_raw | sort -u | grep '<td>[a-z]\+<' | sed -e 's,<td>,http://,;s,<.*,.dl.sourceforge.net/project/,' > $(CONFDIR)/sfnet_mirrors

# the $(CONFDIR)/* targets are intended to check the raw data got in the first step and 
# generate the final lists, skipping dead mirrors. this is the second stage of the gendbs target. 
$(CONFDIR)/epel_mirrors: sig-fsnap ubuntuscan.sh
	grep -iE 'epel/?$$' $(DBTMP)/fsnap | sort -u > $(DBTMP)/epelsnap
	bash ubuntuscan.sh $@ $(DBTMP)/epelsnap $(DBTMP) "/" "RPM-GPG-KEY-EPEL"

$(CONFDIR)/fedora_mirrors: sig-fsnap ubuntuscan.sh
	grep -v epel/ $(DBTMP)/fsnap | grep -v releases | sort -u > $(DBTMP)/fcsnap
	bash ubuntuscan.sh $@ $(DBTMP)/fcsnap $(DBTMP) "/" "releases"

$(CONFDIR)/sl_mirrors: sig-slsnap ubuntuscan.sh
	bash ubuntuscan.sh $@ $(DBTMP)/slsnap $(DBTMP) "/" "5rolling\|6rolling\|7rolling\|8rolling\|9rolling"

$(CONFDIR)/ubuntu_mirrors: sig-ubuntu ubuntuscan.sh
	bash ubuntuscan.sh $@ $(DBTMP)/usnap $(DBTMP) "/pool/" "main"

$(CONFDIR)/cygwin_mirrors:
	wget -q -O $(DBTMP)/cyg_raw http://cygwin.com/mirrors.lst
	grep ^http: $(DBTMP)/cyg_raw | cut -f1 -d\; | sort -u > $(CONFDIR)/cygwin_mirrors

$(CONFDIR)/archlx_mirrors:
	wget -q -O $(DBTMP)/arch_raw '$(ARCHJSON)'
#	jq '.data[]' $(DBTMP)/arch_raw | jq .url -r | sort -u > $(CONFDIR)/archlx_mirrors
	jq '.data[]' $(DBTMP)/arch_raw | jq .url -r | sed -e 's,https://,http://,' > $(DBTMP)/archlx_mirrors
	wget -q -O- https://www.archlinux.org/mirrorlist/all/http/ \
      | grep 'http://' \
      | sed -e 's/.* = //' -e 's/\$$repo.*//' >> $(DBTMP)/archlx_mirrors
	sort -u < $(DBTMP)/archlx_mirrors > $(CONFDIR)/archlx_mirrors

$(CONFDIR)/deb_mirrors.gz: sig-debian deburlgen.pl
	perl deburlgen.pl Archive-http < $(DBTMP)/dsnap > $(DBTMP)/dsnap.urls
	cat extra/deb_mirrors >> $(DBTMP)/dsnap.urls
	-zcat $@ >> $(DBTMP)/dsnap.urls
	sort -u < $(DBTMP)/dsnap.urls > $(DBTMP)/dsnap.urls_sorted
	bash ubuntuscan.sh $(CONFDIR)/deb_mirrors $(DBTMP)/dsnap.urls_sorted $(DBTMP) "/dists"
	gzip -nf9 $(CONFDIR)/deb_mirrors

build_gentoo_mirrors:
	cd $(CONFDIR) && sh $(shell dirname $(abspath $(lastword $(MAKEFILE_LIST))))/gentoo_mirrors.sh

TGTDIR=dbupdate

# some helpers to have reproducible builds where possible
TARARGS =--owner=root --group=root --mode=644 --no-xattrs
ifeq ($(SOURCE_DATE_EPOCH),)
TARARGS += --mtime=@1
else
TARARGS += --mtime=@$(SOURCE_DATE_EPOCH)
endif
ifneq ($(shell LANG=C tar --help | grep clamp-mtime),)
TARARGS += --clamp-mtime
endif
ifneq ($(shell LANG=C tar --help | grep "sort.*name"),)
TARARGS += --sort=name
endif

package: $(DBTMP)
	rm -rf $(TGTDIR) && mkdir $(TGTDIR) && cp -l Makefile deburlgen.pl *.sh sig-* $(TGTDIR) && tar -f- -c $(TARARGS) $(TGTDIR) | xz -9 > $(DBGENERATOR).tar.xz

install:
	cp conf/*mirrors* /usr/lib/apt-cacher-ng/

.PHONY: $(CONFDIR)/gentoo_mirrors.gz $(CONFDIR)/archlx_mirrors $(CONFDIR)/sfnet_mirrors
