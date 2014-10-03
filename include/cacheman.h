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
#include <unordered_map>

// #define USEDUPEFILTER

class dlcon;
class tDlJobHints;
struct foo;

static cmstring FAKEDATEMARK("Sat, 26 Apr 1986 01:23:39 GMT+3");
static cmstring sAbortMsg("<span class=\"ERROR\">Found errors during processing, "
		"aborting as requested.</span>");

static cmstring sIndex("Index");
static cmstring sslIndex("/Index");

struct tPatchEntry
{
	string patchName;
	tFingerprint fprState, fprPatch;
};
typedef deque<tPatchEntry>::const_iterator tPListConstIt;

void DelTree(const string &what);


class tCacheOperation :
	public IFileHandler,
	public tSpecOpDetachable
{

public:
	tCacheOperation(const tSpecialRequest::tRunParms& parms);
	virtual ~tCacheOperation();

protected:
	enum enumMetaType
		: uint_least8_t
		{
			EIDX_UNSUPPORTED = 0,
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
		bool vfile_ondisk=false, uptodate=false, parseignore=false, hideDlErrors=false,
				forgiveDlErrors=false, alreadyparsed=false, synthesized=false;
		enumMetaType eIdxType = EIDX_UNSUPPORTED;
		const tStrDeq *bros = nullptr;
		off_t space = 0;
	};

	std::unordered_map<mstring,tIfileAttribs> m_metaFilesRel;
	// helpers to keep the code cleaner and more readable
	const tIfileAttribs &GetFlags(cmstring &sPathRel) const;
	tIfileAttribs &SetFlags(cmstring &sPathRel);

	void SetCommonUserFlags(cmstring &cmd);

	void UpdateVolatileFiles();
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

	void ProcessSeenMetaFiles(ifileprocessor &pkgHandler);

	void StartDlder();

	enum eDlMsgPrio
	{
		eMsgHideAll,
		eMsgHideErrors,
		eMsgShow
	};
	bool Download(cmstring& sFilePathRel, bool bIsVolatileFile,
			eDlMsgPrio msgLevel, tFileItemPtr pForcedItem=tFileItemPtr(),
			const tHttpUrl *pForcedURL=NULL);

	// internal helper variables
	bool m_bErrAbort, m_bVerbose, m_bForceDownload;
	bool m_bScanInternals, m_bByPath, m_bByChecksum, m_bSkipHeaderChecks;
	bool m_bTruncateDamaged;
	int m_nErrorCount;

	enumMetaType GuessMetaTypeFromURL(const mstring &sPath);

	unsigned int m_nProgIdx, m_nProgTell;

	void TellCount(uint nCount, off_t nSize);

	bool ParseAndProcessMetaFile(ifileprocessor &output_receiver,
			const mstring &sPath, enumMetaType idxType);

	std::unordered_map<mstring,bool> m_forceKeepInTrash;

	bool GetAndCheckHead(cmstring & sHeadfile, cmstring &sFilePathRel, off_t nWantedSize);
	bool Inject(cmstring &fromRel, cmstring &toRel,
			bool bSetIfileFlags=true, const header *pForcedHeader=NULL, bool bTryLink=false);

	void PrintStats(cmstring &title);
	mstring m_processedIfile;

	void ProgTell();
	void AddDelCbox(cmstring &sFileRel);

public:
	typedef std::pair<tFingerprint,mstring> tContId;
	struct tClassDesc {tStrDeq paths; tContId diffIdxId, bz2VersContId;};
	typedef std::map<tContId, tClassDesc> tContId2eqClass;

private:

	tContId2eqClass m_eqClasses;

	bool Propagate(cmstring &donorRel, tContId2eqClass::iterator eqClassIter,
			cmstring *psTmpUnpackedAbs=NULL);
	void InstallBz2edPatchResult(tContId2eqClass::iterator &eqClassIter);
	tCacheOperation(const tCacheOperation&);
	tCacheOperation& operator=(const tCacheOperation&);
	bool PatchFile(cmstring &srcRel, cmstring &patchIdxLocation,
			tPListConstIt pit, tPListConstIt itEnd,
			const tFingerprint *verifData);
	dlcon *m_pDlcon;

	bool IsDeprecatedArchFile(cmstring &sFilePathRel);

	const tIfileAttribs attr_dummy_pure = tIfileAttribs();
	tIfileAttribs attr_dummy;
};


static cmstring compSuffixes[] = { ".bz2", ".gz", ".lzma", ".xz"};
static cmstring compSuffixesAndEmpty[] = { ".bz2", ".gz", ".lzma", ".xz", ""};
static cmstring compSuffixesAndEmptyByLikelyhood[] = { "", ".bz2", ".gz", ".lzma", ".xz"};
static cmstring compSuffixesAndEmptyByRatio[] = { ".xz", ".lzma", ".bz2", ".gz", ""};

bool CompDebVerLessThan(cmstring &s1, cmstring s2);
extern time_t m_gMaintTimeNow;

#endif /*_CACHEMAN_H_*/
