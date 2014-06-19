
DBTMP=tmp
CONFDIR=conf

# Main rule to be called to update all databases. Does two things, first get
# and pre-filter raw data, second: check the sources and generate final lists,
# skipping dead mirrors.
gendbs: $(CONFDIR) clean_db_tmp get_deb_input get_ubu_input get_fedora_input get_sl_input
	$(MAKE) $(CONFDIR)/deb_mirrors.gz $(CONFDIR)/debvol_mirrors.gz $(CONFDIR)/gentoo_mirrors.gz
	$(MAKE) $(CONFDIR)/ubuntu_mirrors $(CONFDIR)/cygwin_mirrors $(CONFDIR)/archlx_mirrors $(CONFDIR)/fedora_mirrors $(CONFDIR)/epel_mirrors $(CONFDIR)/sl_mirrors

clean_db_tmp:
	rm -rf "$(DBTMP)"

$(CONFDIR):
	mkdir -p $(CONFDIR)

$(DBTMP):
	mkdir -p $(DBTMP)

# the get_* targets are intended to fetch data, but only change the key file (prerequisite for other rules) if the data has really changed
# this is the first stage of the gendbs target. 
get_deb_input: $(DBTMP)
# they block checkout mode for some reason but we don't care
#	wget -q -O $(DBTMP)/dsnap 'http://anonscm.debian.org/viewvc/webwml/webwml/english/mirror/Mirrors.masterlist?view=co'
	wget -q -O- 'http://anonscm.debian.org/viewvc/webwml/webwml/english/mirror/Mirrors.masterlist?view=markup' | grep file.line.text | sed -e 's,.*>,,' > $(DBTMP)/dsnap
	md5sum $(DBTMP)/dsnap > $(DBTMP)/sig-debian
	cmp sig-debian $(DBTMP)/sig-debian 2>/dev/null || cp $(DBTMP)/sig-debian sig-debian

# some country-TLDed mirrors are not listed in the mirror list, adding them manually
get_ubu_input:
	( w3m -dump http://www.iana.org/domains/root/db | perl -pe 'if(/^\.(\w\w)\s/) { $$_="http://".lc($$1).".archive.ubuntu.com/ubuntu\n";} else {undef $$_}';  wget -q -O- 'https://wiki.ubuntu.com/Mirrors?action=show&redirect=Archive' 'https://launchpad.net/ubuntu/+archivemirrors' ) | tr -d ' ' | tr -d '\t' | sed -e 's,",\n,g' | grep ^http | sort -u > $(DBTMP)/usnap
	md5sum $(DBTMP)/usnap > $(DBTMP)/sig-ubuntu
	cmp sig-ubuntu $(DBTMP)/sig-ubuntu 2>/dev/null || cp $(DBTMP)/sig-ubuntu sig-ubuntu 

get_fedora_input:
	wget -O- https://mirrors.fedoraproject.org/publiclist/ | tr '"' '\n' | grep -iE '^http.*(linux|epel)/?$$' | sort -u > $(DBTMP)/fsnap
	md5sum $(DBTMP)/fsnap > $(DBTMP)/sig-fsnap
	cmp sig-fsnap $(DBTMP)/sig-fsnap 2>/dev/null || cp $(DBTMP)/sig-fsnap sig-fsnap

get_sl_input:
	wget -O- http://www.scientificlinux.org/download/mirrors | tr '"' '\n' | grep -iE '^http.*(scientific|linux|scientific-linux)/?$$' |sort -u > $(DBTMP)/slsnap
	# the mirrors list for SL does not contain the three main sites; so adding these manually
	printf "http://ftp.scientificlinux.org/linux/scientific/\nhttp://ftp1.scientificlinux.org/linux/scientific/\nhttp://ftp2.scientificlinux.org/linux/scientific/\n" >> $(DBTMP)/slsnap
	md5sum $(DBTMP)/slsnap > $(DBTMP)/sig-slsnap
	cmp sig-slsnap $(DBTMP)/sig-slsnap 2>/dev/null || cp $(DBTMP)/sig-slsnap sig-slsnap

# the $(CONFDIR)/* targets are intended to check the raw data got in the first step and 
# generate the final lists, skipping dead mirrors. this is the second stage of the gendbs target. 
$(CONFDIR)/epel_mirrors: sig-fsnap ubuntuscan.sh
	grep -iE 'epel/?$$' $(DBTMP)/fsnap | sort -u > $(DBTMP)/epelsnap
	bash ubuntuscan.sh $@ $(DBTMP)/epelsnap $(DBTMP) "/" "RPM-GPG-KEY-EPEL"

$(CONFDIR)/fedora_mirrors: sig-fsnap ubuntuscan.sh
	grep -iE 'linux/?$$' $(DBTMP)/fsnap | sort -u > $(DBTMP)/fcsnap
	bash ubuntuscan.sh $@ $(DBTMP)/fcsnap $(DBTMP) "/" "releases"

$(CONFDIR)/sl_mirrors: sig-slsnap ubuntuscan.sh
	bash ubuntuscan.sh $@ $(DBTMP)/slsnap $(DBTMP) "/" "5rolling\|6rolling"

$(CONFDIR)/ubuntu_mirrors: sig-ubuntu ubuntuscan.sh
	bash ubuntuscan.sh $@ $(DBTMP)/usnap $(DBTMP) "/pool/" "main"

$(CONFDIR)/cygwin_mirrors: sig-cygwin
	wget -q -O- http://cygwin.com/mirrors.lst | grep ^http | cut -f1 -d\; | sort -u > $(CONFDIR)/cygwin_mirrors

$(CONFDIR)/archlx_mirrors:
	wget -q --no-check-certificate  -O- 'https://archlinux.de/?page=MirrorStatus' | grep nofollow | cut -f2 -d'"'  | grep ^http | sort -u > $(CONFDIR)/archlx_mirrors

$(CONFDIR)/deb_mirrors.gz: sig-debian deburlgen.pl
	perl deburlgen.pl Archive-http < $(DBTMP)/dsnap > $(DBTMP)/dsnap.urls
	echo http://ftp.debian.org/debian/ >> $(DBTMP)/dsnap.urls
	echo http://ftp.debian.com/debian/ >> $(DBTMP)/dsnap.urls
	echo http://cdn.debian.net/debian/ >> $(DBTMP)/dsnap.urls
	echo http://http.debian.net/debian/ >> $(DBTMP)/dsnap.urls
# a way too experimental...	echo http://http.debian.net/debian/ >> $(DBTMP)/dsnap.urls
# there is no pool... echo http://ftp-master.debian.org/newdists/ >> $(DBTMP)/dsnap.urls
	bash ubuntuscan.sh $(CONFDIR)/deb_mirrors $(DBTMP)/dsnap.urls $(DBTMP) "/dists"
	gzip -f -9 $(CONFDIR)/deb_mirrors

$(CONFDIR)/debvol_mirrors.gz: sig-debian deburlgen.pl
	perl deburlgen.pl Volatile-http < $(DBTMP)/dsnap > $(DBTMP)/debvol.urls
	bash ubuntuscan.sh $(CONFDIR)/debvol_mirrors $(DBTMP)/debvol.urls $(DBTMP) "/dists"
	gzip -f -9 $(CONFDIR)/debvol_mirrors

$(CONFDIR)/gentoo_mirrors.gz:
	cd conf && sh ../gentoo_mirrors.sh

package:
	tar c $(DBTMP)/sig-* sig-* deburlgen.pl gentoo_mirrors.sh ubuntuscan.sh | gzip -9 > $(DBGENERATOR).tgz
