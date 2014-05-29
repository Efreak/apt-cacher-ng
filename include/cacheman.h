#ifndef _CACHEMAN_H_
#define _CACHEMAN_H_

#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "dirwalk.h"
#include "maintenance.h"
#include "lockable.h"
#include "csmapping.h"
#include "bgtask.h"
#include "fileitem.h"
#include <set>

// #define USEDUPEFILTER

class dlcon;
class tDlJobHints;
struct foo;

#define FAKEDATEMARK "Sat, 26 Apr 1986 01:23:39 GMT+3"

static cmstring sIndex("Index");
static cmstring sslIndex("/Index");

struct tPatchEntry
{
	string patchName;
	tFingerprint fprState, fprPatch;
};
typedef deque<tPatchEntry>::const_iterator tPListConstIt;

typedef MYSTD::pair<tFingerprint,mstring> tContId;
struct tClassDesc {tStrDeq paths; tContId diffIdxId, bz2VersContId;};
typedef MYMAP<tContId, tClassDesc> tContId2eqClass;
typedef MYMAP<tContId, tClassDesc>::iterator tClassMapIter;

void DelTree(const string &what);


class tCacheOperation :
	public IFileHandler,
	public tSpecOpDetachable
{

public:
	tCacheOperation(int,tSpecialRequest::eMaintWorkType);
	virtual ~tCacheOperation();

	// having this here makes no sense except of working around an ICE bug of gcc 4.3/4.4.x
	class tGetItemDelegate
	{
		public:
			virtual fileItemMgmt GetFileItemPtr()=0;
	};

protected:
	enum enumIndexType
	{
		EIDX_UNSUPPORTED =0,
		EIDX_RELEASE,
		EIDX_PACKAGES,
		EIDX_SOURCES,
		EIDX_DIFFIDX,
		EIDX_ARCHLXDB,
		EIDX_CYGSETUP,
		EIDX_SUSEREPO,
		EIDX_XMLRPMLIST,
		EIDX_RFC822WITHLISTS,
		EIDX_TRANSIDX,
		EIDX_MD5DILIST
	};
	struct tIfileAttribs
	{
		bool vfile_ondisk:1, uptodate:1, parseignore:1, hideDlErrors:1, forgiveDlErrors:1,
		alreadyparsed:1;
		enumIndexType eIdxType:8;
		const tStrDeq *bros;
		off_t space;
		inline tIfileAttribs() : vfile_ondisk(false), uptodate(false), parseignore(false),
				hideDlErrors(false), forgiveDlErrors(false), alreadyparsed(false),
				eIdxType(EIDX_UNSUPPORTED), bros(NULL), space(0)
		{};
	};

	typedef MYMAP<mstring,tIfileAttribs> tS2IDX;
	tS2IDX m_indexFilesRel;
	// helpers to keep the code cleaner and more readable
	const tIfileAttribs &GetFlags(cmstring &sPathRel) const;
	tIfileAttribs &SetFlags(cmstring &sPathRel);

	void SetCommonUserFlags(cmstring &cmd);

	void UpdateIndexFiles();
	void _BusyDisplayLogs();
	void _Usermsg(mstring m);
	bool AddIFileCandidate(const mstring &sFileRel);

	// NOOP, implemented here for convinience
	bool ProcessOthers(const mstring &sPath, const struct stat &);
	bool ProcessDirAfter(const mstring &sPath, const struct stat &);

	/*!
	 * As the name saids, processes all index files and calls a callback
	 * function maintenence::_HandlePkgEntry on each entry.
	 * 
	 * If a string set object is passed then a little optimization might be 
	 * enabled internally, which avoid repeated processing of a file when another
	 * one with the same contents was already processed. This is only applicable 
	 * having strict path checking disabled, though.
	 *   
	 * */

	void ProcessSeenIndexFiles(ifileprocessor &pkgHandler);

	void StartDlder();

#define VERB_QUIET 0
#define VERB_SHOW 1
#define VERB_SHOW_NOERRORS 2
	bool Download(const MYSTD::string & sFilePathRel, bool bIndexFile,
			int nVerbosity=VERB_SHOW, tGetItemDelegate *p=NULL, const char *pForcedURL=NULL);

	// internal helper variables
	bool m_bErrAbort, m_bVerbose, m_bForceDownload;
	bool m_bScanInternals, m_bByPath, m_bByChecksum, m_bSkipHeaderChecks;
	bool m_bTruncateDamaged;
	int m_nErrorCount;

	enumIndexType GuessIndexTypeFromURL(const mstring &sPath);

	unsigned int m_nProgIdx, m_nProgTell;

	void TellCount(uint nCount, off_t nSize);

	bool ParseAndProcessIndexFile(ifileprocessor &output_receiver,
			const mstring &sPath, enumIndexType idxType);

	MYMAP<mstring,bool> m_forceKeepInTrash;

	bool GetAndCheckHead(cmstring & sHeadfile, cmstring &sFilePathRel, off_t nWantedSize);
	bool Inject(cmstring &fromRel, cmstring &toRel,
			bool bSetIfileFlags=true, const header *pForcedHeader=NULL, bool bTryLink=false);

	void PrintStats(cmstring &title);
	mstring m_processedIfile;

	inline void ProgTell()
	{
		if (++m_nProgIdx == m_nProgTell)
		{
			SendFmt<<"Scanning, found "<<m_nProgIdx<<" file"
					<< (m_nProgIdx>1?"s":"") << "...<br />\n";
			m_nProgTell*=2;
		}
	}
	void AddDelCbox(cmstring &sFileRel);

private:
	bool Propagate(const string &donorRel, tContId2eqClass::iterator eqClassIter,
			cmstring *psTmpUnpackedAbs=NULL);
	void InstallBz2edPatchResult(tContId2eqClass::iterator &eqClassIter);
	tCacheOperation(const tCacheOperation&);
	tCacheOperation& operator=(const tCacheOperation&);
	bool PatchFile(const mstring &srcRel, const mstring &patchIdxLocation,
			tPListConstIt pit, tPListConstIt itEnd,
			const tFingerprint *verifData);
	dlcon *m_pDlcon;
	tContId2eqClass m_eqClasses;

	bool IsDeprecatedArchFile(cmstring &sFilePathRel);

	const tIfileAttribs attr_dummy_pure;
	tIfileAttribs attr_dummy;

	tStrSet m_delCboxFilter;
};


static const string compSuffixes[] = { ".bz2", ".gz", ".lzma", ".xz"};
static const string compSuffixesAndEmpty[] = { ".bz2", ".gz", ".lzma", ".xz", ""};
static const string compSuffixesAndEmptyByLikelyhood[] = { "", ".bz2", ".gz", ".lzma", ".xz"};
static const string compSuffixesAndEmptyByRatio[] = { ".xz", ".lzma", ".bz2", ".gz", ""};

bool CompDebVerLessThan(cmstring &s1, cmstring s2);
extern time_t m_gMaintTimeNow;

#endif /*_CACHEMAN_H_*/
