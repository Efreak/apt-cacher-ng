
ifneq ($(DEBUG),)
	 CXXFLAGS += -g -O0 -DDEBUG
	 CMAKEOPTS += -DADDDEBUGSRC=1 --debug-trycompile --debug-output
endif

export CXXFLAGS
export LDFLAGS

# pass down to cmake's Makefile, but all targets depend on config
default: all
apt-cacher-ng in.acng acngfs clean all: config
	@$(MAKE) -C build $@

config: build/.config-stamp

build/.config-stamp: build/.dir-stamp
	cd build && cmake $(CMAKEOPTS) ..
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

gendbs:
	$(MAKE) -C dbgen CONFDIR=../conf

doxy:
	doxygen Doxyfile
	see doc/dev/html/index.html

# execute them always and consider done afterwards
.PHONY: gendbs clean distclean config conf/gentoo_mirrors.gz

# the dependencies in THIS Makefile shall be processed as sequence
.NOTPARALLEL:
