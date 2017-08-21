
//#define LOCAL_DEBUG
#include "debug.h"

#include "cacheman.h"
#include "expiration.h"
#include "lockable.h"
#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "fileitem.h"
#include "dlcon.h"
#include "dirwalk.h"
#include "header.h"
#include "job.h"

#include "fileio.h"
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <regex.h>

#include <map>
#include <unordered_map>
#include <string>
#include <iostream>
#include <algorithm>

#include <unistd.h>

#ifdef DEBUG
#warning enable, and it will spam a lot!
//#define DEBUGIDX
//#define DEBUGSPAM
#endif

#define MAX_TOP_COUNT 10

using namespace std;

namespace acng
{

static cmstring dis("/binary-");
static cmstring oldStylei18nIdx("/i18n/Index");
static cmstring diffIdxSfx(".diff/Index");

time_t m_gMaintTimeNow=0;

static cmstring sPatchCombinedRel("_actmp/combined.diff");
static cmstring sPatchInputRel("_actmp/patch.base");
static cmstring sPatchResultRel("_actmp/patch.result");

struct tPatchEntry
{
	string patchName;
	tFingerprint fprState, fprPatch;
};
typedef deque<tPatchEntry>::const_iterator tPListConstIt;

struct tContentKey
{
	mstring distinctName;
	tFingerprint fpr;
	mstring toString() const
	{
		return valid() ? distinctName + "|" + (mstring) fpr : sEmptyString;
	}
	bool operator<(const tContentKey& other) const
	{
		if(fpr == other.fpr)
			return distinctName < other.distinctName;
		return fpr < other.fpr;
	}
	bool valid() const { return fpr.csType != CSTYPES::CSTYPE_INVALID; }
};
struct tFileGroup {
	// the list shall be finally sorted by compression type (most favorable first)
	// and among the same type by modification date so the newest appears on top which
	// should be the most appropriate for patching
	tStrDeq paths;

