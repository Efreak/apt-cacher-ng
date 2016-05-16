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
#include <unordered_set>

// #define USEDUPEFILTER

class dlcon;
class tDlJobHints;
struct foo;

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

	enum enumMetaType
		: uint8_t
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
		EIDX_TRANSIDX, // XXX: in the old times, there were special i18n/Index files, are they gone for good now?
		EIDX_MD5DILIST,
		EIDX_SHA256DILIST
	};
protected:
	struct tIfileAttribs
	{
		bool vfile_ondisk:1, uptodate:1, parseignore:1, hideDlErrors:1,
				forgiveDlErrors:1, alreadyparsed:1,
				guessed:1; // file is not on disk, the name is pure calculation from pdiff mechanism
		enumMetaType eIdxType = EIDX_UNSUPPORTED;
		const tStrDeq *bros = nullptr;
		off_t space = 0;
		inline tIfileAttribs() :
				vfile_ondisk(false), uptodate(false),
				parseignore(false), hideDlErrors(false),
				forgiveDlErrors(false), alreadyparsed(false),
				guessed(false)
		{};
	};

	// this is not unordered because sometimes it might need modification
	// while an iterator still works on it
	std::map<mstring,tIfileAttribs> m_metaFilesRel;
	// helpers to keep the code cleaner and more readable
	const tIfileAttribs &GetFlags(cmstring &sPathRel) const;
	tIfileAttribs &SetFlags(cmstring &sPathRel);

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

	void ProcessSeenMetaFiles(std::function<void(tRemoteFileInfo)> pkgHandler);

	void StartDlder();

	enum eDlMsgPrio
	{
		eMsgHideAll,
		eMsgHideErrors,
		eMsgShow
	};
	bool Download(cmstring& sFilePathRel, bool bIsVolatileFile,
			eDlMsgPrio msgLevel, tFileItemPtr pForcedItem=tFileItemPtr(),
			const tHttpUrl *pForcedURL=nullptr, unsigned hints=0);
#define DL_HINT_GUESS_REPLACEMENT 0x1

	// common helper variables
	bool m_bErrAbort, m_bVerbose, m_bForceDownload, m_bSkipIxUpdate = false;
	bool m_bScanInternals, m_bByPath, m_bByChecksum, m_bSkipHeaderChecks;
	bool m_bTruncateDamaged;
	int m_nErrorCount;

	enumMetaType GuessMetaTypeFromURL(const mstring &sPath);

	unsigned int m_nProgIdx, m_nProgTell;

	void TellCount(unsigned nCount, off_t nSize);

	/**
	 * @param collectAllCsTypes If set, will send callbacks for all identified checksum types. In addition, will set the value of Acquire-By-Hash to the pointed boolean.
	 */
	bool ParseAndProcessMetaFile(std::function<void(const tRemoteFileInfo&)> output_receiver,
			const mstring &sPath, enumMetaType idxType, bool byHashMode = false);

	std::unordered_map<mstring,bool> m_forceKeepInTrash;

	bool GetAndCheckHead(cmstring & sHeadfile, cmstring &sFilePathRel, off_t nWantedSize);
	bool Inject(cmstring &fromRel, cmstring &toRel,
			bool bSetIfileFlags=true, const header *pForcedHeader=nullptr, bool bTryLink=false);

	void PrintStats(cmstring &title);
	mstring m_processedIfile;

	void ProgTell();
	void AddDelCbox(cmstring &sFileRel, bool bExtraFile = false);

	typedef std::pair<tFingerprint,mstring> tContId;
	struct tClassDesc {tStrDeq paths; tContId diffIdxId, bz2VersContId;};
	typedef std::map<tContId, tClassDesc> tContId2eqClass;

	// add certain files to the kill bill, to be removed after the activity is done
	virtual void MarkObsolete(cmstring&) {};

	// for compressed map of special stuff
	inline mstring AddLookupGetKey(cmstring &sFilePathRel, bool& isNew)
	{
		unsigned id = m_delCboxFilter.size();
		auto it = m_delCboxFilter.find(sFilePathRel);
		isNew = it==m_delCboxFilter.end();
		if(isNew)
			m_delCboxFilter[sFilePathRel] = id;
		else
			id = it->second;
		char buf[30];
		return mstring(buf, snprintf(buf, sizeof(buf), " name=\"kf\" value=\"%x\"", id));
	}

	// stuff in those directories must be managed by some top-level index files
	// whitelist patterns do not apply there!
	tStrSet m_managedDirs;

private:
	tContId2eqClass m_eqClasses;

	bool Propagate(cmstring &donorRel, tContId2eqClass::iterator eqClassIter,
			cmstring *psTmpUnpackedAbs=nullptr);
	void InstallBz2edPatchResult(tContId2eqClass::iterator &eqClassIter);
	tCacheOperation(const tCacheOperation&);
	tCacheOperation& operator=(const tCacheOperation&);
	bool PatchFile(cmstring &srcRel, cmstring &patchIdxLocation,
			tPListConstIt pit, tPListConstIt itEnd,
			const tFingerprint *verifData);
	dlcon *m_pDlcon = nullptr;

protected:
	bool CalculateBaseDirectories(cmstring& sPath, enumMetaType idxType, mstring& sBaseDir, mstring& sBasePkgDir);
	bool IsDeprecatedArchFile(cmstring &sFilePathRel);
	/**
	 * @brief Process key:val type files, handling multiline values as lists
	 * @param ixInflatedChecksum Pass through as struct attribute to ret callback
	 * @param sExtListFilter If set to non-empty, will only extract value(s) for that key
	 * @param byHashMode Return without calbacks if AcquireByHash is not set to yes. Not setting list filter also makes sense in this mode.
	 */
	bool ParseGenericRfc822Index(filereader& reader, std::function<void(const tRemoteFileInfo&)> &ret,
			cmstring& sCurFilesReferenceDirRel,
			cmstring& sPkgBaseDir,
			enumMetaType ixType, CSTYPES csType, bool ixInflatedChecksum,
			cmstring& sExtListFilter,
			bool byHashMode);
	const tIfileAttribs attr_dummy_pure = tIfileAttribs();
	tIfileAttribs attr_dummy;

	std::unordered_set<std::string> m_oldReleaseFiles, m_oldHashedFiles;
	virtual bool _checkSolidHashOnDisk(cmstring& hexname, const tRemoteFileInfo &entry);
	void BuildCacheFileList();
};


static cmstring compSuffixes[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring compSuffixesAndEmpty[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring compSuffixesAndEmptyByLikelyhood[] = { "", ".xz", ".bz2", ".gz", ".lzma",};
static cmstring compSuffixesAndEmptyByRatio[] = { ".xz", ".lzma", ".bz2", ".gz", ""};
static cmstring compSuffixesByRatio[] = { ".xz", ".lzma", ".bz2", ".gz"};

bool CompDebVerLessThan(cmstring &s1, cmstring s2);
extern time_t m_gMaintTimeNow;

#endif /*_CACHEMAN_H_*/
