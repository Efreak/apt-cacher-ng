
// some variable instances with default values 
// shared among different applications

#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "sockio.h"

#include <atomic>

using namespace std;

namespace acng
{
namespace cfg
{

string ACNG_API cachedir(CACHEDIR), logdir(LOGDIR), udspath(UDSPATH), pidfile, reportpage,
confdir, adminauth, adminauthB64, bindaddr, mirrorsrcs, suppdir(LIBDIR),
capath("/etc/ssl/certs"), cafile, badredmime("text/html");

#define INFOLDER "(^|.*/)"
#define COMPRLIST "(\\.gz|\\.bz2|\\.lzma|\\.xz|\\.zst)"
#define ALXPATTERN ".*\\.(db|files|abs)(\\.tar" COMPRLIST ")?"
#define COMPOPT COMPRLIST"?"
#define HEX(len) "[a-f0-9]{" STRINGIFY(len) "}"
#define HEXSEQ "[a-f0-9]+"
#define PKGS "(\\.[ud]?deb|\\.rpm|\\.drpm|\\.dsc|\\.tar" COMPRLIST ")"
#define CSUM "(SHA|MD)[0-9]+"
#define CSUMS "(SHA|MD)[0-9]+SUM"

//#define COMPONENT_OPTIONAL "(-[a-z0-9-])"
//#define PARANOIASOURCE "(\\.orig|\\.debian)"

string spfilepat;

string pfilepat
("^.*("
	PKGS
	"|\\.diff" COMPRLIST "|\\.jigdo|\\.template|changelog|copyright"
	"|\\.debdelta|\\.diff/.*\\.gz"
	"|" HEXSEQ "-(susedata|updateinfo|primary|deltainfo).xml.gz" //opensuse, index data, hash in filename
	"|fonts/(final/)?[a-z]+32.exe(\\?download.*)?" // msttcorefonts, fonts/final/comic32.exe /corefonts/comic32.exe plus SF's parameters
	"|/dists/.*/installer-[^/]+/[0-9][^/]+/images/.*" // d-i stuff with revision
    "|/[[:alpha:]]{1,2}/" HEX(64) "(-" HEX(64) ")?(\\.gz)?" // FreeBSD, after https://alioth.debian.org/tracker/?func=detail&atid=413111&aid=315254&group_id=100566
    "|/by-hash/" CSUM "/.*" // support Debian/Ubuntu by-hash index files
    "|\\.asc$" // all remaining PGP signatures. Assuming that volatile ones are matched below.
    "|changelogs/pool/.*/changelog.txt$" // packages.ultimediaos.com
    "|/objects/.*/.*\\.(dirtree|filez|commit|commitmeta)|/repo/deltas/.*" // FlatPak
    // for Fedora 29 and 30 , https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=928270
	// XXX: add unit tests
    "|[a-f0-9]+-modules.yaml.gz|[a-f0-9]+-(primary|filelists|comps-[^.]*.[^.]*|updateinfo|prestodelta).xml(|.gz|.xz|.zck)"

	// cachable d-i directory listing with revision
	//"|/dists/.*/installer-[^/]+/[0-9][^/]+(/images)?/" // d-i stuff with revision

	")$");

string svfilepat("/development/rawhide/.*"
    // more stuff for ubuntu dist-upgrader
    "|dists/.*dist-upgrader.*/current/.*" // /dists/xenial/main/dist-upgrader-all/current/xenial.tar.gz
    "|changelogs.ubuntu.com/meta.*" // changelogs.ubuntu.com/meta-release-lts-development
    // XXX: signature might change afterwards... any better solution than this location?
    "|" INFOLDER ".*" PKGS "\\.gpg$"
    );

string vfilepat(INFOLDER
		"(Index|Packages" COMPOPT "|InRelease|Release|mirrors\\.txt|.*\\.gpg|NEWS\\.Debian"
		"|Sources" COMPOPT "|release|index\\.db-.*\\.gz|Contents-[^/]*" COMPOPT
		"|pkglist[^/]*\\.bz2|rclist[^/]*\\.bz2|meta-release[^/]*|Translation[^/]*" COMPOPT
		"|" CSUMS // d-i stuff
		"|((setup|setup-legacy)(\\.ini|\\.bz2|\\.hint)(\\.sig)?)|mirrors\\.lst" // cygwin
		"|repo(index|md)\\.xml(\\.asc|\\.key)?|directory\\.yast" // opensuse
		"|products|content(\\.asc|\\.key)?|media" // opensuse 2, are they important?
		"|filelists\\.xml\\.gz|filelists\\.sqlite\\.bz2|repomd\\.xml" // SL, http://ra.khe.sh/computers/linux/apt-cacher-ng-with-yum.html
		"|packages\\.[[:alpha:]]{2}\\.gz|info\\.txt|license\\.tar\\.gz|license\\.zip" //opensuse
		"|" ALXPATTERN // Arch Linux
		"|metalink\\?repo|.*prestodelta\\.xml\\.gz|repodata/.*\\.(yaml|yml|xml|sqlite)" COMPOPT // CentOS
		"|\\.treeinfo|vmlinuz|(initrd|product|squashfs|updates)\\.img" // Fedora
		"|\\.o" // https://bugs.launchpad.net/ubuntu/+source/apt-cacher-ng/+bug/1078224
		"|Components-.*yml" COMPOPT // DEP-11 aka AppStream
		"|icons-[x0-9]+\\.tar" COMPOPT
		"|CID-Index-[[:alnum:]]+\\.json" COMPOPT
		"|(latest|pub)\\.ssl" // FreeBSD
		")$" // end of filename-only patterns

		"|/dists/.*/installer-[^/]+/[^0-9][^/]+/images/.*" // d-i stuff but not containing a date (year number) in the revision directory (like "current", "beta", ...)
		"|/pks/lookup.op.get" // some Ubuntu PPA management activity
		"|centos/.*/images/.*img" // [#314924] Allow access to CentOS images

		"|connectivity-check.html|ubiquity/.*update|getubuntu/releasenotes" // Ubuntu installer network check, etc.
		"|wiki.ubuntu.com/.*/ReleaseNotes" // this is actually for an internal check and therefore contains the hostname
		"|ubuntu/dists/.*\\.html" // http://archive.ubuntu.com/ubuntu/dists/vivid-updates/main/dist-upgrader-all/current/ReleaseAnnouncement.html
    "|metadata.(ftp-master.debian|tanglu).org/changelogs/.*" // some of them are not static
    "|/refs/heads/app.*|/repo/(summary|config)(\\.sig)?|/Service/News$" // FlatPak
);

//string wfilepat( VPATPREFIX  "(Release|Release\\.gpg|release|meta-release|Translation[^/]*\\.bz2)$");
//string wfilepat(vfilepat);
string wfilepat(INFOLDER
		"(Release|InRelease|.*\\.gpg"
		"|(Packages|Sources)" COMPRLIST "?" // hm... private repos without Release file :-(
		"|.*\\.xml" // SUSE
		"|setup\\.bz2(.sig)?" // Cygwin
		"|" ALXPATTERN // Arch Linux
		"|[a-z]+32.exe"
		"|mirrors.ubuntu.com/mirrors.txt"
    "|/[[:alpha:]]{1,2}/" HEX(64) "(-" HEX(64) ")?(\\.gz)?" // FIXME: add expiration code
		")$");

string pfilepatEx, spfilepatEx, vfilepatEx, svfilepatEx, wfilepatEx; // for customization by user
int offlinemode(false), verboselog(true), stupidfs(false), forcemanaged(false),
extreshhold(20), tpstandbymax(8), tpthreadmax(-1), dirperms(00755), fileperms(00664),
keepnver(0), maxtempdelay(27), vrangeops(1), dlretriesmax(2);

int dlbufsize(30000), exfailabort(1), exporigin(false), numcores(1),
logxff(false), oldupdate(false), recompbz2(false), nettimeout(40), updinterval(0),
forwardsoap(RESERVED_DEFVAL), usewrap(RESERVED_DEFVAL), redirmax(RESERVED_DEFVAL),
stucksecs(500), persistoutgoing(1), pipelinelen(10), exsupcount(RESERVED_DEFVAL),
optproxytimeout(-1), patrace(false), maxredlsize(1<<16), nsafriendly(false),
trackfileuse(false), exstarttradeoff(500000000), fasttimeout(4), discotimeout(15);

int maxdlspeed(RESERVED_DEFVAL);

string optproxycmd;
int optproxycheckint=-1;

#ifdef DEBUG
int dnscachetime(30);
#else
int dnscachetime(1800);
#endif

string ACNG_API agentname("Apt-Cacher-NG/" ACVERSION);
string ACNG_API remoteport("80"), port(ACNG_DEF_PORT);
string ACNG_API agentheader;

string ACNG_API requestapx;
string sigbuscmd;
mstring connectPermPattern("~~~");

#ifdef DEBUG
int debug(3), foreground(true);
//string cachedir("/var/cache/acng"), logdir("/var/log/acng"), udspath, pidfile;
#else
int debug(0), foreground(false);
#endif

string ACNG_API cacheDirSlash; // guaranteed to have a trailing path separator

// If second == unspec -> first applies as filter, if first unspec -> not filtered
int conprotos[2] = { PF_UNSPEC, PF_UNSPEC };

std::atomic_bool degraded(false);

int allocspace = 1024*1024;

}

}
