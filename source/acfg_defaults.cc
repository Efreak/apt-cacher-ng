
// some variable instances with default values 
// shared among different applications

#include "config.h"
#include "meta.h"
#include "acfg.h"

using namespace MYSTD;

namespace acfg
{

string cachedir("/var/tmp"), logdir("/var/tmp"), fifopath, pidfile, reportpage,
confdir, adminauth, bindaddr, mirrorsrcs, suppdir("/usr/lib/apt-cacher-ng"),
capath("/etc/ssl/certs"), cafile;

#define INFOLDER "(^|.*/)"
#define COMPRLIST "(\\.gz|\\.bz2|\\.lzma|\\.xz)"
#define ALXPATTERN ".*\\.(db|files|abs)(\\.tar" COMPRLIST ")?"
#define COMPOPT COMPRLIST"?"
//#define COMPONENT_OPTIONAL "(-[a-z0-9-])"
//#define PARANOIASOURCE "(\\.orig|\\.debian)"

string pfilepat(".*(\\.d?deb|\\.rpm|\\.drpm|\\.dsc|\\.tar" COMPRLIST "(\\.gpg)?"
		"|\\.diff" COMPRLIST "|\\.jigdo|\\.template|changelog|copyright"
		"|\\.udeb|\\.debdelta|\\.diff/.*\\.gz|(Devel)?ReleaseAnnouncement(\\?.*)?"
		"|[a-f0-9]+-(susedata|updateinfo|primary|deltainfo).xml.gz" //opensuse, index data, hash in filename
		"|fonts/(final/)?[a-z]+32.exe(\\?download.*)?" // msttcorefonts, fonts/final/comic32.exe /corefonts/comic32.exe plus SF's parameters
		"|/dists/.*/installer-[^/]+/[0-9][^/]+/images/.*" // d-i stuff with revision
")$");

string vfilepat(INFOLDER
		"(Index|Packages" COMPOPT "|InRelease|Release|Release\\.gpg|custom\\.gpg|mirrors.txt|"
		"Sources" COMPOPT "|release|index\\.db-.*\\.gz|Contents-[^/]*" COMPOPT
		"|pkglist[^/]*\\.bz2|rclist[^/]*\\.bz2|meta-release[^/]*|Translation[^/]*" COMPOPT
		"|MD5SUMS|SHA1SUMS" // d-i stuff
		"|((setup|setup-legacy)(\\.ini|\\.bz2|\\.hint)(\\.sig)?)|mirrors\\.lst" // cygwin
		"|repo(index|md)\\.xml(\\.asc|\\.key)?|directory\\.yast" // opensuse
		"|products|content(\\.asc|\\.key)?|media" // opensuse 2, are they important?
		"|filelists\\.xml\\.gz|filelists\\.sqlite\\.bz2|repomd\\.xml" // SL, http://ra.khe.sh/computers/linux/apt-cacher-ng-with-yum.html
		"|packages\\.[a-zA-Z][a-zA-Z]\\.gz|info\\.txt|license\\.tar\\.gz|license\\.zip" //opensuse
		"|" ALXPATTERN // Arch Linux
		"|metalink\\?repo|.*prestodelta\\.xml\\.gz|repodata/.*\\.(xml|sqlite)" COMPRLIST // CentOS
		")$" // end of only-filename paterns
		"|/dists/.*/installer-[^/]+/[^0-9][^/]+/images/.*"); // d-i stuff but not containing a number (year) in the revision directory (like "current", "beta", ...)

//string wfilepat( VPATPREFIX  "(Release|Release\\.gpg|release|meta-release|Translation[^/]*\\.bz2)$");
//string wfilepat(vfilepat);
string wfilepat(INFOLDER
		"(Release|InRelease|Release\\.gpg|custom\\.gpg"
		"|(Packages|Sources)" COMPRLIST "?"
		"|Translation[^/]*" COMPRLIST "?" // to be checked, but they should never really go anywhere
		"|MD5SUMS|SHA1SUMS"
		"|.*\\.xml" // SUSE
		"|" ALXPATTERN // Arch Linux
		"|[a-z]+32.exe"
		")$"
		"|/dists/.*/installer-.*/images/.*");


int offlinemode(false), verboselog(true), stupidfs(false), forcemanaged(false),
extreshhold(20), tpstandbymax(8), tpthreadmax(-1), dirperms(00755), fileperms(00664),
keepnver(0), maxtempdelay(27), vrangeops(1);

int dlbufsize(70000), exfailabort(1), exporigin(false), numcores(1),
logxff(false), oldupdate(false), recompbz2(false), nettimeout(60), updinterval(0),
forwardsoap(RESERVED_DEFVAL), usewrap(RESERVED_DEFVAL), redirmax(RESERVED_DEFVAL),
stucksecs(500), persistoutgoing(1), pipelinelen(255), exsupcount(RESERVED_DEFVAL),
optproxytimeout(-1);

#ifdef DEBUG
int dnscachetime(30);
#else
int dnscachetime(1800);
#endif

string agentname("Debian Apt-Cacher-NG/" ACVERSION);
string remoteport("80"), port(ACNG_DEF_PORT);
string agentheader;

string requestapx;

#ifdef DEBUG
int debug(3), foreground(true);
//string cachedir("/var/cache/acng"), logdir("/var/log/acng"), fifopath, pidfile;
#else
int debug(0), foreground(false);
#endif

tHttpUrl proxy_info;

string cacheDirSlash; // guaranteed to have a trailing path separator

int conprotos[2] = { PF_UNSPEC, PF_UNSPEC };

}