	tContentKey diffIdxId;
#ifdef EXPLICIT_INDEX_USE_CHECKING
	bool isReferenced = false;
#endif
};
class tFileGroups : public std::map<tContentKey, tFileGroup> {};
cmstring& cacheman::GetFirstPresentPath(const tFileGroups& groups, const tContentKey& ckey)
{
	auto it = groups.find(ckey);
	if(it == groups.end())
		return sEmptyString;

	for(auto& s: it->second.paths)
		if(GetFlags(s).vfile_ondisk)
			return s;

	return sEmptyString;
}

cacheman::cacheman(const tSpecialRequest::tRunParms& parms) :
	tSpecOpDetachable(parms),
	m_bErrAbort(false), m_bVerbose(false), m_bForceDownload(false),
	m_bScanInternals(false), m_bByPath(false), m_bByChecksum(false), m_bSkipHeaderChecks(false),
	m_bTruncateDamaged(false),
	m_nErrorCount(0),
	m_nProgIdx(0), m_nProgTell(1), m_pDlcon(nullptr)
{
	m_szDecoFile="maint.html";
	m_gMaintTimeNow=GetTime();

	m_bErrAbort=(parms.cmd.find("abortOnErrors=aOe")!=stmiss);
	m_bByChecksum=(parms.cmd.find("byChecksum")!=stmiss);
	m_bByPath=(StrHas(parms.cmd, "byPath") || m_bByChecksum);
	m_bVerbose=(parms.cmd.find("beVerbose")!=stmiss);
	m_bForceDownload=(parms.cmd.find("forceRedownload")!=stmiss);
	m_bSkipHeaderChecks=(parms.cmd.find("skipHeadChecks")!=stmiss);
	m_bTruncateDamaged=(parms.cmd.find("truncNow")!=stmiss);
	m_bSkipIxUpdate=(m_parms.cmd.find("skipIxUp=si")!=stmiss);

}

cacheman::~cacheman()
{
	delete m_pDlcon;
	m_pDlcon=nullptr;
}

bool cacheman::ProcessOthers(const string &, const struct stat &)
{
	// NOOP
	return true;
}

bool cacheman::ProcessDirAfter(const string &, const struct stat &)
{
	// NOOP
	return true;
}


bool cacheman::AddIFileCandidate(const string &sPathRel)
{

 	if(sPathRel.empty())
 		return false;

	enumMetaType t;
	if ( (rex::FILE_VOLATILE == rex::GetFiletype(sPathRel)
	// SUSE stuff, not volatile but also contains file index data
	|| endsWithSzAr(sPathRel, ".xml.gz") )
	&& (t=GuessMetaTypeFromURL(sPathRel)))
 	{
		tIfileAttribs & atts=m_metaFilesRel[sPathRel];
 		atts.vfile_ondisk=true;
		atts.eIdxType=t;
 		return true;
     }
 	return false;
}

// defensive getter/setter methods, don't create non-existing entries
const cacheman::tIfileAttribs & cacheman::GetFlags(cmstring &sPathRel) const
{
	auto it=m_metaFilesRel.find(sPathRel);
	if(m_metaFilesRel.end()==it)
		return attr_dummy_pure;
	return it->second;
}
cacheman::tIfileAttribs &cacheman::SetFlags(cmstring &sPathRel)
{
	ASSERT(!sPathRel.empty());
	return sPathRel.empty() ? attr_dummy : m_metaFilesRel[sPathRel];
}

cacheman::tIfileAttribs & cacheman::GetRWFlags(cmstring &sPathRel)
{
	auto it=m_metaFilesRel.find(sPathRel);
	if(m_metaFilesRel.end()==it)
		return const_cast<cacheman::tIfileAttribs&>(attr_dummy_pure);
	return it->second;
}

// detects when an architecture has been removed entirely from the Debian archive
bool cacheman::IsDeprecatedArchFile(cmstring &sFilePathRel)
{
	tStrPos pos = sFilePathRel.rfind("/dists/");
	if(pos == stmiss)
		return false; // cannot tell
	pos=sFilePathRel.find_first_not_of('/', pos+7);
	if(pos == stmiss)
		return false;
	pos=sFilePathRel.find('/', pos);
	if(pos == stmiss)
		return false;
	// should match the path up to Release/InRelease file

	if(endsWithSzAr(sFilePathRel, "Release") && pos >= sFilePathRel.length()-9)
		return false; // that would be the file itself, or InRelease


	string s;
	filereader reader;
	if( (s=sFilePathRel.substr(0, pos)+"/Release",
			GetFlags(s).uptodate && reader.OpenFile(SABSPATH(s)))
			||
			(s=sFilePathRel.substr(0, pos)+"/InRelease",
						GetFlags(s).uptodate && reader.OpenFile(SABSPATH(s))
						)
	)
	{
		pos = sFilePathRel.find("/binary-", pos);
		if(stmiss == pos)
			return false; // heh?
		pos+=8;
		tStrPos posend = sFilePathRel.find('/', pos);
		if(stmiss == posend)
			return false; // heh?

		mstring sLine;
		while(reader.GetOneLine(sLine))
		{
			tSplitWalk w(&sLine, SPACECHARS);
			if(!w.Next() || w.str() != "Architectures:")
				continue;
			while(w.Next())
			{
				if(sFilePathRel.compare(pos, posend-pos, w.str()) == 0)
					return false; // architecture is there, not deprecated
			}
			return true; // okay, now that arch should have been there :-(
		}
	}

	return false;
}

/*
mstring FindCommonPath(cmstring& a, cmstring& b)
{
	LPCSTR pa(a.c_str), pb(b.c_str());
	LPCSTR po(pa), lspos(pa);
	while(*pa && *pb) { if(*pa == '/') lspos = pa;  ++pa; ++pb; }
	return a.substr(0, lspos-po);
}
*/

bool cacheman::Download(cmstring& sFilePathRel, bool bIsVolatileFile,
		cacheman::eDlMsgPrio msgVerbosityLevel,
		tFileItemPtr pFi, const tHttpUrl * pForcedURL, unsigned hints,
		cmstring* sGuessedFrom)
{

	LOGSTART("tCacheMan::Download");

	mstring sErr;
	bool bSuccess=false;

//	bool holdon = sFilePathRel == "debrep/dists/experimental/contrib/binary-amd64/Packages";

#define NEEDED_VERBOSITY_ALL_BUT_ERRORS (msgVerbosityLevel >= eMsgHideErrors)
#define NEEDED_VERBOSITY_EVERYTHING (msgVerbosityLevel >= eMsgShow)

	const tIfileAttribs &flags=GetFlags(sFilePathRel);
	if(flags.uptodate)
	{
		if(NEEDED_VERBOSITY_ALL_BUT_ERRORS)
			SendFmt<<"Checking "<<sFilePathRel<< (bIsVolatileFile
					? "... (fresh)<br>\n" : "... (complete)<br>\n");
		return true;
	}

#define GOTOREPMSG(x) {sErr = x; bSuccess=false; goto rep_dlresult; }

	const cfg::tRepoData *pRepoDesc=nullptr;
	mstring sRemoteSuffix, sFilePathAbs(SABSPATH(sFilePathRel));

	fileItemMgmt fiaccess;
	tHttpUrl parserPath, parserHead;
	const tHttpUrl *pResolvedDirectUrl=nullptr;

	// header could contained malformed data and be nuked in the process,
	// try to get the original source whatever happens
	header hor;
	hor.LoadFromFile(sFilePathAbs + ".head");

	if(!pFi)
	{
		fiaccess.PrepareRegisteredFileItemWithStorage(sFilePathRel, false);
		pFi=fiaccess.getFiPtr();
	}
	if (!pFi)
	{
		if (NEEDED_VERBOSITY_ALL_BUT_ERRORS)
			SendFmt << "Checking " << sFilePathRel << "...\n"; // just display the name ASAP
		GOTOREPMSG(" could not create file item handler.");
	}

	if (bIsVolatileFile && m_bForceDownload)
	{
		if (!pFi->SetupClean())
			GOTOREPMSG("Item busy, cannot reload");
		if (NEEDED_VERBOSITY_ALL_BUT_ERRORS)
			SendFmt << "Downloading " << sFilePathRel << "...\n";
	}
	else
	{
		if(bIsVolatileFile && m_bSkipIxUpdate)
		{
			SendFmt << "Checking " << sFilePathRel << "... (skipped, as requested)<br>\n";
			return true;
		}

		fileitem::FiStatus initState = pFi->Setup(bIsVolatileFile);
		if (initState > fileitem::FIST_COMPLETE)
			GOTOREPMSG(pFi->GetHeader().frontLine);
		if (fileitem::FIST_COMPLETE == initState)
		{
			int hs = pFi->GetHeader().getStatus();
			if(hs != 200)
			{
				SendFmt << "Error downloading " << sFilePathRel << ":\n";
				goto format_error;
				//GOTOREPMSG(pFi->GetHeader().frontLine);
			}
			SendFmt << "Checking " << sFilePathRel << "... (complete)<br>\n";
			return true;
		}
		if (NEEDED_VERBOSITY_ALL_BUT_ERRORS)
			SendFmt << (bIsVolatileFile ? "Checking/Updating " : "Downloading ")
			<< sFilePathRel	<< "...\n";
	}

	StartDlder();

	if (pForcedURL)
		pResolvedDirectUrl=pForcedURL;
	else
	{
		// must have the URL somewhere

		bool bCachePathAsUriPlausible=parserPath.SetHttpUrl(sFilePathRel, false);
		ldbg("Find backend for " << sFilePathRel << " parsed as host: "  << parserPath.sHost
				<< " and path: " << parserPath.sPath << ", ok? " << bCachePathAsUriPlausible);

		if(!cfg::stupidfs && bCachePathAsUriPlausible
				&& 0 != (pRepoDesc = cfg::GetRepoData(parserPath.sHost))
				&& !pRepoDesc->m_backends.empty())
		{
			ldbg("will use backend mode, subdirectory is path suffix relative to backend uri");
			sRemoteSuffix=parserPath.sPath.substr(1);
		}
		else
		{
			// ok, cache location does not hint to a download source,
			// try to resolve to an URL based on the old header information;
			// if not possible, guessing by looking at related files and making up
			// the URL as needed

			if(bCachePathAsUriPlausible) // default fallback, unless the next check matches
				pResolvedDirectUrl = parserPath.NormalizePath();

			// and prefer the source from xorig which is likely to deliver better result
			if(hor.h[header::XORIG] && parserHead.SetHttpUrl(hor.h[header::XORIG], false))
				pResolvedDirectUrl = parserHead.NormalizePath();
			else if(sGuessedFrom
					&& hor.LoadFromFile(SABSPATH(*sGuessedFrom + ".head"))
					&& hor.h[header::XORIG]) // might use a related file as reference
			{
				mstring refURL(hor.h[header::XORIG]);

				tStrPos spos(0); // if not 0 -> last slash sign position if both
				for(tStrPos i=0; i< sGuessedFrom->size() && i< sFilePathRel.size(); ++i)
				{
					if(sFilePathRel[i] != sGuessedFrom->at(i))
						break;
					if(sFilePathRel[i] == '/')
						spos = i;
				}
				// cannot underflow since checked by less-than
				auto chopLen = sGuessedFrom->length() - spos;
				auto urlSlashPos = refURL.size()-chopLen;
				if(chopLen < refURL.size() && refURL[urlSlashPos] == '/')
				{
					refURL.erase(urlSlashPos);
					refURL += sFilePathRel.substr(spos);
					//refURL.replace(urlSlashPos, chopLen, sPathSep.substr(spos));
					if(parserHead.SetHttpUrl(refURL, false))
						pResolvedDirectUrl = parserHead.NormalizePath();
				}
			}

			if(!pResolvedDirectUrl)
			{
				SendChunkSZ("<b>Failed to resolve original URL</b><br>");
				return false;
			}
		}
	}

	// might still need a repo data description
	if (pResolvedDirectUrl)
	{
		cfg::tRepoResolvResult repinfo;
		cfg::GetRepNameAndPathResidual(*pResolvedDirectUrl, repinfo);
		auto hereDesc = repinfo.repodata;
		if(repinfo.repodata && !repinfo.repodata->m_backends.empty())
		{
			pResolvedDirectUrl = nullptr;
			pRepoDesc = hereDesc;
			sRemoteSuffix = repinfo.sRestPath;
		}
	}

	m_pDlcon->AddJob(pFi, pResolvedDirectUrl, pRepoDesc, &sRemoteSuffix, 0, cfg::REDIRMAX_DEFAULT);

	m_pDlcon->WorkLoop();
	if (pFi->WaitForFinish(nullptr) == fileitem::FIST_COMPLETE
			&& pFi->GetHeaderUnlocked().getStatus() == 200)
	{
		bSuccess = true;
		if (NEEDED_VERBOSITY_ALL_BUT_ERRORS)
			SendFmt << "<i>(" << pFi->GetTransferCount() / 1024 << "KiB)</i>\n";
	}
	else
	{
		format_error:
		if (IsDeprecatedArchFile(sFilePathRel))
		{
			if (NEEDED_VERBOSITY_EVERYTHING)
				SendChunkSZ("<i>(no longer available)</i>\n");
			m_forceKeepInTrash[sFilePathRel] = true;
			bSuccess = true;
		}
		else if (flags.forgiveDlErrors
				||
				(pFi->GetHeaderUnlocked().getStatus() == 404
								&& endsWith(sFilePathRel, oldStylei18nIdx))
		)
		{
			bSuccess = true;
			if (NEEDED_VERBOSITY_EVERYTHING)
				SendChunkSZ("<i>(ignored)</i>\n");
		}
		else
			GOTOREPMSG(pFi->GetHttpMsg());
	}

	rep_dlresult:

	if(pFi)
	{
		auto dlCount = pFi->GetTransferCount();
		static cmstring sInternal("[INTERNAL:");
		// need to account both, this traffic as officially tracked traffic, and also keep the count
		// separately for expiration about trade-off calculation
		log::transfer(dlCount, 0, sInternal + GetTaskName() + "]", sFilePathRel, false);
	}

	if (bSuccess && bIsVolatileFile)
		SetFlags(sFilePathRel).uptodate = true;

	if(!bSuccess)
	{
		if(pRepoDesc && pRepoDesc->m_backends.empty() && !hor.h[header::XORIG] && !pForcedURL)
		{
			// oh, that crap: in a repo, but no backends configured, and no original source
			// to look at because head file is probably damaged :-(
			// try to re-resolve relative to InRelease and retry download
			SendChunkSZ("<span class=\"WARNING\">"
					"Warning, running out of download locations (probably corrupted "
					"cache). Trying an educated guess...<br>\n"
					")</span>\n<br>\n");

			cmstring::size_type pos=sFilePathRel.length();
			while(true)
			{
				pos=sFilePathRel.rfind(CPATHSEPUNX, pos);
				if(pos == stmiss)
					break;
				for(const auto& sfx : {&inRelKey, &relKey})
				{
					if(endsWith(sFilePathRel, *sfx))
						continue;

					auto testpath=sFilePathRel.substr(0, pos) + *sfx;
					if(GetFlags(testpath).vfile_ondisk)
					{
						header hare;
						if(hare.LoadFromFile(SABSPATH(testpath)+".head")
								&& hare.h[header::XORIG])
						{
							string url(hare.h[header::XORIG]);
							url.replace(url.size() - sfx->size(), sfx->size(), sFilePathRel.substr(pos));
							tHttpUrl tu;
							if(tu.SetHttpUrl(url, false))
							{
								SendChunkSZ("Restarting download... ");
								if(pFi)
									pFi->ResetCacheState();
								return Download(sFilePathRel, bIsVolatileFile,
										msgVerbosityLevel, tFileItemPtr(), &tu);
							}
						}
					}
				}
				if(!pos)
					break;
				pos--;
			}
		}
		else if((hints&DL_HINT_GUESS_REPLACEMENT)
				&& pFi->GetHeaderUnlocked().getStatus() == 404)
		{
			// another special case, slightly ugly :-(
			// this is explicit hardcoded repair code
			// it switches to a version with better compression silently
			static struct {
				string fromEnd, toEnd, extraCheck;
			} fixmap[] =
			{
					{ "/Packages.bz2", "/Packages.xz", "" },
					{ "/Sources.bz2", "/Sources.xz", "" },
					{ "/Release", "/InRelease", "" },
					{ "/InRelease", "/Release", "" },
					{ ".bz2", ".xz", "i18n/Translation-" },
					{ "/Packages.gz", "/Packages.xz", "" },
					{ "/Sources.gz", "/Sources.xz", "" }
			};
			for(const auto& fix : fixmap)
			{
				if(!endsWith(sFilePathRel, fix.fromEnd) || !StrHas(sFilePathRel, fix.extraCheck))
					continue;
				SendChunkSZ("Attempting to download the alternative version... ");
				// if we have it already, use it as-is
				if (!pResolvedDirectUrl)
				{
					auto p = pFi->GetHeaderUnlocked().h[header::XORIG];
					if (p && parserHead.SetHttpUrl(p))
						pResolvedDirectUrl = &parserHead;
				}
				auto newurl(*pResolvedDirectUrl);
				if(!endsWith(newurl.sPath, fix.fromEnd) || !StrHas(newurl.sPath, fix.extraCheck))
					continue;
				newurl.sPath.replace(newurl.sPath.size()-fix.fromEnd.size(), fix.fromEnd.size(),
						fix.toEnd);
				if(Download(sFilePathRel.substr(0, sFilePathRel.size() - fix.fromEnd.size())
						+ fix.toEnd, bIsVolatileFile, msgVerbosityLevel, tFileItemPtr(), &newurl,
				hints&~DL_HINT_GUESS_REPLACEMENT ))
				{
					MarkObsolete(sFilePathRel);
					return true;
				}
				// XXX: this sucks a little bit since we don't want to show the checkbox
				// when the fallback download succeeded... but on failures, the previous one
				// already added a newline before
				AddDelCbox(sFilePathRel, sErr, true);
				return false;
			}
		}
		//else
		//	AddDelCbox(sFilePathRel);

		if (sErr.empty())
			sErr = "Download error";
		if (NEEDED_VERBOSITY_EVERYTHING || m_bVerbose)
		{
			if(!flags.hideDlErrors)
			{
				SendFmt << "<span class=\"ERROR\">" << sErr << "</span>\n";
				if(0 == (hints&DL_HINT_NOTAG))
					AddDelCbox(sFilePathRel, sErr);
			}
		}
	}

	// there must have been output
	if (NEEDED_VERBOSITY_ALL_BUT_ERRORS)
		SendChunk("\n<br>\n");

	return bSuccess;
}

#define ERRMSGABORT if(m_nErrorCount && m_bErrAbort) { SendChunk(sErr); return; }
#define ERRABORT if(m_nErrorCount && m_bErrAbort) { return; }

inline tStrPos FindCompSfxPos(const string &s)
{
	for(auto &p : sfxXzBz2GzLzma)
		if(endsWith(s, p))
			return(s.size()-p.size());
	return stmiss;
}

static short FindCompIdx(cmstring &s)
{
	for(unsigned i=0;i<_countof(sfxXzBz2GzLzma); ++i)
		if(endsWith(s, sfxXzBz2GzLzma[i]))
			return i;
	return -1;
}

void DelTree(const string &what)
{
	class killa : public IFileHandler
	{
		virtual bool ProcessRegular(const mstring &sPath, const struct stat &)
		{
			::unlink(sPath.c_str()); // XXX log some warning?
			return true;
		}
		bool ProcessOthers(const mstring &sPath, const struct stat &x)
		{
			return ProcessRegular(sPath, x);
		}
		bool ProcessDirAfter(const mstring &sPath, const struct stat &)
		{
			::rmdir(sPath.c_str()); // XXX log some warning?
			return true;
		}
	} hh;
	IFileHandler::DirectoryWalk(what, &hh, false, false);
}

// crap
#if 0
struct lessThanByAvailability
{
	cacheman& m_cman;
	lessThanByAvailability(cacheman &cman) :
		m_cman(cman)
	{
	}

	bool operator()(const string &s1, const string &s2) const
		{
		auto f1(m_cman.GetFlags(s1)), f2(m_cman.GetFlags(s2));
		if(f1.vfile_ondisk && !f2.vfile_ondisk)
			return true;
		if(!f1.vfile_ondisk && f2.vfile_ondisk)
			return false;
		if(!f1.vfile_ondisk)
			return false;
		// both here?
		Cstat st1(SABSPATH(s1)), st2(SABSPATH(s2));
		// errors?
		if(!st1) st1.st_mtim.tv_sec = 0;
		if(!st2) st2.st_mtim.tv_sec = 0;
		return st1.st_mtim.tv_sec > st2.st_mtim.tv_sec;
		}
};

struct tEqualToFingerprint : public tFingerprint
{
	tEqualToFingerprint(const tFingerprint &re) : tFingerprint(re) {}
	bool operator()(const tPatchEntry &other) const { return other.fprState == *this; }
};


struct tCompRateIterator
{
	tStrDeq& _stuff;
	tStrDeq::iterator it;
	tCompRateIterator(tStrDeq& stuff) : _stuff(stuff) { it = _stuff.end();};
	mstring& value() { return *it;	}
	bool next() {
		if(it == _stuff.end()) {it = _stuff.begin(); return it != _stuff.end();}

	}
};

#endif


tFingerprint * BuildPatchList(string sFilePathAbs, deque<tPatchEntry> &retList)
{
	retList.clear();
	string sLine;
	static tFingerprint ret;
	ret.csType=CSTYPE_INVALID;

	filereader reader;
	if(!reader.OpenFile(sFilePathAbs))
		return nullptr;

	enum { eCurLine, eHistory, ePatches} eSection;
	eSection=eCurLine;

	unsigned peAnz(0);

	// This code should be tolerant to minor changes in the format

	tStrVec tmp;
	off_t otmp;
	while(reader.GetOneLine(sLine))
	{
		int nTokens=Tokenize(sLine, SPACECHARS, tmp);
		if(3==nTokens)
		{
			if(tmp[0] == "SHA1-Current:")
			{
				otmp=atoofft(tmp[2].c_str(), -2);
				if(otmp<0 || !ret.Set(tmp[1], CSTYPE_SHA1, otmp))
					return nullptr;
			}
			else
			{
				tFingerprint fpr;
				otmp=atoofft(tmp[1].c_str(),-2);
				if(otmp<0 || !fpr.Set(tmp[0], CSTYPE_SHA1, otmp))
					return nullptr;

				if(peAnz && retList[peAnz%retList.size()].patchName == tmp[2])
					// oh great, this is also our target
				{
					if (eHistory == eSection)
						retList[peAnz%retList.size()].fprState = fpr;
					else
						retList[peAnz%retList.size()].fprPatch = fpr;
				}
				else
				{
					retList.resize(retList.size()+1);
					retList.back().patchName=tmp[2];
					if (eHistory == eSection)
						retList.back().fprState = fpr;
					else
						retList.back().fprPatch = fpr;
				}

				peAnz++;
			}
		}
		else if(1==nTokens)
		{
			if(tmp[0] == "SHA1-History:")
				eSection=eHistory;
			else if(tmp[0] == "SHA1-Patches:")
				eSection=ePatches;
			else
				return nullptr;
		}
		else if(nTokens) // not null but weird count
			return nullptr; // error
	}

	return ret.csType != CSTYPE_INVALID ? &ret : nullptr;
}



bool cacheman::GetAndCheckHead(cmstring & sTempDataRel, cmstring &sReferencePathRel,
		off_t nWantedSize)
{


	class tHeadOnlyStorage: public fileitem_with_storage
	{
	public:

		cmstring & m_sTempDataRel, &m_sReferencePathRel;
		off_t m_nGotSize;

		tHeadOnlyStorage(cmstring & sTempDataRel, cmstring &sReferencePathRel) :

			fileitem_with_storage(sTempDataRel) // storage ref to physical data file
					,
			m_sTempDataRel(sTempDataRel),
			m_sReferencePathRel(sReferencePathRel), m_nGotSize(-1)


		{
			m_bAllowStoreData = false;
			m_bHeadOnly = true;
			m_head.LoadFromFile(SABSPATH(m_sReferencePathRel+".head"));
		}
		~tHeadOnlyStorage()
		{
			m_head.StoreToFile( CACHE_BASE + m_sPathRel + ".head");
			m_nGotSize = atoofft(m_head.h[header::CONTENT_LENGTH], -17);
		}
	};

	auto p(make_shared<tHeadOnlyStorage>(sTempDataRel, sReferencePathRel));
	return (Download(sReferencePathRel, true, eMsgHideAll, p)
			&& ( (tHeadOnlyStorage*) p.get())->m_nGotSize == nWantedSize);
}



bool cacheman::Inject(cmstring &fromRel, cmstring &toRel,
		bool bSetIfileFlags, const header *pHead, bool bTryLink)
{
	LOGSTART("tCacheMan::Inject");

	// XXX should it really filter it here?
	if(GetFlags(toRel).uptodate)
		return true;

	auto sFromAbs(SABSPATH(fromRel)), sToAbs(SABSPATH(toRel));

	Cstat infoFrom(sFromAbs), infoTo(sToAbs);
	if(infoFrom && infoTo && infoFrom.st_ino == infoTo.st_ino && infoFrom.st_dev == infoTo.st_dev)
		return true;

#ifdef DEBUG_FLAGS
	bool nix = stmiss!=fromRel.find("debrep/dists/squeeze/non-free/binary-amd64/");
	SendFmt<<"Replacing "<<toRel<<" with " << fromRel <<  sBRLF;
#endif

	if(!infoFrom)
	{
		MTLOGASSERT(0, "Bad source file: " << sFromAbs);
		return false;
	}

	header head;

	if (!pHead)
	{
		pHead = &head;

		if (head.LoadFromFile(sFromAbs+".head") > 0 && head.h[header::CONTENT_LENGTH])
		{
			if(infoFrom.st_size != atoofft(head.h[header::CONTENT_LENGTH]))
			{
				MTLOGASSERT(0, "Bad file size");
				return false;
			}
		}
		else if(head.LoadFromFile(sToAbs+".head") > 0)
		{
			head.set(header::CONTENT_LENGTH, (off_t) infoFrom.st_size);
		}
		else
		{
			MTLOGASSERT(0, "Cannot build meta data for " << sToAbs);
			return false;
		}
		head.set(header::CONTENT_TYPE, "octet/stream");
		head.set(header::LAST_MODIFIED, FAKEDATEMARK);
	}

	class tInjectItem : public fileitem_with_storage
	{
	public:
		bool m_link;
		tInjectItem(cmstring &to, bool bTryLink) : fileitem_with_storage(to), m_link(bTryLink)
		{
		}
		// noone else should attempt to store file through it
		virtual bool DownloadStartedStoreHeader(const header &, size_t, const char *,
				bool, bool&) override
		{
			return false;
		}
		virtual bool Inject(cmstring &fromRel, const header &head)
		{
			m_head = head;
			mstring sPathAbs = SABSPATH(m_sPathRel);
			mkbasedir(sPathAbs);
			if(head.StoreToFile(sPathAbs+".head") <=0)
				return false;

			if(m_link)
			{
				setLockGuard;
				if (LinkOrCopy(SABSPATH(fromRel), sPathAbs))
				{
					m_status = FIST_COMPLETE;
					notifyAll();
					return true;
				}
			}

			// shit. Permission problem? Use the old fashioned way :-(
			filereader data;
			if(!data.OpenFile(SABSPATH(fromRel), true))
				return false;

			/* evil...
			if(!m_link)
			{
				// just in case it's the same file, kill the target
				::unlink(m_sPathAbs.c_str());
				::unlink((m_sPathAbs+".head").c_str());
			}
			*/

			if (Setup(true) > fileitem::FIST_COMPLETE)
				return false;
			bool bNix(false);
			if (!fileitem_with_storage::DownloadStartedStoreHeader(head, 0,
					nullptr, false, bNix))
				return false;
			if(!StoreFileData(data.GetBuffer(), data.GetSize()) || ! StoreFileData(nullptr, 0))
				return false;
			if(GetStatus() != FIST_COMPLETE)
				return false;
			return true;
		}
	};
	auto pfi(make_shared<tInjectItem>(toRel, bTryLink));
	// register it in global scope
	fileItemMgmt fi;
	if(!fi.RegisterFileItem(pfi))
	{
		MTLOGASSERT(false, "Couldn't register copy item");
		return false;
	}
	bool bOK = pfi->Inject(fromRel, *pHead);

	MTLOGASSERT(bOK, "Inject: failed");

	if(bSetIfileFlags)
	{
		tIfileAttribs &atts = SetFlags(toRel);
		atts.uptodate = atts.vfile_ondisk = bOK;
	}

	return bOK;
}

void cacheman::StartDlder()
{
	if (!m_pDlcon)
		m_pDlcon = new dlcon(true);
}

void cacheman::ExtractAllRawReleaseDataFixStrandedPatchIndex(tFileGroups& idxGroups,
		const tStrDeq& releaseFilesRel)
{
	for(auto& sPathRel : releaseFilesRel)
	{
#ifdef DEBUG
		SendFmt << "Start parsing " << sPathRel << "<br>";
#endif
		// raw data extraction
		{
			typedef map<string, tContentKey> tFile2Cid;
			// pull all contents into a sorted dictionary for later filtering
			tFile2Cid file2cid;
			auto recvInfo = [&file2cid, this](const tRemoteFileInfo &entry)
									{
#if 0 // bad, keeps re-requesting update of such stuff forever. Better let the quick content check analyze and skip them.
				tStrPos compos=FindCompSfxPos(entry.sFileName);
				// skip some obvious junk and its gzip version
				if(0==entry.fpr.size || (entry.fpr.size<33 && stmiss!=compos))
					return;
#endif
				auto& cid = file2cid[entry.sDirectory+entry.sFileName];
				cid.fpr=entry.fpr;

				tStrPos pos=entry.sDirectory.rfind(dis);
				// if looking like Debian archive, keep just the part after binary-...
				if(stmiss != pos)
					cid.distinctName=entry.sDirectory.substr(pos)+entry.sFileName;
				else
					cid.distinctName=entry.sFileName;
									};

			ParseAndProcessMetaFile(std::function<void (const tRemoteFileInfo &)>(recvInfo),
					sPathRel, EIDX_RELEASE);

			if(file2cid.empty())
				continue;


#ifndef EXPLICIT_INDEX_USE_CHECKING
			// first, look around for for .diff/Index files on disk, update them, check patch base file
			// and make sure one is there or something is not right
			for(const auto cid : file2cid)
			{
				// not diff index or not in cache?
				if(!endsWith(cid.first, diffIdxSfx))
					continue;
				auto& flags = GetFlags(cid.first);
				if(!flags.vfile_ondisk)
					continue;
				//if(!flags.uptodate && !Download(cid.first, true, eMsgShow))
				//	continue;

				// ok, having a good .diff/Index file, what now?

				string sBase=cid.first.substr(0, cid.first.size()-diffIdxSfx.size());

				// two rounds, try to find any in descending order, then try to download one
				for(int checkmode=0; checkmode < 3; checkmode++)
				{
					for(auto& suf: sfxXzBz2GzLzma)
					{
						auto cand(sBase+suf);
						if(checkmode == 0)
						{
							if(_QuickCheckSolidFileOnDisk(cand))
								goto found_base;
						}
						else if(checkmode == 1) // now really check on disk
						{
							if(0 == ::access(SZABSPATH(cand), F_OK))
								goto found_base;
						}
						else
						{
							SendFmt << "No base file to use patching on " << cid.first << ", trying to fetch " << cand << hendl;
							if(Download(cand, true, eMsgHideErrors, tFileItemPtr(), 0, 0, &cid.first))
							{
								SetFlags(cand).vfile_ondisk=true;
								goto found_base;
							}
						}
					}
				}
				found_base:;
			}
#endif
			//dbgState();

			// now refine all extracted information and store it in eqClasses for later processing
			for(auto if2cid : file2cid)
			{
				string sNativeName=if2cid.first.substr(0, FindCompSfxPos(if2cid.first));
				tContentKey groupId;

				// identify the key for this group. Ideally, this is the descriptor
				// of the native representation, if found in the release file, in doubt
				// take the one from the best compressed form or the current one
				auto it2=file2cid.find(sNativeName);
				if(it2 != file2cid.end())
					groupId=it2->second;
				else
					for(auto& ps : sfxXzBz2GzLzma)
						ifThereStoreThereAndBreak(file2cid, sNativeName+ps, groupId);
				if(!groupId.valid())
					groupId = if2cid.second;

				tFileGroup &tgt = idxGroups[groupId];
				tgt.paths.emplace_back(if2cid.first);

				// also the index file id
				if(!tgt.diffIdxId.valid()) // XXX if there is no index at all, avoid repeated lookups somehow?
					ifThereStoreThere(file2cid, sNativeName+diffIdxSfx, tgt.diffIdxId);

				// and while we are at it, check the checksum of small files in order
				// to reduce server request count
				if(if2cid.second.fpr.size < 42000)
				{
					auto& flags = GetRWFlags(if2cid.first);
					if(flags.vfile_ondisk && if2cid.second.fpr.CheckFile(SABSPATH(if2cid.first)))
						flags.uptodate=true;
				}

			}
		}
	}

}

/*
* First, strip the list down to those which are at least partially present in the cache.
* And keep track of some folders for expiration.
*/

void cacheman::FilterGroupData(tFileGroups& idxGroups)
{
	for(auto it=idxGroups.begin(); it!=idxGroups.end();)
	{
		unsigned found = 0;
		for(auto& path: it->second.paths)
		{
			// WARNING: this check works only as long as stuff is volatile AND index type
			if(!GetFlags(path).vfile_ondisk)
				continue;
			found++;
			auto pos = path.rfind('/');
			if(pos!=stmiss)
				m_managedDirs.insert(path.substr(0, pos+1));
		}

		if(found)
		{
#ifdef EXPLICIT_INDEX_USE_CHECKING
			//bool holdon = StrHas(it->second.paths.front(), "contrib/binary-amd64/Packa");
			// remember that index file might be used by other groups
			if(it->second.diffIdxId.valid())
			{
				auto indexIter = idxGroups.find(it->second.diffIdxId);
				if(indexIter != idxGroups.end())
					indexIter->second.isReferenced = true;
			}
#endif
			++it;
		}
		else
			idxGroups.erase(it++);
	}
#ifdef DEBUGIDX
	SendFmt << "Folders with checked stuff:" << hendl;
	for(auto s : m_managedDirs)
		SendFmt << s << hendl;
#endif
#ifdef EXPLICIT_INDEX_USE_CHECKING
	// some preparation of patch index processing
	for(auto& group: idxGroups)
	{
		if(!endsWith(group.first.distinctName, diffIdxSfx))
			continue;
		for(auto& indexPath: group.second.paths)
			Download(indexPath, true, eMsgShow, tFileItemPtr(), 0, 0);

		if(group.second.isReferenced)
			continue;

		// existing but unreferenced pdiff index files are bad, that means that some client
		// is tracking this stuff via diff update but ACNG has no clue of the remaining contents
		// -> let's make sure the best compressed version is present on disk
		// In that special case the extra index files will not become active ASAP but that's
		// probably good enough for expiration activity (use it the day after).

		for(auto& indexPath: group.second.paths)
		{
			auto sBase = indexPath.substr(0, indexPath.size()-diffIdxSfx.size());
			SendFmt << "Warning: no base file to use patching on " << indexPath
					<< ", trying to fetch some" << hendl;
			for(auto& suf : sfxXzBz2GzLzma)
			{
				auto cand(sBase+suf);
				if(Download(cand, true, eMsgShow, tFileItemPtr(), 0, 0, &indexPath))
				{
					SetFlags(cand).vfile_ondisk=true;
					break;
				}
			}
		}
	}
#endif

}
void cacheman::SortAndInterconnectGroupData(tFileGroups& idxGroups)
{

	for(auto& it: idxGroups)
	{
#ifdef DEBUG
		for(auto&x : it.second.paths)
			assert(!x.empty());
#endif
		for(auto jit = it.second.paths.begin(); jit != it.second.paths.end();)
		{
			if(FindCompIdx(*jit) < 0) // uncompressed stays for now
				jit++;
			else if(GetFlags(*jit).vfile_ondisk)
				jit++;
			else
				jit = it.second.paths.erase(jit);
		}

		// FULLY clean data set, disk-referencing after this moment

		sort(it.second.paths.begin(), it.second.paths.end());
				//lessThanByAvailability(*this));

		// build a daisy chain for later download disarming
		tIfileAttribs *pfirst(nullptr), *pprev(nullptr);
		for(auto &path : it.second.paths)
		{
			auto it=m_metaFilesRel.find(path);
			if(it==m_metaFilesRel.end())
				continue;
			if(!pfirst)
			{
				pprev = pfirst = & it->second;
				continue;
			}
			pprev->bro = & it->second;
			pprev = & it->second;
		}
		if(pprev)
			pprev->bro = pfirst;
	}
}

void cacheman::PatchOne(cmstring& pindexPathRel, const tStrDeq& siblings)
{
//	cmstring* pBest = nullptr;
//	const tIfileAttribs* pBestAttr = nullptr;
	int changeNeedCount = 0;
	//tStrSet locationsInSidestore;

	for(const auto& pb: siblings)
	{
		auto& fl = GetFlags(pb);
		if(!fl.vfile_ondisk)
			continue;
		if(!fl.parseignore && !fl.uptodate)
			changeNeedCount++;
		//locationsInSidestore.emplace( pb.substr(0, FindCompSfxPos(pb)));
	}

	if(!changeNeedCount)
		return;
	filereader reader;
	if(!reader.OpenFile(SABSPATH(pindexPathRel), true, 1))
		return;
	map<string, deque<string> > contents;
	ParseGenericRfc822File(reader, "", contents);
	auto& sStateCurrent = contents["SHA256-Current"];
	if(sStateCurrent.empty() || sStateCurrent.front().empty())
		return;
	auto& csHist = contents["SHA256-History"];
	if(csHist.empty() || csHist.size() < 2)
		return;

	tFingerprint probeStateWanted, // the target data
	probe, // temp scan object
	probeOrig; // appropriate patch base stuff

	if(!probeStateWanted.Set(tSplitWalk(& sStateCurrent.front()), CSTYPE_SHA256))
				return;

	unordered_map<string,tFingerprint> patchSums;
	for(const auto& line: contents["SHA256-Patches"])
	{
		tSplitWalk split(&line);
		tFingerprint probe;
		if(!probe.Set(split, CSTYPE_SHA256) || !split.Next())
			continue;
		EMPLACE_PAIR_COMPAT(patchSums, split.str(), probe);
	}
	cmstring sPatchResultAbs(SABSPATH(sPatchResultRel));
	cmstring sPatchInputAbs(SABSPATH(sPatchInputRel));
	cmstring sPatchCombinedAbs(SABSPATH(sPatchCombinedRel));

	// returns true if a new patched file was created
	auto tryPatch = [&](cmstring& pbaseRel) -> bool
			{
		// XXX: use smarter line matching or regex
		auto probeCS = probeOrig.GetCsAsString();
		auto probeSize = offttos(probeOrig.size);
		FILE_RAII pf;
		for(const auto& histLine: csHist)
		{
			// quick filter
			if(!pf.p && !startsWith(histLine, probeCS))
				continue;

			// analyze the state line
			tSplitWalk split(&histLine, SPACECHARS);
			if(!split.Next() || !split.Next())
				continue;
			// at size token
			if(!pf.p && probeSize != split.str())
				return false; // faulty data?
			if(!split.Next())
				continue;
			string pname = split.str();
			trimString(pname);
			if(pname.empty())
				return false; // XXX: maybe throw int with line number

			// ok, first patch of the sequence found
			if(!pf.p)
			{
				acng::mkbasedir(sPatchCombinedAbs);
				// append mode!
				pf.p=fopen(sPatchCombinedAbs.c_str(), "w");
				if(!pf.p)
				{
					SendChunk("Failed to create intermediate patch file, stop patching...<br>");
					return false;
				}
			}
			// ok, so we started collecting patches...

			string patchPathRel(pindexPathRel.substr(0, pindexPathRel.size()-5) +
					pname + ".gz");
			if(!Download(patchPathRel, false, eDlMsgPrio::eMsgHideErrors,
					tFileItemPtr(), nullptr, DL_HINT_NOTAG, &pindexPathRel))
			{
				return false;
			}
			SetFlags(patchPathRel).parseignore = true; // static stuff, hands off!

			// append context to combined diff while unpacking
			// XXX: probe result can be checked against contents["SHA256-History"]
			if(!probe.ScanFile(SABSPATH(patchPathRel), CSTYPE_SHA256, true, pf.p))
			{
				if(m_bVerbose)
					SendFmt << "Failure on checking of intermediate patch data in " << patchPathRel << ", stop patching...<br>";
				return false;
			}
			if(probe != patchSums[pname])
			{
				SendFmt<< "Bad patch data in " << patchPathRel <<" , stop patching...<br>";
				return false;
			}
		}
		if(pf.p)
		{
			::fprintf(pf.p, "w patch.result\n");
			::fflush(pf.p); // still a slight risk of file closing error but it's good enough for now
			if(::ferror(pf.p))
			{
				SendChunk("Patch application error<br>");
				return false;
			}
			checkForceFclose(pf.p);

#ifndef DEBUGIDX
			if(m_bVerbose)
#endif
				SendChunk("Patching...<br>");

			tSS cmd;
			cmd << "cd '" << CACHE_BASE << "_actmp' && ";
			auto act = cfg::suppdir + SZPATHSEP "acngtool";
			if(!cfg::suppdir.empty() && 0==access(act.c_str(), X_OK))
				cmd << "'" << act << "' patch patch.base " << sPatchCombinedAbs << " patch.result";
			else
				cmd << " red --silent patch.base < " << sPatchCombinedAbs;

			if (::system(cmd.c_str()))
			{
				MTLOGASSERT(false, "Command failed: " << cmd);
				return false;
			}

			if (!probe.ScanFile(sPatchResultAbs, CSTYPE_SHA256, false))
			{
				MTLOGASSERT(false, "Scan failed: " << sPatchResultAbs);
				return false;
			}

			if(probe != probeStateWanted)
			{
				MTLOGASSERT(false,"Final verification failed");
				return false;
			}
			return true;
		}
		return false;
			};

	// start with uncompressed type, xz, bz2, gz, lzma
	for(auto itype : { -1, 0, 1, 2, 3})
	{
		for(const auto& pb : siblings)
		{
			if(itype != FindCompIdx(pb))
				continue;

			FILE_RAII df;
			DelTree(SABSPATH("_actmp"));
			acng::mkbasedir(sPatchInputAbs);
			df.p = fopen(sPatchInputAbs.c_str(), "w");
			if(!df.p)
			{
				SendFmt << "Cannot write temporary patch data to "
						<< sPatchInputRel << "<br>";
				return;
			}
			if(!probeOrig.ScanFile(SABSPATH(pb),
					CSTYPE_SHA256, true, df.p))
				continue;
			df.close();
			if(probeStateWanted == probeOrig)
			{
				SetFlags(pb).uptodate = true;
				SyncSiblings(pb, siblings);
				return; // the file is uptodate already...
			}

			if(!tryPatch(pb))
				continue; // only if fails on file checks

			// install to one of uncompressed locations, let SyncSiblings handle the rest
			for(auto& path: siblings)
			{
				// if possible, try to reconstruct reliable download information
				// inject might need it
				header h;
				header *ph(0);
				if(h.LoadFromFile(SABSPATH(pindexPathRel)+".head")
						&& h.h[header::XORIG])
				{
					auto len=strlen(h.h[header::XORIG]);
					if(len < diffIdxSfx.length())
						return; // heh?
					h.h[header::XORIG][len-diffIdxSfx.length()] = 0;
					h.set(header::CONTENT_TYPE, "octet/stream");
					h.set(header::LAST_MODIFIED, FAKEDATEMARK);
					h.set(header::CONTENT_LENGTH, probeStateWanted.size);
					ph = &h;
				}

#ifndef DEBUGIDX
				if(m_bVerbose)
#endif
					SendFmt << "Installing as " << path << ", state: " <<  probeStateWanted << hendl;


				if(FindCompIdx(path) < 0
						&& Inject(sPatchResultRel, path, true, ph, 0))
				{
					SyncSiblings(path, siblings);
				}
			}

			// patched, installed, DONE!
			return;
		}
	}
}

void cacheman::UpdateVolatileFiles()
{
	LOGSTARTFUNC

	string sErr; // for download error output

	// just reget them as-is and we are done. Also include non-index files, to be sure...
	if (m_bForceDownload)
	{
		SendChunk("<b>Bringing index files up to date...</b><br>\n");
		for (auto& f: m_metaFilesRel)
		{
			// nope... tell the fileitem to ignore file data instead ::truncate(SZABSPATH(it->first), 0);
			if (!Download(f.first, true, eMsgShow))
				m_nErrorCount += !m_metaFilesRel[f.first].forgiveDlErrors;
		}
		ERRMSGABORT;
		return;
	}

	tFileGroups idxGroups;

	auto dbgState = [&]() {
#ifdef DEBUGSPAM
	for (auto& f: m_metaFilesRel)
		SendFmt << "State of " << f.first << ": "
			<< f.second.toString();
#endif
	};

#ifdef DEBUGIDX
		auto dbgDump = [&](const char *msg, int pfx) {
			tSS printBuf;
			printBuf << "#########################################################################<br>\n"
					<< "## " <<  msg  << sBRLF
					<< "#########################################################################<br>\n";
			for(const auto& cp : idxGroups)
			{
				printBuf << pfx << ": cKEY: " << cp.first.toString() << hendl
						<< pfx << ": idxKey:"<<cp.second.diffIdxId.toString() << hendl
						<< pfx << ": Paths:" << hendl;
				for(const auto& path : cp.second.paths)
					printBuf << pfx << ":&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;"
					<< path << "&lt;=&gt;" << GetFlags(path).toString()
					<< hendl;
			}
			SendChunk(printBuf);
		};
#else
#define dbgDump(x,y)
#endif

	dbgState();

	MTLOGDEBUG("<br><br><b>STARTING ULTIMATE INTELLIGENCE</b><br><br>");

	if(m_bSkipIxUpdate)
	{
		SendFmt << "<span class=\"ERROR\">"
				"Warning: Online Activity is disabled, some update errors might be not recoverable without it"
				"<br></span>\n";
	}

	// this runs early with the state that is present on disk, before updating any file,
	// since it deals with the "reality" in the cache

	SendChunk("<b>Checking implicitly referenced files...</b><br>");

	/*
	 * Update all Release files
	 *
	 */
	tStrDeq goodReleaseFiles = GetGoodReleaseFiles();

	for(auto& sPathRel : goodReleaseFiles)
	{
		m_nErrorCount += !ProcessByHashReleaseFileRestoreFiles(sPathRel, "");
		if(m_nErrorCount)
			continue; // don't damage that copy

		if(!Download(sPathRel, true,
				m_metaFilesRel[sPathRel].hideDlErrors ? eMsgHideErrors : eMsgShow,
						tFileItemPtr(), 0, DL_HINT_GUESS_REPLACEMENT))
		{
			m_nErrorCount+=(!m_metaFilesRel[sPathRel].hideDlErrors);

			if(CheckStopSignal())
				return;

			continue;
		}
	}

	SendChunk("<b>Bringing index files up to date...</b><br>\n");

	{
		std::unordered_set<std::string> oldReleaseFiles;
		auto baseFolder = cfg::cacheDirSlash + cfg::privStoreRelSnapSufix;
		IFileHandler::FindFiles(baseFolder, [&baseFolder, &oldReleaseFiles, this](cmstring &sPath, const struct stat &st)
				-> bool {
			oldReleaseFiles.emplace(sPath.substr(baseFolder.size() + 1));
			return true;
		});

		if(!FixMissingByHashLinks(oldReleaseFiles))
		{
			SendFmt << "Error fixing by-hash links" << hendl;
			m_nErrorCount++;
			return;
		}
	}

	ExtractAllRawReleaseDataFixStrandedPatchIndex(idxGroups, goodReleaseFiles);

	dbgDump("After group building:", 0);

	if(CheckStopSignal())
		return;

	// OK, the equiv-classes map is built, now post-process the knowledge
	FilterGroupData(idxGroups);
	ERRMSGABORT;
	dbgDump("Refined (1):", 1);
	dbgState();

	SortAndInterconnectGroupData(idxGroups);

	// there was a quick check during data extraction but its context is lost now.
	// share it's knowledge between different members of the group
	for(auto& g: idxGroups)
		for(auto& p: g.second.paths)
			if(GetFlags(p).uptodate)
				SyncSiblings(p, g.second.paths);

	dbgDump("Refined (5):", 5);
	dbgState();
//	DelTree(SABSPATH("_actmp")); // do one to test the permissions

	// now do patching where possible
	for(auto& groupKV: idxGroups)
	{
		if(!groupKV.second.diffIdxId.valid())
			continue;
		// any of them should do the job
		auto& ipath = GetFirstPresentPath(idxGroups, groupKV.second.diffIdxId);
		if(ipath.empty())
			continue;
		mstring sPatchedPathRel;
		PatchOne(ipath, groupKV.second.paths);
	}
	dbgState();

	// semi-smart download of remaining files
	for(auto& groupKV: idxGroups)
	{
		for(auto& sfxFilter: sfxXzBz2GzLzmaNone)
		{
			for(auto& pathRel: groupKV.second.paths)
			{
//				bool holdon = StrHas(pathRel, "debrep/dists/experimental/contrib/source/Sources.xz");

				if(!endsWith(pathRel, sfxFilter))
					continue;

				auto &fl = GetRWFlags(pathRel);
#ifdef DEBUGIDX
				SendFmt << "Considering flags: " << pathRel << " " << fl.toString() << hendl;
#endif
				if(!fl.vfile_ondisk)
					continue;
				if(fl.parseignore)
					continue;
				if(!fl.uptodate)
				{
					if(!Download(pathRel, true, eMsgShow))
					{
						fl.parseignore = true;
						m_nErrorCount += !fl.forgiveDlErrors;
						continue;
					}
					// a failed download will be caught separately but for now, another download attempt is pointless
				}
				if(fl.uptodate)
					SyncSiblings(pathRel, groupKV.second.paths);
			}
		}
	}

	MTLOGDEBUG("<br><br><b>NOW GET THE REST</b><br><br>");

	// fetch all remaining stuff, at least the relevant parts
	for(auto& idx2att : m_metaFilesRel)
	{
		if (idx2att.second.uptodate || idx2att.second.parseignore)
			continue;
		if(!idx2att.second.vfile_ondisk || idx2att.second.eIdxType == EIDX_NOTREFINDEX)
			continue;
		string sErr;
		if(Download(idx2att.first, true,
				idx2att.second.hideDlErrors ? eMsgHideErrors : eMsgShow,
						tFileItemPtr(), 0, DL_HINT_GUESS_REPLACEMENT))
		{
			continue;
		}
		m_nErrorCount+=(!idx2att.second.forgiveDlErrors);
	}
}

void cacheman::SyncSiblings(cmstring &srcPathRel,const tStrDeq& targets)
{
	auto srcDirFile = SplitDirPath(srcPathRel);
	//auto srcType = GetTypeSuffix(srcDirFile.second);
	for(const auto& tgt: targets)
	{
		auto& flags = GetFlags(tgt);
		// not valid or it's us?
		if(!flags.vfile_ondisk || tgt == srcPathRel)
			continue;

		auto tgtDirFile = SplitDirPath(tgt);
		bool sameFolder = tgtDirFile.first == srcDirFile.first;

		// that's for sure, no matter what filename is, "us" is filtered
		if(sameFolder || !m_bByPath)
		{
			MTLOGDEBUG("Disabling use of " << tgt);
			SetFlags(tgt).parseignore = true;
		}

		if(!sameFolder && m_bByPath
			&& srcDirFile.second == tgtDirFile.second)
				//&& 0 == strcmp(srcType, GetTypeSuffix(targetDirFile.second)))
		{
			Inject(srcPathRel, tgt, true, 0, false);
		}
	}
}

cacheman::enumMetaType cacheman::GuessMetaTypeFromURL(const mstring& sPath)
{
	tStrPos pos = sPath.rfind(SZPATHSEP);
	string sPureIfileName = (stmiss == pos) ? sPath : sPath.substr(pos + 1);

	stripSuffix(sPureIfileName, ".gz");
	stripSuffix(sPureIfileName, ".bz2");
	stripSuffix(sPureIfileName, ".xz");
	stripSuffix(sPureIfileName, ".lzma");
	if (sPureIfileName=="Packages") // Debian's Packages file
		return EIDX_PACKAGES;

	if(endsWithSzAr(sPureIfileName, ".db") || endsWithSzAr(sPureIfileName, ".db.tar"))
		return EIDX_ARCHLXDB;

	if (sPureIfileName == "setup")
		return EIDX_CYGSETUP;

	if (sPureIfileName == "repomd.xml")
		return EIDX_SUSEREPO;

	if (sPureIfileName.length() > 50 && endsWithSzAr(sPureIfileName, ".xml") && sPureIfileName[40]
			== '-')
		return EIDX_XMLRPMLIST;

	if (sPureIfileName == "Sources")
		return EIDX_SOURCES;

	if (sPureIfileName == "Release" || sPureIfileName=="InRelease")
		return EIDX_RELEASE;

	if (sPureIfileName == sIndex)
		return endsWithSzAr(sPath, "i18n/Index") ? EIDX_TRANSIDX : EIDX_DIFFIDX;

	if (sPureIfileName == "MD5SUMS" && StrHas(sPath, "/installer-"))
		return EIDX_MD5DILIST;

	if (sPureIfileName == "SHA256SUMS" && StrHas(sPath, "/installer-"))
		return EIDX_SHA256DILIST;

	return EIDX_NOTREFINDEX;
}

bool cacheman::ParseAndProcessMetaFile(std::function<void(const tRemoteFileInfo&)> ret,
		const std::string &sPath,
		enumMetaType idxType, bool byHashMode)
{

	LOGSTART("expiration::ParseAndProcessMetaFile");

	// not error, just not supported
	if(idxType == EIDX_NOTREFINDEX)
		return true;

#ifdef DEBUG_FLAGS
	bool bNix=StrHas(sPath, "/i18n/");
#endif

	m_processedIfile = sPath;

	// full path of the directory of the processed index file with trailing slash
	string sBaseDir;
	// for some file types the main directory that parsed entries refer to
	// may differ, for some Debian index files for example
	string sPkgBaseDir;
	if(!CalculateBaseDirectories(sPath, idxType, sBaseDir, sPkgBaseDir))
	{
		m_nErrorCount++;
		SendFmt << "Unexpected index file without subdir found: " << sPath;
		return false;
	}

	filereader reader;

	if (!reader.OpenFile(SABSPATH(sPath), false, 1))
	{
		if(! GetFlags(sPath).forgiveDlErrors) // that would be ok (added by ignorelist), don't bother
		{
			tErrnoFmter err;
			SendFmt<<"<span class=\"WARNING\">WARNING: unable to open "<<sPath
					<<"(" << err << ")</span>\n<br>\n";
		}
		return false;
	}

	// some common variables
	mstring sLine, key, val;
	tRemoteFileInfo info;
	info.SetInvalid();
	tStrVec vsMetrics;
	string sStartMark;

	switch(idxType)
	{
	case EIDX_PACKAGES:
		LOG("filetype: Packages file");
		static const string sMD5sum("MD5sum"), sFilename("Filename"), sSize("Size");

		UrlUnescape(sPkgBaseDir);

		while(reader.GetOneLine(sLine))
		{
			trimBack(sLine);
			//cout << "file: " << *it << " line: "  << sLine<<endl;
			if (sLine.empty())
			{
				if(info.IsUsable())
					ret(info);
				info.SetInvalid();

				if(CheckStopSignal())
					return true; // XXX: should be rechecked by the caller ASAP!

				continue;
			}
			else if (ParseKeyValLine(sLine, key, val))
			{
				// not looking for data we already have
				if(key==sMD5sum)
					info.fpr.SetCs(val, CSTYPE_MD5);
				else if(key==sSize)
					info.fpr.size=atoofft(val.c_str());
				else if(key==sFilename)
				{
					UrlUnescape(val);
					info.sDirectory=sPkgBaseDir;
					tStrPos pos=val.rfind(SZPATHSEPUNIX);
					if(pos==stmiss)
						info.sFileName=val;
					else
					{
						info.sFileName=val.substr(pos+1);
						info.sDirectory.append(val, 0, pos+1);
					}
				}
			}
		}
		break;
	case EIDX_ARCHLXDB:
		LOG("assuming Arch Linux package db");
		{
			unsigned nStep = 0;
			enum tExpData
			{
				_fname, _csum, _csize, _nthng
			} typehint(_nthng);

			while (reader.GetOneLine(sLine)) // last line doesn't matter, contains tar's padding
			{
				trimLine(sLine);

				if (nStep >= 2)
				{
					if (info.IsUsable())
						ret(info);
					info.SetInvalid();
					nStep = 0;

					if (CheckStopSignal())
						return true;

					continue;
				}
				else if (endsWithSzAr(sLine, "%FILENAME%"))
					typehint = _fname;
				else if (endsWithSzAr(sLine, "%CSIZE%"))
					typehint = _csize;
				else if (endsWithSzAr(sLine, "%MD5SUM%"))
					typehint = _csum;
				else
				{
					switch (typehint)
					{
					case _fname:
						info.sDirectory = sBaseDir;
						info.sFileName = sLine;
						nStep = 0;
						break;
					case _csum:
						info.fpr.SetCs(sLine, CSTYPE_MD5);
						nStep++;
						break;
					case _csize:
						info.fpr.size = atoofft(sLine.c_str());
						nStep++;
						break;
					default:
						continue;
					}
					// next line is void for now
					typehint = _nthng;
				}
			}
		}
		break;
	case EIDX_CYGSETUP:
		LOG("assuming Cygwin package setup listing");

		while (reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			unsigned begin(0);
			if(startsWithSz(sLine, "install: "))
				begin=9;
			else if(startsWithSz(sLine, "source: "))
				begin=8;
			else
				continue;
			tSplitWalk split(&sLine, SPACECHARS, begin);
			if(split.Next() && info.SetFromPath(split, sPkgBaseDir)
					&& split.Next() && info.SetSize(split.remainder())
					&& split.Next() && info.fpr.SetCs(split))
			{
				ret(info);
			}
			info.SetInvalid();
		}
		break;
	case EIDX_SUSEREPO:
		LOG("SUSE pkg list file, entry level");
		while(reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			for(tSplitWalk split(&sLine, "\"'><=/"); split.Next(); )
			{
				cmstring tok(split);
				LOG("testing filename: " << tok);
				if(!endsWithSzAr(tok, ".xml.gz"))
					continue;
				LOG("index basename: " << tok);
				info.sFileName = tok;
				info.sDirectory = sBaseDir;
				ret(info);
				info.SetInvalid();
			}
		}
		break;
	case EIDX_XMLRPMLIST:
		LOG("XML based file list, pickup any valid filename ending in .rpm");
		while(reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			for(tSplitWalk split(&sLine, "\"'><=/"); split.Next(); )
			{
				cmstring tok(split);
				LOG("testing filename: " << tok);
				if (endsWithSzAr(tok, ".rpm")
						|| endsWithSzAr(tok, ".drpm")
						|| endsWithSzAr(tok, ".srpm"))
				{
					LOG("RPM basename: " << tok);
					info.sFileName = tok;
					info.sDirectory = sBaseDir;
					ret(info);
					info.SetInvalid();
				}
			}
		}
		break;
		// like http://ftp.uni-kl.de/debian/dists/jessie/main/installer-amd64/current/images/SHA256SUMS
	case EIDX_MD5DILIST:
	case EIDX_SHA256DILIST:
		LOG("Plain list of filenames and checksums");
		while(reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			tSplitWalk split(&sLine, SPACECHARS);
			info.fpr.size=-1;
			if( split.Next() && info.fpr.SetCs(split,
					idxType == EIDX_MD5DILIST ? CSTYPE_MD5 : CSTYPE_SHA256)
					&& split.Next() && (info.sFileName = split).size()>0)
			{
				info.sDirectory = sBaseDir;

				auto pos=info.sFileName.find_first_not_of("./");
				if(pos!=stmiss)
					info.sFileName.erase(0, pos);

				pos=info.sFileName.rfind(SZPATHSEPUNIX);
				if (stmiss!=pos)
				{
					info.sDirectory += info.sFileName.substr(0, pos+1);
					info.sFileName.erase(0, pos+1);
				}
				ret(info);
				info.SetInvalid();
			}
		}
		break;
	case EIDX_DIFFIDX:
		return ParseDebianRfc822Index(reader, ret, sBaseDir, sPkgBaseDir,
				EIDX_DIFFIDX, CSTYPES::CSTYPE_SHA256, "SHA256-Download", byHashMode);
	case EIDX_SOURCES:
		return ParseDebianRfc822Index(reader, ret, sBaseDir, sPkgBaseDir,
				EIDX_SOURCES, CSTYPES::CSTYPE_MD5, "Files", byHashMode);
	case EIDX_TRANSIDX:
		return ParseDebianRfc822Index(reader, ret, sBaseDir, sPkgBaseDir,
				EIDX_TRANSIDX, CSTYPES::CSTYPE_SHA1, "SHA1", byHashMode);
	case EIDX_RELEASE:
		if(byHashMode)
			return ParseDebianRfc822Index(reader, ret, sBaseDir, sPkgBaseDir,
					EIDX_RELEASE, CSTYPES::CSTYPE_INVALID, "", true);
		return ParseDebianRfc822Index(reader, ret, sBaseDir, sPkgBaseDir,
				EIDX_RELEASE, CSTYPES::CSTYPE_SHA256, "SHA256", false);
	default:
		SendChunk("<span class=\"WARNING\">"
				"WARNING: unable to read this file (unsupported format)</span>\n<br>\n");
		return false;
	}
	return reader.CheckGoodState(false);
}

bool cacheman::CalculateBaseDirectories(cmstring& sPath, enumMetaType idxType, mstring& sBaseDir, mstring& sPkgBaseDir)
{

	// full path of the directory of the processed index file with trailing slash
	sBaseDir.assign(SZPATHSEP);
	// for some file types the main directory that parsed entries refer to
	// may differ, for some Debian index files for example
	sPkgBaseDir.assign(sBaseDir);

	tStrPos pos = sPath.rfind(CPATHSEP);
	if(stmiss==pos)
		return false;
	sBaseDir.assign(sPath, 0, pos + 1);

	// does this look like a Debian archive structure? i.e. paths to other files have a base
	// directory starting in dists/?
	// The assumption doesn't however apply to the d-i checksum
	// lists, those refer to themselves only.
	//
	// similar considerations for Cygwin setup

	if (idxType != EIDX_MD5DILIST && idxType != EIDX_SHA256DILIST && stmiss != (pos =
			sBaseDir.rfind("/dists/")))
		sPkgBaseDir = sPkgBaseDir.assign(sBaseDir, 0, pos + 1);
	else if (idxType == EIDX_CYGSETUP && stmiss != (pos = sBaseDir.rfind("/cygwin/")))
		sPkgBaseDir = sPkgBaseDir.assign(sBaseDir, 0, pos + 8);
	else
		sPkgBaseDir = sBaseDir;

	return true;
}

void cacheman::ParseGenericRfc822File(filereader& reader,
		cmstring& sExtListFilter,
		map<string, deque<string> >& contents)
{
	string sLine, key, val, lastKey;
	deque<string>* pLastVal(nullptr);
	while (reader.GetOneLine(sLine))
	{
		if (sLine.empty())
			continue;

		if (isspace((unsigned) (sLine[0])))
		{
			if (!pLastVal)
				continue;

			// also skip if a filter is set for extended lists on specific key
			if (!sExtListFilter.empty() && sExtListFilter != lastKey)
				continue;

			trimFront(sLine);
			pLastVal->push_back(sLine);
		}
		else if (ParseKeyValLine(sLine, key, val))
		{
			// override the old key if existing, we don't merge
			auto ins = contents.insert(make_pair(key, deque<string>
			{ val }));
			lastKey = key;
			pLastVal = &ins.first->second;
		}
	}
}

bool cacheman::ParseDebianIndexLine(tRemoteFileInfo& info, cmstring& fline)
{
	info.sFileName.clear();
	// ok, read "checksum size filename" into info and check the word count
	tSplitWalk split(&fline);
	if (!split.Next()
			|| !info.fpr.SetCs(split, info.fpr.csType)
			|| !split.Next())
		return false;
	string val(split);
	info.fpr.size = atoofft((LPCSTR) val.c_str(), -2L);
	if (info.fpr.size < 0 || !split.Next())
		return false;
	UrlUnescapeAppend(split, info.sFileName);
	return true;
}

bool cacheman::ParseDebianRfc822Index(filereader& reader,
		std::function<void(const tRemoteFileInfo&)> &ret, cmstring& sBaseDir, cmstring& sPkgBaseDirConst,
		enumMetaType origIdxType, CSTYPES csType,
		cmstring& sExtListFilter, bool byHashMode)
{
	// beam the whole file into our model
	map< string,deque<string> > contents;
	ParseGenericRfc822File(reader, sExtListFilter, contents);
	mstring sSubDir;
	auto it = contents.find("Directory");
	if (it != contents.end() && !it->second.empty())
	{
		sSubDir = it->second.front();
		trimBack(sSubDir);
		sSubDir += sPathSep;
	}
	if(byHashMode)
	{
		it = contents.find("Acquire-By-Hash");
		if(contents.end() == it || it->second.empty() || it->second.front() != "yes")
			return true;
	}

	tRemoteFileInfo info;
	info.SetInvalid();
	info.fpr.csType = csType;

	mstring sPkgBaseDir;
	UrlUnescapeAppend(sPkgBaseDirConst, sPkgBaseDir);

	auto processList = [this, &ret, &info, &sSubDir, &sBaseDir,
						&sPkgBaseDir, &origIdxType](deque<string> fileList) -> void
			{
		uint32_t checkFilter(0); // don't do costly locking for every single line :-(
		for (auto& fline: fileList)
		{
			if(!(checkFilter++ & 0xff) && CheckStopSignal())
				return;
			if(!ParseDebianIndexLine(info, fline))
				continue;
			switch (origIdxType)
			{
			case EIDX_SOURCES:
				info.sDirectory = sPkgBaseDir + sSubDir;
				ret(info);
				break;
			case EIDX_TRANSIDX: // csum refers to the files as-is
			case EIDX_DIFFIDX:
				info.sDirectory = sBaseDir + sSubDir;
				ret(info);
				break;
			case EIDX_RELEASE:
			{
				info.sDirectory = sBaseDir + sSubDir;
				// usually has subfolder prefix, split and move into directory part
				auto pos = info.sFileName.rfind(SZPATHSEPUNIX);
				if (stmiss != pos)
				{
					info.sDirectory += info.sFileName.substr(0, (unsigned long) pos + 1);
					info.sFileName.erase(0, (unsigned long) pos + 1);
				}
				ret(info);
				break;
			}
			default:
				ASSERT(!"Originally determined type cannot reach this case!");
				break;
			}
		}
	};

	if(origIdxType == EIDX_RELEASE && byHashMode)
	{
		for(auto& cst: { CSTYPES::CSTYPE_MD5, CSTYPES::CSTYPE_SHA1,
			CSTYPES::CSTYPE_SHA256, CSTYPES::CSTYPE_SHA512 })
		{
			info.fpr.csType = cst;
			processList(contents[GetCsNameReleaseFile(cst)]);
		}
	}
	else
		processList(contents[sExtListFilter]);

	return reader.CheckGoodState(false);
}

void cacheman::ProcessSeenIndexFiles(std::function<void(tRemoteFileInfo)> pkgHandler)
{
	LOGSTARTFUNC
	for(auto& path2att: m_metaFilesRel)
	{
		if(CheckStopSignal())
			return;

		tIfileAttribs &att=path2att.second;
		enumMetaType itype = att.eIdxType;
		if(!itype) // default?
			itype=GuessMetaTypeFromURL(path2att.first);
		if(!itype) // still unknown/unsupported... Just ignore.
			continue;
		if(att.parseignore || (!att.vfile_ondisk && !att.uptodate))
			continue;

		/*
		 * Actually, all that information is available earlier when analyzing index classes.
		 * Could be implemented there as well and without using .bros pointer etc...
		 *
		 * BUT: what happens if some IO error occurs?
		 * Not taking this risk-> only skipping when file was processed correctly.
		 *
		 */

		if(!m_bByPath && att.alreadyparsed)
		{
			SendChunk(string("Skipping in ")+path2att.first+" (equivalent checks done before)<br>\n");
			continue;
		}

		//bool bNix=(it->first.find("experimental/non-free/binary-amd64/Packages.xz") != stmiss);

		SendChunk(string("Parsing metadata in ")+path2att.first+sBRLF);

		if( ! ParseAndProcessMetaFile(pkgHandler, path2att.first, itype))
		{
			if(!m_metaFilesRel[path2att.first].forgiveDlErrors)
			{
				m_nErrorCount++;
				SendChunk("<span class=\"ERROR\">An error occurred while reading this file, some contents may have been ignored.</span>\n");
				AddDelCbox(path2att.first, "Index data processing error");
				SendChunk(sBRLF);
			}
			continue;
		}
		else if(!m_bByPath)
		{
			att.alreadyparsed = true;
			for(auto next = att.bro; next != &att; next = next->bro)
			{
				next->alreadyparsed = true;
				MTLOGDEBUG("Marking sibling as processed");
			}
		}
	}
}

void cacheman::AddDelCbox(cmstring &sFileRel, cmstring& reason, bool bIsOptionalGuessedFile)
{
	mstring fileParm = AddLookupGetKey(sFileRel, reason.empty() ? mstring(" ") : reason);

	if(bIsOptionalGuessedFile)
	{
		string bn(GetBaseName(sFileRel));
		if(startsWithSz(bn, "/"))
			bn.erase(0, 1);
		SendFmtRemote <<  "<label><input type=\"checkbox\""
				<< fileParm << ">(also tag " << html_sanitize(bn) << ")</label><br>";
	}
	else
		SendFmtRemote <<  "<label><input type=\"checkbox\" "<< fileParm<<">Tag</label>"
			"\n<!--\n" maark << int(ControLineType::Error) << "Problem with "
			<< html_sanitize(sFileRel) << "\n-->\n";
}
void cacheman::TellCount(unsigned nCount, off_t nSize)
{
	SendFmt << sBRLF << nCount <<" package file(s) marked "
			"for removal in few days. Estimated disk space to be released: "
			<< offttosH(nSize) << "." << sBRLF << sBRLF;
}

mstring cacheman::AddLookupGetKey(cmstring &sFilePathRel, cmstring& errorReason)
{
	unsigned id = m_pathMemory.size();
	auto it = m_pathMemory.find(sFilePathRel);
	if(it==m_pathMemory.end())
		m_pathMemory[sFilePathRel] = {errorReason, id};
	else
		id = it->second.id;

	mstring ret(WITHLEN(" name=\"kf\" value=\""));
	ret +=to_base36(id);
	ret += "\"";
	return ret;
}

void cacheman::PrintStats(cmstring &title)
{
	multimap<off_t, cmstring*> sorted;
	off_t total=0;
	for(auto &f: m_metaFilesRel)
	{
		total += f.second.space;
		if(f.second.space)
			EMPLACE_PAIR_COMPAT(sorted,f.second.space, &f.first);
	}
	if(!total)
		return;
	int nMax = std::min(int(sorted.size()), int(MAX_TOP_COUNT));
	if(m_bVerbose)
		nMax = MAX_VAL(int);

	if(!m_bVerbose)
	{
	m_fmtHelper << "<br>\n<table name=\"shorttable\"><thead>"
			"<tr><th colspan=2>" << title;
	if(!m_bVerbose && sorted.size()>MAX_TOP_COUNT)
		m_fmtHelper << " (Top " << nMax << "<span name=\"noshowmore\">,"
				" <a href=\"javascript:show_rest();\">show more / cleanup</a></span>)";
	m_fmtHelper << "</th></tr></thead>\n<tbody>";
	for(auto it=sorted.rbegin(); it!=sorted.rend(); ++it)
	{
		m_fmtHelper << "<tr><td><b>"
				<< offttosH(it->first) << "</b></td><td>"
				<< *(it->second) << "</td></tr>\n";
		if(nMax--<=0)
			break;
	}
	SendFmt << "</tbody></table>"

	// the other is hidden for now
	<< "<div name=\"bigtable\" class=\"xhidden\">";
	}

	m_fmtHelper << "<br>\n<table><thead>"
				"<tr><th colspan=1><input type=\"checkbox\" onclick=\"copycheck(this, 'xfile');\"></th>"
				"<th colspan=2>" << title << "</th></tr></thead>\n<tbody>";
		for(auto it=sorted.rbegin(); it!=sorted.rend(); ++it)
		{
			m_fmtHelper << "<tr><td><input type=\"checkbox\" class=\"xfile\""
					<< AddLookupGetKey(*(it->second), "") << "></td>"
						"<td><b>" << html_sanitize(offttosH(it->first)) << "</b></td><td>"
					<< *(it->second) << "</td></tr>\n";


		}
		SendFmt << "</tbody></table>";

		if(m_pathMemory.empty())
		{
			SendFmtRemote << "<br><b>Action(s):</b><br>"
							"<input type=\"submit\" name=\"doDelete\""
							" value=\"Delete selected files\">";
			SendFmtRemote << BuildCompressedDelFileCatalog();
		}
		if(!m_bVerbose)
			SendFmt << "</div>";
}

#ifdef DEBUG
int parseidx_demo(LPCSTR file)
{

	class tParser : public cacheman
	{
	public:
		tParser() : cacheman({2, tSpecialRequest::workIMPORT, "doImport="}) {};
		inline int demo(LPCSTR file)
		{
			return !ParseAndProcessMetaFile([](const tRemoteFileInfo &entry) ->void {
				cout << "Dir: " << entry.sDirectory << endl << "File: " << entry.sFileName << endl
									<< "Checksum-" << GetCsName(entry.fpr.csType) << ": " << entry.fpr.GetCsAsString()
									<< endl;
				}, file, GuessMetaTypeFromURL(file));
		}
		virtual bool ProcessRegular(const mstring &, const struct stat &) override {return true;}
		virtual bool ProcessOthers(const mstring &, const struct stat &) override {return true;}
		virtual bool ProcessDirAfter(const mstring &, const struct stat &) override {return true;}
		virtual void Action() override {};
	}
	mgr;

	return mgr.demo(file);
}
#endif


void cacheman::ProgTell()
{
	if (++m_nProgIdx == m_nProgTell)
	{
		SendFmt<<"Scanning, found "<<m_nProgIdx<<" file"
				<< (m_nProgIdx>1?"s":"") << "...<br />\n";
		m_nProgTell*=2;
	}
}

bool cacheman::_checkSolidHashOnDisk(cmstring& hexname,
		const tRemoteFileInfo &entry,
		cmstring& srcPrefix
		)
{
	string solidPath = CACHE_BASE + entry.sDirectory.substr(srcPrefix.length()) + "by-hash/" +
				GetCsNameReleaseFile(entry.fpr.csType) + '/' + hexname;
	return ! ::access(solidPath.c_str(), F_OK);
}

void cacheman::BuildCacheFileList()
{
	//dump_proc_status();
	IFileHandler::DirectoryWalk(cfg::cachedir, this);
	//dump_proc_status();
}

bool cacheman::ProcessByHashReleaseFileRestoreFiles(cmstring& releasePathRel, cmstring& stripPrefix)
{
	int errors = 0;

	return ParseAndProcessMetaFile([this, &errors, &stripPrefix](const tRemoteFileInfo &entry) -> void
	{
		// ignore, those files are empty and are likely to report false positives
		if(entry.fpr.size < 29)
			return;

		auto hexname(BytesToHexString(entry.fpr.csum, GetCSTypeLen(entry.fpr.csType)));
		// ok, getting all hash versions...
		if(!_checkSolidHashOnDisk(hexname, entry, stripPrefix))
			return; // not for us

		auto wantedPathRel = entry.sDirectory.substr(stripPrefix.size())
				+ entry.sFileName;
		auto wantedPathAbs = SABSPATH(wantedPathRel);
#ifdef DEBUGIDX
		SendFmt << entry.sDirectory.substr(stripPrefix.size()) + "by-hash/" +
				GetCsNameReleaseFile(entry.fpr.csType) + '/' + hexname
				<< " was " << wantedPathAbs << hendl;
#endif
		Cstat wantedState(wantedPathAbs);
		string solidPathRel, solidPathAbs;
		// lazy construction for the check below
		if(!wantedState || wantedState.st_size != entry.fpr.size)
		{
			solidPathRel = entry.sDirectory.substr(stripPrefix.size()) + "by-hash/" +
									GetCsNameReleaseFile(entry.fpr.csType) + '/' + hexname;
			solidPathAbs = SABSPATH(solidPathRel);
		}

		bool contentMatch(false);
		// either target file is missing or is an older(?) version of different size
		// and our version fits better
		if(!wantedState || (wantedState.st_size != entry.fpr.size
				&& (contentMatch = entry.fpr.CheckFile(solidPathAbs))))
		{
			if(m_bVerbose)
				SendFmt << "Restoring virtual file " << wantedPathRel
					<< " (equal to " << solidPathRel << ")" << hendl;

			// return with increased count if error happens
			errors++;

			header h;
			// load by-hash header, check URL, rewrite URL, copy the stuff over
			if(!h.LoadFromFile(SABSPATH(solidPathRel) + ".head") || ! h.h[header::XORIG])
				{
				if(m_bVerbose)
					SendFmt << "Couldn't read " << SABSPATH(solidPathRel) << ".head<br>";
				return;
				}
			string origin(h.h[header::XORIG]);
			tStrPos pos = origin.rfind("by-hash/");
			if(pos == stmiss)
			{
			if(m_bVerbose)
				SendFmt << SABSPATH(solidPathRel) << " is not from by-hash folder<br>";
			return;
			}
			h.set(header::XORIG, origin.substr(0, pos) + entry.sFileName);
			// most servers report crap type on by-hash files, use generic one
			h.set(header::CONTENT_TYPE, "octet/stream");
			//	should be ok				h.set(header::CONTENT_LENGTH, entry.fpr.size)
			if(!Inject(solidPathRel, wantedPathRel, false, &h, false))
			{
			if(m_bVerbose)
				SendChunk("Couldn't install<br>");
			return;
			}
			auto& flags = SetFlags(wantedPathRel);
			if(flags.vfile_ondisk)
				flags.uptodate = true;

			errors--;
		}
	},
	stripPrefix + releasePathRel, enumMetaType::EIDX_RELEASE, true) && errors == 0;
}

bool cacheman::FixMissingByHashLinks(std::unordered_set<std::string> &oldReleaseFiles)
{
	bool ret = true;

	// path of side store with trailing slash relativ to cache folder
	auto srcPrefix(cfg::privStoreRelSnapSufix + sPathSep);

	for(const auto& snapPathInXstore: oldReleaseFiles)
	{
		if(endsWithSzAr(snapPathInXstore, ".upgrayedd"))
			continue;
		// path relative to cache folder
		if(!ProcessByHashReleaseFileRestoreFiles(snapPathInXstore, srcPrefix))
		{
			SendFmt << "There were error(s) processing " << snapPathInXstore << ", ignoring..."<< hendl;
			if(!m_bVerbose)
				SendChunk("Enable verbosity to see more");
			return ret;
		}
#ifdef DEBUGIDX
		SendFmt << "Purging " << SABSPATH(srcPrefix + snapPathInXstore) << hendl;
#endif
		unlink(SABSPATH(srcPrefix + snapPathInXstore).c_str());
	}
	return ret;
}

tStrDeq cacheman::GetGoodReleaseFiles()
{
	tStrMap t;
	for (const auto& kv : m_metaFilesRel)
	{
		bool inr;
		if(endsWith(kv.first, inRelKey))
			inr=true;
		else if(endsWith(kv.first, relKey))
			inr=false;
		else
			continue;
		if(!kv.second.vfile_ondisk)
			continue;
		auto df=SplitDirPath(kv.first);
		string& fn = t[df.first];

		if(inr) // always wins
		{
			if(!fn.empty()) // there was Release already... crap
				SetFlags(kv.first).parseignore = true;
			fn = df.second;
		}
		else
		{
			if(fn.empty())
				fn = df.second;
			else
				SetFlags(kv.first).parseignore = true;
		}
	}
	tStrDeq ret;
	for(const auto& kv: t) ret.emplace_back(kv.first+kv.second);
	return ret;
}

}
