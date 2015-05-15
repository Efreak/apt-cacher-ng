
ifneq ($(DEBUG),)
	 CXXFLAGS += -g -O0 -DDEBUG
	 CMAKEOPTS += -DADDDEBUGSRC=1 --debug-trycompile --debug-output
endif

ifneq ($(NOLTO),)
	CMAKEOPTS += -DNOLTO=1
endif

export CXXFLAGS
export LDFLAGS

# pass down to cmake's Makefile, but all targets depend on config
default: all
apt-cacher-ng in.acng acngfs clean all: config
	@$(MAKE) -C build $@

config: build/.config-stamp

build/.config-stamp: build/.dir-stamp CMakeLists.txt
	cd build && cmake $(CMAKEOPTS) $(CURDIR)
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

PKGNAME=apt-cacher-ng
VERSION=$(shell cat VERSION)
TAGVERSION=$(subst rc,_rc,$(subst pre,_pre,$(VERSION)))
DISTNAME=$(PKGNAME)-$(VERSION)
DEBSRCNAME=$(PKGNAME)_$(shell echo $(VERSION) | sed -e "s,pre,~pre,;s,rc,~rc,;").orig.tar.xz

tarball: doc notdebianbranch nosametarball
	# diff-index is buggy and reports false positives... trying to work around
	git update-index --refresh || git commit -a
	git diff-index --quiet HEAD || git commit -a
	git archive --prefix $(DISTNAME)/ HEAD | xz -9 > ../$(DISTNAME).tar.xz
	test -e /etc/debian_version && ln -f ../$(DISTNAME).tar.xz ../$(DEBSRCNAME) || true
	test -e ../tarballs && ln -f ../$(DISTNAME).tar.xz ../tarballs/$(DEBSRCNAME) || true
	test -e ../build-area && ln -f ../$(DISTNAME).tar.xz ../build-area/$(DEBSRCNAME) || true

tarball-remove:
	rm -f ../$(DISTNAME).tar.xz ../tarballs/$(DEBSRCNAME) ../$(DEBSRCNAME) ../build-area/$(DEBSRCNAME)

release: test noremainingwork tarball
	git tag upstream/$(TAGVERSION)

unrelease: tarball-remove
	-git tag -d upstream/$(TAGVERSION)

pristine-commit:
	pristine-tar commit ../$(DISTNAME).tar.xz $(TAGVERSION)
	pristine-tar commit ../tarballs/$(DEBSRCNAME) $(TAGVERSION)

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

test:
	bash -x test/cmdline/apt-cacher-ng.sh
	cd test/misc && bash soaptest.sh

# execute them always and consider done afterwards
.PHONY: gendbs clean distclean config conf/gentoo_mirrors.gz test

# the dependencies in THIS Makefile shall be processed as sequence
.NOTPARALLEL:
