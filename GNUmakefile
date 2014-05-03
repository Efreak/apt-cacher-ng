
ifneq ($(DEBUG),)
	 CXXFLAGS += -g -O0 -DDEBUG
	 CMAKEOPTS += -DADDDEBUGSRC=1 --debug-trycompile --debug-output
endif

ifneq ($(CXXFLAGS),)
CMAKEOPTS += "-DCMAKE_CXX_FLAGS=$(CXXFLAGS)"
else
CMAKEOPTS += "-DCMAKE_CXX_FLAGS=-g -O3 -Wall"
endif

ifneq ($(LDFLAGS),)
CMAKEOPTS += "-DCMAKE_EXE_LINKER_FLAGS=$(LDFLAGS)"
endif

DBTMP=dbgen/tmp

# pass down to cmake's Makefile, but all targets depend on config
default: all
apt-cacher-ng in.acng acngfs clean all: config
	@$(MAKE) -C build $@

config: build/.config-stamp

build/.config-stamp: build/.dir-stamp
	cd build && cmake .. $(CMAKEOPTS)
	@>$@

build/.dir-stamp:
	@test -d build || mkdir build
	@>$@

distclean:
	rm -rf build $(DBTMP) acsyscap.h *.o Makefile

doc: README
README: build/.dir-stamp doc/src/README.but doc/src/manpage.but doc/src/acngfs.but doc/src/textparm.but GNUmakefile
	@mkdir -p doc/html doc/man
	halibut --text=doc/README doc/src/textparm.but doc/src/README.but
	halibut --pdf=doc/apt-cacher-ng.pdf doc/src/README.but
	cd doc/html && halibut --html ../src/README.but
	cd doc/man && halibut --man ../src/manpage.but && halibut --man ../src/acngfs.but
	cp doc/README README

fixversion: VERSION
	mkdir -p build/tmp
	sed -e 's,^.*define.*ACVERSION.*,#define ACVERSION "$(VERSION)",' < include/config.h > build/tmp/hh
	cmp include/config.h build/tmp/hh || cp build/tmp/hh include/config.h

VERSION=$(shell cat VERSION)
DISTNAME=apt-cacher-ng-$(VERSION)
DEBSRCNAME=apt-cacher-ng_$(shell echo $(VERSION) | sed -e "s,pre,~pre,").orig.tar.xz

tarball: fixversion doc notdebianbranch nosametarball
	git diff-index --quiet HEAD || git commit -a
	git archive --prefix $(DISTNAME)/ HEAD | xz -9 > ../$(DISTNAME).tar.xz
#	cp -l tmp/$(DISTNAME)/doc/README tmp/$(DISTNAME)
	test -e /etc/debian_version && ln -f ../$(DISTNAME).tar.xz ../$(DEBSRCNAME) || true
	test -e ../tarballs && ln -f ../$(DISTNAME).tar.xz ../tarballs/$(DEBSRCNAME) || true
	test -e ../build-area && ln -f ../$(DISTNAME).tar.xz ../build-area/$(DEBSRCNAME) || true

tarball-remove:
	rm -f ../$(DISTNAME).tar.xz ../tarballs/$(DEBSRCNAME) ../$(DEBSRCNAME) ../build-area/$(DEBSRCNAME)

release: noremainingwork tarball
	git tag upstream/$(VERSION)

unrelease: tarball-remove
	git tag -d upstream/$(VERSION)

noremainingwork:
	test ! -e TODO.next # the quick reminder for the next release should be empty

notdebianbranch:
	test ! -f debian/rules # make sure it is not run from the wrong branch

nosametarball:
	test ! -f ../$(DISTNAME).tar.xz # make sure not to overwrite existing tarball
	test ! -f ../tarballs/$(DEBSRCNAME)

# Main rule to be called to update all databases. Does two things, first get
# and pre-filter raw data, second: check the sources and generate final lists,
# skipping dead mirrors.
gendbs: clean_db_tmp get_deb_input get_ubu_input get_fedora_input get_sl_input
	$(MAKE) conf/deb_mirrors.gz conf/debvol_mirrors.gz conf/gentoo_mirrors.gz
	$(MAKE) conf/ubuntu_mirrors conf/cygwin_mirrors conf/archlx_mirrors conf/fedora_mirrors conf/epel_mirrors conf/sl_mirrors

clean_db_tmp:
	rm -rf "$(DBTMP)"

# the get_* targets are intended to fetch data, but only change the key file (prerequisite for other rules) if the data has really changed
# this is the first stage of the gendbs target. 
get_deb_input:
	mkdir -p $(DBTMP)
# they block checkout mode for some reason but we don't care
#	wget -q -O $(DBTMP)/dsnap 'http://anonscm.debian.org/viewvc/webwml/webwml/english/mirror/Mirrors.masterlist?view=co'
	wget -q -O- 'http://anonscm.debian.org/viewvc/webwml/webwml/english/mirror/Mirrors.masterlist?view=markup' | grep file.line.text | sed -e 's,.*>,,' > $(DBTMP)/dsnap
	md5sum $(DBTMP)/dsnap > $(DBTMP)/sig-debian
	cmp dbgen/sig-debian $(DBTMP)/sig-debian 2>/dev/null || cp $(DBTMP)/sig-debian dbgen/sig-debian

# some country-TLDed mirrors are not listed in the mirror list, adding them manually
get_ubu_input:
	mkdir -p $(DBTMP)
	( w3m -dump http://www.iana.org/domains/root/db | perl -pe 'if(/^\.(\w\w)\s/) { $$_="http://".lc($$1).".archive.ubuntu.com/ubuntu\n";} else {undef $$_}';  wget -q -O- 'https://wiki.ubuntu.com/Mirrors?action=show&redirect=Archive' 'https://launchpad.net/ubuntu/+archivemirrors' ) | tr -d ' ' | tr -d '\t' | sed -e 's,",\n,g' | grep ^http | sort -u > $(DBTMP)/usnap
	md5sum $(DBTMP)/usnap > $(DBTMP)/sig-ubuntu
	cmp dbgen/sig-ubuntu $(DBTMP)/sig-ubuntu 2>/dev/null || cp $(DBTMP)/sig-ubuntu dbgen/sig-ubuntu 

get_fedora_input:
	mkdir -p $(DBTMP)
	wget -O- https://mirrors.fedoraproject.org/publiclist/ | tr '"' '\n' | grep -iE '^http.*(linux|epel)/?$$' | sort -u > $(DBTMP)/fsnap
	md5sum $(DBTMP)/fsnap > $(DBTMP)/sig-fsnap
	cmp dbgen/sig-fsnap $(DBTMP)/sig-fsnap 2>/dev/null || cp $(DBTMP)/sig-fsnap dbgen/sig-fsnap

get_sl_input:
	mkdir -p $(DBTMP)
	wget -O- http://www.scientificlinux.org/download/mirrors | tr '"' '\n' | grep -iE '^http.*(scientific|linux|scientific-linux)/?$$' |sort -u > $(DBTMP)/slsnap
	# the mirrors list for SL does not contain the three main sites; so adding these manually
	printf "http://ftp.scientificlinux.org/linux/scientific/\nhttp://ftp1.scientificlinux.org/linux/scientific/\nhttp://ftp2.scientificlinux.org/linux/scientific/\n" >> $(DBTMP)/slsnap
	md5sum $(DBTMP)/slsnap > $(DBTMP)/sig-slsnap
	cmp dbgen/sig-slsnap $(DBTMP)/sig-slsnap 2>/dev/null || cp $(DBTMP)/sig-slsnap dbgen/sig-slsnap

# the conf/* targets are intended to check the raw data got in the first step and 
# generate the final lists, skipping dead mirrors. this is the second stage of the gendbs target. 
conf/epel_mirrors: dbgen/sig-fsnap dbgen/ubuntuscan.sh
	grep -iE 'epel/?$$' $(DBTMP)/fsnap | sort -u > $(DBTMP)/epelsnap
	bash dbgen/ubuntuscan.sh $@ $(DBTMP)/epelsnap $(DBTMP) "/" "RPM-GPG-KEY-EPEL"

conf/fedora_mirrors: dbgen/sig-fsnap dbgen/ubuntuscan.sh
	grep -iE 'linux/?$$' $(DBTMP)/fsnap | sort -u > $(DBTMP)/fcsnap
	bash dbgen/ubuntuscan.sh $@ $(DBTMP)/fcsnap $(DBTMP) "/" "releases"

conf/sl_mirrors: dbgen/sig-slsnap dbgen/ubuntuscan.sh
	bash dbgen/ubuntuscan.sh $@ $(DBTMP)/slsnap $(DBTMP) "/" "5rolling\|6rolling"

conf/ubuntu_mirrors: dbgen/sig-ubuntu dbgen/ubuntuscan.sh
	bash dbgen/ubuntuscan.sh $@ $(DBTMP)/usnap $(DBTMP) "/pool/" "main"

conf/cygwin_mirrors: dbgen/sig-cygwin
	wget -q -O- http://cygwin.com/mirrors.lst | grep ^http | cut -f1 -d\; | sort -u > conf/cygwin_mirrors

conf/archlx_mirrors:
	wget -q --no-check-certificate  -O- 'https://archlinux.de/?page=MirrorStatus' | grep nofollow | cut -f2 -d'"'  | grep ^http | sort -u > conf/archlx_mirrors

conf/deb_mirrors.gz: dbgen/sig-debian dbgen/deburlgen.pl
	perl dbgen/deburlgen.pl Archive-http < $(DBTMP)/dsnap > $(DBTMP)/dsnap.urls
	echo http://ftp.debian.org/debian/ >> $(DBTMP)/dsnap.urls
	echo http://ftp.debian.com/debian/ >> $(DBTMP)/dsnap.urls
	echo http://cdn.debian.net/debian/ >> $(DBTMP)/dsnap.urls
	echo http://http.debian.net/debian/ >> $(DBTMP)/dsnap.urls
# a way too experimental...	echo http://http.debian.net/debian/ >> $(DBTMP)/dsnap.urls
# there is no pool... echo http://ftp-master.debian.org/newdists/ >> $(DBTMP)/dsnap.urls
	bash dbgen/ubuntuscan.sh conf/deb_mirrors $(DBTMP)/dsnap.urls $(DBTMP) "/dists"
	gzip -f -9 conf/deb_mirrors

conf/debvol_mirrors.gz: dbgen/sig-debian dbgen/deburlgen.pl
	perl dbgen/deburlgen.pl Volatile-http < $(DBTMP)/dsnap > $(DBTMP)/debvol.urls
	bash dbgen/ubuntuscan.sh conf/debvol_mirrors $(DBTMP)/debvol.urls $(DBTMP) "/dists"
	gzip -f -9 conf/debvol_mirrors

conf/gentoo_mirrors.gz:
	cd conf && sh ../dbgen/gentoo_mirrors.sh

doxy:
	doxygen Doxyfile
	see doc/dev/html/index.html

# execute them always and consider done afterwards
.PHONY: gendbs clean distclean config conf/gentoo_mirrors.gz

# the dependencies in THIS Makefile shall be processed as sequence
.NOTPARALLEL:
