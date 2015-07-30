
//#define LOCAL_DEBUG
#include "debug.h"

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
#include <string>
#include <iostream>
#include <algorithm>

#include <unistd.h>

#ifdef DEBUG
#define DEBUGIDX
#warning enable, and it will spam a lot!
//#define DEBUGSPAM
#endif

#define MAX_TOP_COUNT 10

using namespace std;

static cmstring oldStylei18nIdx("/i18n/Index");
static cmstring diffIdxSfx(".diff/Index");
static cmstring sConstDiffIdxHead(".diff/Index.head");
static cmstring sPatchBaseRel("_actmp/patch.base");
static cmstring sPatchResRel("_actmp/patch.result");
static const string relKey("/Release"), inRelKey("/InRelease");
static unsigned int nKillLfd=1;
time_t m_gMaintTimeNow=0;

tCacheOperation::tCacheOperation(const tSpecialRequest::tRunParms& parms) :
	tSpecOpDetachable(parms),
	m_bErrAbort(false), m_bVerbose(false), m_bForceDownload(false),
	m_bScanInternals(false), m_bByPath(false), m_bByChecksum(false), m_bSkipHeaderChecks(false),
	m_bTruncateDamaged(false),
	m_nErrorCount(0),
	m_nProgIdx(0), m_nProgTell(1), m_pDlcon(nullptr)
{
	m_szDecoFile="maint.html";
	m_gMaintTimeNow=GetTime();

}

tCacheOperation::~tCacheOperation()
{
	delete m_pDlcon;
	m_pDlcon=nullptr;
}

bool tCacheOperation::ProcessOthers(const string &, const struct stat &)
{
	// NOOP
	return true;
}

bool tCacheOperation::ProcessDirAfter(const string &, const struct stat &)
{
	// NOOP
	return true;
}


bool tCacheOperation::AddIFileCandidate(const string &sPathRel)
{
	if(sPathRel.empty())
		return false;

	enumMetaType t;
	if ( (rechecks::FILE_VOLATILE == rechecks::GetFiletype(sPathRel)
	// SUSE stuff, not volatile but also contains file index data
	|| endsWithSzAr(sPathRel, ".xml.gz") )
	&& (t=GuessMetaTypeFromURL(sPathRel)) != EIDX_UNSUPPORTED)
	{
		tIfileAttribs & atts=m_metaFilesRel[sPathRel];
		atts.vfile_ondisk=true;
		atts.eIdxType=t;

		return true;
    }
	return false;
}

// defensive getter/setter methods, don't create non-existing entries
const tCacheOperation::tIfileAttribs & tCacheOperation::GetFlags(cmstring &sPathRel) const
{
	auto it=m_metaFilesRel.find(sPathRel);
	if(m_metaFilesRel.end()==it)
		return attr_dummy_pure;
	return it->second;
}
tCacheOperation::tIfileAttribs &tCacheOperation::SetFlags(cmstring &sPathRel)
{
	ASSERT(!sPathRel.empty());
	return sPathRel.empty() ? attr_dummy : m_metaFilesRel[sPathRel];
}

// detects when an architecture has been removed entirely from the Debian archive
bool tCacheOperation::IsDeprecatedArchFile(cmstring &sFilePathRel)
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


bool tCacheOperation::Download(cmstring& sFilePathRel, bool bIsVolatileFile,
		tCacheOperation::eDlMsgPrio msgVerbosityLevel,
		tFileItemPtr pFi, const tHttpUrl * pForcedURL, unsigned hints)
{

	LOGSTART("tCacheMan::Download");

	mstring sErr;
	bool bSuccess=false;
#ifdef DEBUG_FLAGS
	bool nix=StrHas(sFilePathRel, "debrep/dists/experimental/non-free/debian-installer/binary-mips/Packages.gz");
#endif

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

	const acfg::tRepoData *pRepoDesc=nullptr;
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
		fiaccess.PrepageRegisteredFileItemWithStorage(sFilePathRel, false);
		pFi=fiaccess.get();
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
		if(!acfg::stupidfs && bCachePathAsUriPlausible
				&& 0 != (pRepoDesc = acfg::GetRepoData(parserPath.sHost))
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

			if(bCachePathAsUriPlausible) // unless other rule matches, this path in cache should represent the remote URI
				pResolvedDirectUrl = parserPath.NormalizePath();

			// and prefer the source from xorig which is likely to deliver better result
			if(hor.h[header::XORIG] && parserHead.SetHttpUrl(hor.h[header::XORIG], false))
				pResolvedDirectUrl = parserHead.NormalizePath();
			else if(flags.guessed)
			{
				// might use a related file as reference

				auto pos = sFilePathAbs.rfind(".diff/");
				if (pos != stmiss)
				{
					// ok, that's easy, looks like getting patches and the
					// .diff/Index must be already there
					auto xpath = sFilePathAbs.substr(0, pos) + sConstDiffIdxHead;
					if (hor.LoadFromFile(xpath) > 0 && hor.h[header::XORIG])
					{
						xpath = hor.h[header::XORIG];
						// got the URL of the original .diff/Index file?
						ldbg("sample url is " << xpath);
						if (endsWith(xpath, diffIdxSfx))
						{
							// yes, it is, replace the ending with the local part of the filename
							xpath.erase(xpath.size() - diffIdxSfx.size());
							xpath += sFilePathAbs.substr(pos);
							if (parserHead.SetHttpUrl(xpath, false))
								pResolvedDirectUrl = parserHead.NormalizePath();
						}
					}
				}
				else
				{
					// usecase: getting a non-pdiff file
					// potential neighbors? something like:
					static cmstring testsfxs[] =
					{ ".diff/Index", ".bz2", ".gz", ".xz", ".lzma" };

					// First, getting a "native" base path of that file, therefore
					// chop of the compression suffix and append foo while testing
					//
					// after resolving, chop of foo from the URL and add the
					// comp.suffix again
					//
					cmstring * pCompSuf = nullptr;
					auto xBasePath = sFilePathAbs;
					for (auto& testCompSuf : compSuffixesAndEmpty)
					{
						if (endsWith(xBasePath, testCompSuf))
						{
							pCompSuf = &testCompSuf;
							xBasePath.erase(xBasePath.size()-testCompSuf.size());
							break;
						}
					}

					for (auto& foo : testsfxs)
					{
						ldbg("get example url from header of " << xBasePath);
						if (pCompSuf && hor.LoadFromFile(xBasePath + foo + ".head") > 0 && hor.h[header::XORIG])
						{
							string urlcand = hor.h[header::XORIG];
							if (endsWith(urlcand, foo))
							{
								// ok, looks plausible
								ldbg("sample url is " << urlcand);
								urlcand.erase(urlcand.size() - foo.size());
								urlcand += *pCompSuf;
								if (parserHead.SetHttpUrl(urlcand, false))
								{
									pResolvedDirectUrl = parserHead.NormalizePath();
									break;
								}
							}
						}
					}
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
		acfg::tRepoResolvResult repinfo;
		acfg::GetRepNameAndPathResidual(*pResolvedDirectUrl, repinfo);
		auto hereDesc = repinfo.repodata;
		if(repinfo.repodata && !repinfo.repodata->m_backends.empty())
		{
			pResolvedDirectUrl = nullptr;
			pRepoDesc = hereDesc;
			sRemoteSuffix = repinfo.sRestPath;
		}
	}

	m_pDlcon->AddJob(pFi, pResolvedDirectUrl, pRepoDesc, &sRemoteSuffix, 0);

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
			// for the removal of .bz2 compressed versions
			// of various files
			if (endsWithSzAr(sFilePathRel, "/Packages.bz2")
					|| endsWithSzAr(sFilePathRel, "/Sources.bz2")
					|| (StrHas(sFilePathRel, "i18n/Translation-") &&
							endsWithSzAr(sFilePathRel, ".bz2")))

			{
				SendChunkSZ("Attempting to download the .xz version... ");
				// if we have it already, use it as-is
				if (!pResolvedDirectUrl)
				{
					auto p = pFi->GetHeaderUnlocked().h[header::XORIG];
					if (p && parserHead.SetHttpUrl(p))
						pResolvedDirectUrl = &parserHead;
				}
				if (pResolvedDirectUrl)
				{
					// XXX: maybe pointless copy, work around with a flag?
					auto newurl(*pResolvedDirectUrl);
					if (endsWithSzAr(newurl.sPath, ".bz2"))
					{
						newurl.sPath.erase(newurl.sPath.size() - 4);
						newurl.sPath.append(".xz");
						if(Download(sFilePathRel.substr(0, sFilePathRel.size() - 4) + ".xz",
								bIsVolatileFile, msgVerbosityLevel, tFileItemPtr(), &newurl))
						{
							MarkObsolete(sFilePathRel);
							return true;
						}
						return false;
					}
				}
			}
		}

		if (sErr.empty())
			sErr = "Download error";
		if (NEEDED_VERBOSITY_EVERYTHING || m_bVerbose)
		{
#ifndef DEBUG
			if(!flags.hideDlErrors)
#endif
			{
				SendFmt << "<span class=\"ERROR\">" << sErr << "</span>\n";
				AddDelCbox(sFilePathRel);
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
	for(auto &p : compSuffixes)
		if(endsWith(s, p))
			return(s.size()-p.size());
	return stmiss;
}
static unsigned short FindCompIdx(cmstring &s)
{
	unsigned short i=0;
	for(;i<_countof(compSuffixes); i++)
		if(endsWith(s, compSuffixes[i]))
			return i;
	return i;
}

static const string dis("/binary-");

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
	DirectoryWalk(what, &hh, false, false);
}

struct fctLessThanCompMtime
{
	string m_base;
	fctLessThanCompMtime(const string &base) :
		m_base(base)
	{
	}
	bool operator()(const string &s1, const string &s2) const
	{
		struct stat stbuf1, stbuf2;
		tStrPos cpos1(FindCompSfxPos(s1) ), cpos2(FindCompSfxPos(s2));
		if(cpos1!=cpos2)
			return cpos1 > cpos2; // sfx found -> less than npos (=w/o sfx) -> be smaller
		// s1 is lesser when its newer
		if (::stat((m_base + s1).c_str(), &stbuf1))
			stbuf1.st_mtime = 0;
		if (::stat((m_base + s2).c_str(), &stbuf2))
			stbuf2.st_mtime = 0;
		return stbuf1.st_mtime > stbuf2.st_mtime;
	}
};

struct tCompByState : public tFingerprint
{
	tCompByState(const tFingerprint &re) : tFingerprint(re) {}
	bool operator()(const tPatchEntry &other) const { return other.fprState == *this; }
};

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

	uint peAnz(0);

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


bool tCacheOperation::PatchFile(const string &srcRel,
		const string &diffIdxPathRel, tPListConstIt pit, tPListConstIt itEnd,
		const tFingerprint *verifData)
{
	if(m_bVerbose)
		SendFmt<< "Patching from " << srcRel << " via " << diffIdxPathRel << "...<br>\n";

	string sFinalPatch(CACHE_BASE+"_actmp/combined.diff");
	if(diffIdxPathRel.length()<=diffIdxSfx.length())
		return false; // just be sure about that

	FILE_RAII pf;
	for(;pit!=itEnd; pit++)
	{
		string pfile(diffIdxPathRel.substr(0, diffIdxPathRel.size()-diffIdxSfx.size()+6)
				+pit->patchName+".gz");
		auto& flags=SetFlags(pfile);
		flags.guessed=true;
		flags.hideDlErrors=true;
		if(!Download(pfile, false, eMsgShow))
		{
			m_metaFilesRel.erase(pfile); // remove the mess for sure
			SendFmt << "Failed to download patch file " << pfile << " , stop patching...<br>";
			return false;
		}

		if(CheckStopSignal())
			return false;

		SetFlags(pfile).parseignore=true; // not an ifile, never parse this
		::mkbasedir(sFinalPatch);
		if(!pf.p && ! (pf.p=fopen(sFinalPatch.c_str(), "w+")))
		{
			SendChunk("Failed to create intermediate patch file, stop patching...<br>");
			return false;
		}
		tFingerprint probe;
		if(!probe.ScanFile(CACHE_BASE+pfile, CSTYPE_SHA1, true, pf.p))
		{

			if(CheckStopSignal())
				return false;

			if(m_bVerbose)
				SendFmt << "Failure on checking of intermediate patch data in " << pfile << ", stop patching...<br>";
			return false;
		}
		if ( ! (probe == pit->fprPatch))
		{
			SendFmt<< "Bad patch data in " << pfile <<" , stop patching...<br>";
			return false;
		}
	}
	if(pf.p)
	{
		::fprintf(pf.p, "w patch.result\n");
		::fflush(pf.p); // still a slight risk of file closing error but it's good enough for now
		if(::ferror(pf.p))
		{
			SendChunk("Patch merging error<br>");
			return false;
		}
		checkForceFclose(pf.p);
	}

	if(m_bVerbose)
		SendChunk("Patching...<br>");

	tSS cmd;
	cmd << "cd '" << CACHE_BASE << "_actmp' && ";
	auto act = acfg::suppdir + SZPATHSEP "acngtool";
	if(!acfg::suppdir.empty() && 0==access(act.c_str(), X_OK))
		cmd << "'" << act << "' patch patch.base " << sFinalPatch << " patch.result";
	else
		cmd << " red --silent patch.base < " << sFinalPatch;

	if (::system(cmd.c_str()))
	{
		MTLOGASSERT(false, "Command failed: " << cmd);
		return false;
	}

	tFingerprint probe;
	string respathAbs = CACHE_BASE + sPatchResRel;
	if (!probe.ScanFile(respathAbs, CSTYPE_SHA1, false))
	{
		MTLOGASSERT(false, "Scan failed: " << respathAbs);
		return false;
	}

	if(verifData && probe != *verifData)
	{
		MTLOGASSERT(false,"Verification failed, against: " << respathAbs);
		return false;
	}
	return true;
}

bool tCacheOperation::GetAndCheckHead(cmstring & sTempDataRel, cmstring &sReferencePathRel,
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



bool tCacheOperation::Inject(cmstring &from, cmstring &to,
		bool bSetIfileFlags, const header *pHead, bool bTryLink)
{
	LOGSTART("tCacheMan::Inject");

	// XXX should it really filter it here?
	if(GetFlags(to).uptodate)
		return true;

	auto sAbsFrom(SABSPATH(from)), sAbsTo(SABSPATH(to));

	Cstat infoFrom(sAbsFrom), infoTo(sAbsTo);
	if(infoFrom && infoTo && infoFrom.st_ino == infoTo.st_ino && infoFrom.st_dev == infoTo.st_dev)
		return true;

#ifdef DEBUG_FLAGS
	bool nix = stmiss!=from.find("debrep/dists/squeeze/non-free/binary-amd64/");
	SendFmt<<"Replacing "<<to<<" with " << from <<  "<br>\n";
#endif

	header head;

	if (!pHead)
	{
		pHead = &head;

		if (head.LoadFromFile(sAbsFrom+".head") <= 0 || !head.h[header::CONTENT_LENGTH])
		{
			MTLOGASSERT(0, "Cannot read " << from << ".head or bad data");
			return false;
		}
		if(GetFileSize(SABSPATH(from), -1) != atoofft(head.h[header::CONTENT_LENGTH]))
		{
			MTLOGASSERT(0, "Bad file size");
			return false;
		}
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
	auto pfi(make_shared<tInjectItem>(to, bTryLink));
	// register it in global scope
	fileItemMgmt fi;
	if(!fi.RegisterFileItem(pfi))
	{
		MTLOGASSERT(false, "Couldn't register copy item");
		return false;
	}
	bool bOK = pfi->Inject(from, *pHead);

	MTLOGASSERT(bOK, "Inject: failed");

	if(bSetIfileFlags)
	{
		tIfileAttribs &atts = SetFlags(to);
		atts.uptodate = atts.vfile_ondisk = bOK;
	}

	return bOK;
}


bool tCacheOperation::Propagate(cmstring &donorRel, tContId2eqClass::iterator eqClassIter,
		cmstring *psTmpUnpackedAbs)
{
#ifdef DEBUG
	SendFmt<< "Installing " << donorRel << "<br>\n";
	bool nix=StrHas(donorRel, "debrep/dists/experimental/main/binary-amd64/Packages");
#endif

	const tStrDeq &tgts = eqClassIter->second.paths;

	// we know it's uptodate, make sure to avoid attempts to modify it in background
	fileItemMgmt src;
	if(GetFlags(donorRel).uptodate)
	{
		if(!src.PrepageRegisteredFileItemWithStorage(donorRel, false))
			return false;

		src.get()->Setup(false);
		// something changed it in meantime?!
		if (src.get()->GetStatus() != fileitem::FIST_COMPLETE)
			return false;
	}

	int nInjCount=0;
	for (const auto& tgtCand : tgts)
	{
		if(donorRel == tgtCand)
			continue;
		const tIfileAttribs &flags=GetFlags(tgtCand);
		// exists somewhere else and same compression type? -> replace it
		if(FindCompIdx(donorRel) == FindCompIdx(tgtCand))
		{
			if (flags.vfile_ondisk)
			{
				// counts fresh file as injected, no need to recheck them in Inject()
				if (flags.uptodate || Inject(donorRel, tgtCand))
					nInjCount++;
				else
					MTLOGASSERT(false, "Inject failed");
			}
#if 0 // little hack to make repairs faster (bz2 to xz) in case where there is an extra copy
			// actually it's overkill for what it's good for,
			// the alternative version download attempt
			// fixes the issue good enough
			else
			{
				// however, if it has bros lying around that are no longer
				// tracked by upstream index then force its installation!
				auto cpos=FindCompSfxPos(tgtCand);
				string sBasename=tgtCand.substr(0, cpos);
				for(auto& sfx : compSuffixesAndEmpty)
				{
					if(endsWith(tgtCand, sfx)) // same file, checked before
						continue;
					auto probeOtherSfx = sBasename+sfx;
					if(!GetFlags(probeOtherSfx).vfile_ondisk)
						continue;
					// HIT! Install a copy and break
					if (Inject(donorRel, tgtCand))
					{
						SendFmt << "<span class=\"WARNING\">"
								"Warning: added a new file "
								<< tgtCand << " from another source "
								"because upstream index apparently "
								"stopped listing the "
								<< probeOtherSfx <<
								"version</span><br>\n";
						nInjCount++;
						SetFlags(tgtCand) = flags;
					}
					break;
				}
			}
#endif
		}
	}

	// defuse some stuff located in the same directory, like .gz variants of .bz2 files
	// and this REALLY means: ONLY THE SAME DIRECTORY!
	// it disables alternative variants with different compressions
	// and shall not affect the strict-path checking later
	for (const auto& tgt: tgts)
	{
#ifdef DEBUG_EXTRA
	SendFmt<< "Bro check for " << tgt << "<br>\n";
#endif
		auto& myState = GetFlags(tgt);
		if(!myState.vfile_ondisk || !myState.uptodate || myState.parseignore)
			continue;

		tStrPos cpos=FindCompSfxPos(tgt);
		string sBasename=tgt.substr(0, cpos);
		string sux=cpos==stmiss ? "" : tgt.substr(FindCompSfxPos(tgt));
		for(auto& compsuf : compSuffixesAndEmpty)
		{
			if(sux==compsuf) continue; // touch me not
			mstring sBro = sBasename+compsuf;
#ifdef DEBUG_EXTRA
	SendFmt<< "Search bro: " << sBro << "<br>\n";
#endif
			auto kv=m_metaFilesRel.find(sBro);
			if(kv!=m_metaFilesRel.end() && kv->second.vfile_ondisk)
			{
				kv->second.parseignore=true; // gotcha
				MTLOGDEBUG("Defused bro of " << tgt << ": " << sBro);

				// also, we don't care if the pdiff index vanished for some reason
				kv = m_metaFilesRel.find(sBasename+".diff/Index");
				if(kv!=m_metaFilesRel.end())
					kv->second.forgiveDlErrors=true;

				// if we have a freshly unpacked version and bro should be the the same
				// and bro is older than the donor file for some reason... update bro!
				if (psTmpUnpackedAbs && compsuf.empty())
				{
					struct stat stbuf;
					time_t broModTime;

					if (0 == ::stat(SZABSPATH(sBro), &stbuf)
							&& (broModTime = stbuf.st_mtime, 0 == ::stat(SZABSPATH(donorRel), &stbuf))
							&& broModTime < stbuf.st_mtime)
					{
						MTLOGDEBUG("Unpacked version in " << sBro << " too old, replace from "
								<< *psTmpUnpackedAbs);
						FileCopy(*psTmpUnpackedAbs, SZABSPATH(sBro));
					}
				}
			}
		}
	}

	if(!nInjCount && endsWith(donorRel, sPatchResRel))
	{
		/*
		 * Now that's a special case, the file was patched and
		 * we need to store the latest state somewhere. But there
		 * was no good candidate to keep that copy. Looking closer for
		 * some appropriate location.
		 * */
		string sLastRessort;
		for (auto& tgt : tgts)
		{
			if(stmiss!=FindCompSfxPos(tgt))
				continue;
			// ultimate ratio... and then use the shortest path
			if(sLastRessort.empty() || sLastRessort.size() > tgt.size())
				sLastRessort=tgt;
		}
		Inject(donorRel, sLastRessort);
	}

	return true;
}

void tCacheOperation::StartDlder()
{
	if (!m_pDlcon)
		m_pDlcon = new dlcon(true);
}

void tCacheOperation::UpdateVolatileFiles()
{
	LOGSTART("expiration::UpdateVolatileFiles()");

	SendChunk("<b>Bringing index files up to date...</b><br>\n");

	string sErr; // for download error output
	const string sPatchBaseAbs=CACHE_BASE+sPatchBaseRel;
	mkbasedir(sPatchBaseAbs);

	tContId2eqClass& eqClasses = m_eqClasses;
	// just reget them as-is and we are done
	if (m_bForceDownload)
	{
		for (auto& f: m_metaFilesRel)
		{
			// nope... tell the fileitem to ignore file data instead ::truncate(SZABSPATH(it->first), 0);
			if (!Download(f.first, true, eMsgShow))
				m_nErrorCount += !m_metaFilesRel[f.first].forgiveDlErrors;
		}
		ERRMSGABORT;
		return;
	}


	auto dbgState = [&]() {
#ifdef DEBUGSPAM
	for (auto& f: m_metaFilesRel)
		SendFmt << "State of " << f.first << ": "
			<< f.second.alreadyparsed << "|"
			<< f.second.forgiveDlErrors << "|"
			<< f.second.hideDlErrors << "|"
			<< f.second.parseignore << "|"
			<< f.second.space << "|"
			<< f.second.uptodate << "|"
			<< f.second.vfile_ondisk << "|"
			<< f.second.guessed << "|<br>\n";
#endif
	};
	dbgState();
	typedef unordered_map<string, tContId> tFile2Cid;

	MTLOGDEBUG("<br><br><b>STARTING ULTIMATE INTELLIGENCE</b><br><br>");

	/*
	 * Update all Release files
	 *
	 */
	class releaseStuffReceiver : public ifileprocessor
	{
		public:
			tFile2Cid m_file2cid;
			virtual void HandlePkgEntry(const tRemoteFileInfo &entry)
			{
				if(entry.bInflateForCs) // XXX: no usecase yet, ignore
					return;

				tStrPos compos=FindCompSfxPos(entry.sFileName);

				// skip some obvious junk and its gzip version
				if(0==entry.fpr.size || (entry.fpr.size<33 && stmiss!=compos))
					return;

				auto& cid = m_file2cid[entry.sDirectory+entry.sFileName];
				cid.first=entry.fpr;

				tStrPos pos=entry.sDirectory.rfind(dis);
				// if looking like Debian archive, keep just the part after binary-...
				if(stmiss != pos)
					cid.second=entry.sDirectory.substr(pos)+entry.sFileName;
				else
					cid.second=entry.sFileName;
			}
	};

	// little helper to pick only one of Release OR InRelease from the same directory
	tStrMap releaseFilesUniq;
	for (auto sfx :
	{ inRelKey, relKey })
	{
		for (auto& iref : m_metaFilesRel)
		{
			auto& sPathRel = iref.first;
			if (endsWith(sPathRel, sfx))
			{
				auto pos = sPathRel.rfind('/');
				auto& fname = releaseFilesUniq[sPathRel.substr(0, pos)];
				if (fname.empty())
					fname = sPathRel.substr(pos);
			}
		}
	}

	// iterate over initial *Releases files
	for(auto& relRef : releaseFilesUniq)
	{
		cmstring sPathRel=relRef.first+relRef.second;

		if(!Download(sPathRel, true, m_metaFilesRel[sPathRel].hideDlErrors
					? eMsgHideErrors : eMsgShow))
		{
			m_nErrorCount+=(!m_metaFilesRel[sPathRel].hideDlErrors);

			if(CheckStopSignal())
				return;

			continue;
		}

		m_metaFilesRel[sPathRel].uptodate=true;
#ifdef DEBUG
		SendFmt << "Start parsing " << sPathRel << "<br>";
#endif
		releaseStuffReceiver recvr;
		ParseAndProcessMetaFile(recvr, sPathRel, EIDX_RELEASE);

		if(recvr.m_file2cid.empty())
			continue;

		// first, look around for for .diff/Index files on disk and prepare their processing
		for(const auto cid : recvr.m_file2cid)
		{
			// not diff index or not in cache?
			if(!endsWith(cid.first, diffIdxSfx) || !GetFlags(cid.first).vfile_ondisk)
				continue;
			// found an ...diff/Index file!
			string sBase=cid.first.substr(0, cid.first.size()-diffIdxSfx.size());
			for(auto& suf : compSuffixesAndEmptyByRatio)
			{
				const auto& flags=GetFlags(sBase+suf);
				if(flags.vfile_ondisk)
					goto has_base; // break 2 levels
			}
			// ok, not found, enforce dload of any existing patch base file?
			for(auto& suf : compSuffixesAndEmptyByRatio)
			{
				if(ContHas(recvr.m_file2cid, (sBase+suf)))
				{
					SetFlags(sBase+suf).guessed=true;
					LOG("enforcing dl: " << (sBase+suf));
					break;
				}
			}
has_base:
			;
		}
		dbgState();

		// now refine all extracted information and store it in eqClasses for later processing
		for(auto if2cid : recvr.m_file2cid)
		{
			string sNativeName=if2cid.first.substr(0, FindCompSfxPos(if2cid.first));
			tContId sCandId=if2cid.second;
			// find a better one which serves as the flag content id for the whole group
			for(auto& ps : compSuffixesAndEmptyByLikelyhood)
			{
				auto it2=recvr.m_file2cid.find(sNativeName+ps);
				if(it2 != recvr.m_file2cid.end())
					sCandId=it2->second;
			}
			tClassDesc &tgt=eqClasses[sCandId];
			tgt.paths.emplace_back(if2cid.first);

			// pick up the id for bz2 verification later
			if(tgt.bz2VersContId.second.empty() && endsWithSzAr(if2cid.first, ".bz2"))
				tgt.bz2VersContId=if2cid.second;

			// also the index file id
			if(tgt.diffIdxId.second.empty()) // XXX if there is no index at all, avoid repeated lookups somehow?
			{
				auto j = recvr.m_file2cid.find(sNativeName+diffIdxSfx);
				if(j!=recvr.m_file2cid.end())
					tgt.diffIdxId=j->second;
			}

			// and while we are at it, check the checksum of small files in order
			// to reduce server request count
			if(if2cid.second.first.size<10000 && ContHas(m_metaFilesRel, if2cid.first))
			{
				if(if2cid.second.first.CheckFile(SABSPATH(if2cid.first)))
					m_metaFilesRel[if2cid.first].uptodate=true;
			}
		}
	}

#ifdef DEBUGIDX
	auto dbgDump = [&](const char *msg, int pfx) {
		tSS printBuf;
		printBuf << "#########################################################################<br>\n"
			<< "## " <<  msg  << "<br>\n"
			<< "#########################################################################<br>\n";
		for(const auto& cp : eqClasses)
		{
			printBuf << pfx << ": TID: "
				<<  cp.first.first<<cp.first.second<<"<br>\n" << pfx << ": "
				<< "bz2TID:" << cp.second.bz2VersContId.first
				<< cp.second.bz2VersContId.second<<"<br>\n" << pfx << ": "
				<< "idxTID:"<<cp.second.diffIdxId.first 
				<< cp.second.diffIdxId.second <<"<br>\n" << pfx << ": "
				<< "Paths:<br>\n";
			for(const auto& path : cp.second.paths)
				printBuf << pfx << ":&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;" << path <<"<br>\n";
		}
		SendChunk(printBuf);
	};
#else
#define dbgDump(x,y)
#endif

	dbgDump("After class building:", 0);

	if(CheckStopSignal())
		return;

	/*
	 * OK, the equiv-classes map is built, now post-process the knowledge
	 *
	 * First, strip the list down to those which are at least partially present in the cache
	 */

	for(auto it=eqClasses.begin(); it!=eqClasses.end();)
	{
		for(auto& path: it->second.paths)
		{
			if(!GetFlags(path).vfile_ondisk)
				continue;
			goto strip_next_class;
		}
		eqClasses.erase(it++);
		continue;
strip_next_class:
		++it;
	}
	ERRMSGABORT;
	dbgDump("Refined (1):", 1);
	dbgState();
	// Let the most recent files be in the front of the list, but the uncompressed ones have priority
	for(auto& it: eqClasses)
	{
#ifdef DEBUG
		for(auto&x : it.second.paths)
			assert(!x.empty());
#endif
		sort(it.second.paths.begin(), it.second.paths.end(),
				fctLessThanCompMtime(CACHE_BASE));
		// and while we are at it, give them pointers back to the eq-classes
		for(auto &path : it.second.paths)
			SetFlags(path).bros=&it.second.paths;
	}
	dbgDump("Refined (2):", 2);
	dbgState();
	DelTree(SABSPATH("_actmp")); // do one to test the permissions

	// Iterate over classes and do patch-update where possible
	for(auto cid2eqcl=eqClasses.begin(); cid2eqcl!=eqClasses.end();cid2eqcl++)
	{
		decltype(cid2eqcl) itDiffIdx; // iterator pointing to the patch index descriptor
		int nProbeCnt(3);
		string patchidxFileToUse;
		deque<tPatchEntry> patchList;
		tFingerprint *pEndSum(nullptr);
		tPListConstIt itPatchStart;

		if(CheckStopSignal())
			return;

		DelTree(SABSPATH("_actmp"));

		if (cid2eqcl->second.diffIdxId.second.empty() || eqClasses.end() == (itDiffIdx
					= eqClasses.find(cid2eqcl->second.diffIdxId)) || itDiffIdx->second.paths.empty())
			goto NOT_PATCHABLE; // no patches available

		// iterate over patch paths and fine a present one which is most likely the most recent one
		for (const auto& pp : itDiffIdx->second.paths)
		{
			if (m_metaFilesRel[pp].vfile_ondisk)
			{
				patchidxFileToUse = pp;
				break;
			}
		}
		if (patchidxFileToUse.empty()) // huh, not found? Then just take the first one
			patchidxFileToUse = itDiffIdx->second.paths.front();

		if (!Download(patchidxFileToUse, true, eMsgShow))
			continue;

		if(CheckStopSignal())
			return;

		pEndSum=BuildPatchList(CACHE_BASE+patchidxFileToUse, patchList);

		if(!pEndSum)
			goto NOT_PATCHABLE;

		/* ok, patches should be available, what to patch? Probe up to three of the most
		 * recent ones
		 *
		 * 		 XXX now ideally, it should unpack on each test into an extra file and
		 * 		  then get the one which matched best. But it's too cumbersome, and
		 * 		   if the code works correctly, the first hit should always the best version
		 * 		   */

		for(const auto& pathRel: cid2eqcl->second.paths)
		{
			if(--nProbeCnt<0)
				break;

			FILE_RAII df;
			tFingerprint probe;
			auto absPath(SABSPATH(pathRel));
			::mkbasedir(sPatchBaseAbs);
			df.p = fopen(sPatchBaseAbs.c_str(), "w");
			if(!df.p)
			{
				SendFmt << "Cannot write temporary patch data to " << sPatchBaseAbs << "<br>";
				break;
			}
			if (GetFlags(pathRel).vfile_ondisk)
			{
				header h;
				if(h.LoadFromFile(absPath+ ".head")<=0  || GetFileSize(absPath, -2)
						!= atoofft(h.h[header::CONTENT_LENGTH], -3))
				{
					// only use sources that look like consistent downloads
					MTLOGDEBUG("########### Header looks suspicious on " << absPath << ".head");
					continue;
				}
				MTLOGDEBUG("#### Testing file: " << pathRel << " as patch base candidate");
				if (probe.ScanFile(absPath, CSTYPE_SHA1, true, df.p))
				{
					fflush(df.p); // write the whole file to disk ASAP!

					if(CheckStopSignal())
						return;

#ifdef DEBUG
					MTLOGDEBUG("## Looking for a patch for " << probe.GetCsAsString()
							<< "_" << probe.size
							<< " and latest version: "
							<< pEndSum->GetCsAsString() << "_" << pEndSum->size
							<< " or in ... "
					);
					for(auto patch:patchList)
						SendFmt << patch.fprPatch << " for version " << patch.fprState<<"<br>";
#endif
					// Hit the current state, no patching needed for it?
					if(probe == *pEndSum)
					{
						// since we know the stuff is fresh, no need to refetch it later
						m_metaFilesRel[pathRel].uptodate=true;
						if(m_bVerbose)
							SendFmt << "Found fresh version in " << pathRel << "<br>";

						Propagate(pathRel, cid2eqcl, &sPatchBaseAbs);

						if(CheckStopSignal())
							return;

						goto CONTINUE_NEXT_GROUP;
					}
					// or found at some previous state, try to patch it?
					else if (patchList.end() != (itPatchStart = find_if(patchList.begin(),
									patchList.end(), tCompByState(probe))))
					{
						// XXX for now, construct a replacement header based on some assumptions
						// tried hard and cannot imagine any case where this would be harmful
						if (h.h[header::XORIG])
						{
							string s(h.h[header::XORIG]);
							h.set(header::XORIG, s.substr(0, FindCompSfxPos(s)));
						}

						if(CheckStopSignal())
							return;

						if (m_bVerbose)
							SendFmt << "Found patching base candidate, unpacked to "
								<< sPatchBaseAbs << "<br>";

						if (df.close(), PatchFile(sPatchBaseAbs, patchidxFileToUse, itPatchStart,
									patchList.end(), pEndSum))
						{

							if(CheckStopSignal())
								return;

							h.set(header::LAST_MODIFIED, FAKEDATEMARK);
							h.set(header::CONTENT_LENGTH, pEndSum->size);
							if (h.StoreToFile(CACHE_BASE+sPatchResRel+".head") <= 0)
							{
								MTLOGDEBUG("############ Failed to store target header as "
										<< SABSPATH(sPatchResRel) << ".head");
								continue;
							}

							SendChunk("Patching result: succeeded<br>");
							Propagate(sPatchResRel, cid2eqcl);

							if(CheckStopSignal())
								return;

							InstallBz2edPatchResult(cid2eqcl);

							if(CheckStopSignal())
								return;

							break;
						}
						else
						{
							SendChunk("Patching result: failed<br>");
							// don't break, maybe the next one can be patched
						}

						if(CheckStopSignal())
							return;
					}
				}
			}
		}

NOT_PATCHABLE:

		MTLOGDEBUG("ok, now try to get a good version of that file and install this into needed locations");

		// prefer to download them in that order, no uncompressed versions because
		// mirrors usually don't have them
		for (auto& ps : compSuffixesByRatio)
		{
			for (auto& cand: cid2eqcl->second.paths)
			{
				if(!endsWith(cand, ps))
					continue;
				if(Download(cand, true, GetFlags(cand).hideDlErrors ? eMsgHideErrors : eMsgShow))
				{
					if(CheckStopSignal())
						return;
					if(Propagate(cand, cid2eqcl)) // all locations are covered?
						goto CONTINUE_NEXT_GROUP;
				}

				if(CheckStopSignal())
					return;
			}
		}

CONTINUE_NEXT_GROUP:

		if(CheckStopSignal())
			return;
	}

	dbgDump("Refined (3):", 3);

	dbgState();

	MTLOGDEBUG("<br><br><b>NOW GET THE REST</b><br><br>");

	// fetch all remaining stuff
	for(auto& idx2att : m_metaFilesRel)
	{
		if (idx2att.second.uptodate || idx2att.second.parseignore)
			continue;
		if(!idx2att.second.vfile_ondisk && !idx2att.second.guessed)
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

void tCacheOperation::InstallBz2edPatchResult(tContId2eqClass::iterator &eqClassIter)
{
#ifndef HAVE_LIBBZ2
	return;
#else
	if(!acfg::recompbz2)
		return;

	string sFreshBz2Rel;
	tFingerprint &bz2fpr=eqClassIter->second.bz2VersContId.first;
	string sRefBz2Rel;

	for (const auto& tgt : eqClassIter->second.paths)
	{
		if (endsWithSzAr(tgt, ".bz2"))
		{
			auto &fl = GetFlags(tgt);
			if (fl.vfile_ondisk)
			{
				// needs a reference location to get the HTTP headers for, pickup something
				if(sRefBz2Rel.empty())
					sRefBz2Rel=tgt;

				if (fl.uptodate)
				{
					sFreshBz2Rel = tgt;
					goto inject_bz2s;
				}

				if(sFreshBz2Rel.empty())
					sFreshBz2Rel = sPatchResRel+".bz2";

				// continue searching, there might be a working version
			}
		}
	}

	// not skipped this code... needs recompression then?
	if (sFreshBz2Rel.empty())
		return;

	// ok, it points to the temp file then, create it

	if (!Bz2compressFile(SZABSPATH(sPatchResRel), SZABSPATH(sFreshBz2Rel))
			|| !bz2fpr.CheckFile(SABSPATH(sFreshBz2Rel))
	// fileitem implementation may nuke the data on errors... doesn't matter here
			|| !GetAndCheckHead(sFreshBz2Rel, sRefBz2Rel, bz2fpr.size))
	{
		return;
	}

	if (m_bVerbose)
		SendFmt << "Compressed into " << sFreshBz2Rel << "<br>\n";

	inject_bz2s:
	// use a recursive call to distribute bz2 versions

	if(CheckStopSignal())
		return;

	if (!sFreshBz2Rel.empty())
	{
#ifdef DEBUG
		SendFmt << "Recursive call to install the bz2 version from " << sFreshBz2Rel << "<br>";
#endif
		Propagate(sFreshBz2Rel, eqClassIter);
	}
#endif
}

tCacheOperation::enumMetaType tCacheOperation::GuessMetaTypeFromURL(cmstring &sPath)
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

	return EIDX_UNSUPPORTED;
}

bool tCacheOperation::ParseAndProcessMetaFile(ifileprocessor &ret, const std::string &sPath,
		enumMetaType idxType)
{

	LOGSTART("expiration::ParseAndProcessMetaFile");

#ifdef DEBUG_FLAGS
	bool bNix=StrHas(sPath, "/i18n/");
#endif

	m_processedIfile = sPath;

	// pre calc relative base folders for later
	string sCurFilesReferenceDirRel(SZPATHSEP);
	// for some file types that may differ, e.g. if the path looks like a Debian mirror path
	string sPkgBaseDir = sCurFilesReferenceDirRel;
	tStrPos pos = sPath.rfind(CPATHSEP);
	if(stmiss!=pos)
	{
		sCurFilesReferenceDirRel.assign(sPath, 0, pos+1);

		// does this look like a Debian archive structure? i.e. paths to other files have a base
		// directory starting in dists/?
		// The assumption doesn't however apply to the d-i checksum
		// lists, those refer to themselves only.
		//
		// similar considerations for Cygwin setup

		if(idxType != EIDX_MD5DILIST && stmiss!=(pos=sCurFilesReferenceDirRel.rfind("/dists/")))
			sPkgBaseDir = sPkgBaseDir.assign(sCurFilesReferenceDirRel, 0, pos+1);
		else if(idxType == EIDX_CYGSETUP && stmiss!=(pos=sCurFilesReferenceDirRel.rfind("/cygwin/")))
			sPkgBaseDir = sPkgBaseDir.assign(sCurFilesReferenceDirRel, 0, pos+8);
		else
			sPkgBaseDir=sCurFilesReferenceDirRel;
	}
	else
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
	bool bUse(false);

	enumMetaType origIdxType=idxType;


	REDO_AS_TYPE:
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
					ret.HandlePkgEntry(info);
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
			uint nStep = 0;
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
						ret.HandlePkgEntry(info);
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
						info.sDirectory = sCurFilesReferenceDirRel;
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

			uint begin(0);
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
				ret.HandlePkgEntry(info);
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
				info.sDirectory = sCurFilesReferenceDirRel;
				ret.HandlePkgEntry(info);
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
					info.sDirectory = sCurFilesReferenceDirRel;
					ret.HandlePkgEntry(info);
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
				info.sDirectory = sCurFilesReferenceDirRel;

				pos=info.sFileName.find_first_not_of("./");
				if(pos!=stmiss)
					info.sFileName.erase(0, pos);

				pos=info.sFileName.rfind(SZPATHSEPUNIX);
				if (stmiss!=pos)
				{
					info.sDirectory += info.sFileName.substr(0, pos+1);
					info.sFileName.erase(0, pos+1);
				}
				ret.HandlePkgEntry(info);
				info.SetInvalid();
			}
		}
		break;
	case EIDX_DIFFIDX:
		info.fpr.csType = CSTYPE_SHA1;
		info.bInflateForCs = true;
		sStartMark = "SHA1-Patches:";
		idxType = EIDX_RFC822WITHLISTS;
		goto REDO_AS_TYPE;

	case EIDX_SOURCES:
		info.fpr.csType = CSTYPE_MD5;
		sStartMark="Files:";
		idxType = EIDX_RFC822WITHLISTS;
		goto REDO_AS_TYPE;

	case EIDX_TRANSIDX:
	case EIDX_RELEASE:
		info.fpr.csType = CSTYPE_SHA1;
		sStartMark="SHA1:";
		// parser follows
		//no break
	case EIDX_RFC822WITHLISTS:
		// common info object does not help here because there are many entries, and directory
		// could appear after the list :-(
	{
		// template for the data set PLUS try-its-gzipped-version flag
		tStrDeq fileList;
		mstring sDirHeader;

		UrlUnescape(sPkgBaseDir);

		while(reader.GetOneLine(sLine))
		{
			if(startsWith(sLine, sStartMark))
				bUse=true;
			else if(!startsWithSz(sLine, " ")) // list header block ended for sure
				bUse = false;

			trimBack(sLine);

			if(startsWithSz(sLine, "Directory:"))
			{
				trimBack(sLine);
				tStrPos pos=sLine.find_first_not_of(SPACECHARS, 10);
				if(pos!=stmiss)
					sDirHeader=sLine.substr(pos)+SZPATHSEP;
			}
			else if (bUse)
				fileList.emplace_back(sLine);
			else if(sLine.empty()) // ok, time to commit the list
			{
				for (auto& fline : fileList)
				{
					info.sFileName.clear();
					// ok, read "checksum size filename" into info and check the word count
					tSplitWalk split(&fline);
					if (!split.Next() || !info.fpr.SetCs(split, info.fpr.csType) || !split.Next())
						continue;
					val = split;
					info.fpr.size = atoofft(val.c_str(), -2);
					if (info.fpr.size < 0 || !split.Next())
						continue;
					UrlUnescapeAppend(split, info.sFileName);

					switch (origIdxType)
					{
					case EIDX_SOURCES:
						info.sDirectory = sPkgBaseDir + sDirHeader;
						ret.HandlePkgEntry(info);
						break;
					case EIDX_TRANSIDX: // csum refers to the files as-is
						info.sDirectory = sCurFilesReferenceDirRel + sDirHeader;
						ret.HandlePkgEntry(info);
						break;
					case EIDX_DIFFIDX:
						info.sDirectory = sCurFilesReferenceDirRel + sDirHeader;
						ret.HandlePkgEntry(info);
						info.sFileName += ".gz";
						ret.HandlePkgEntry(info);
						break;
					case EIDX_RELEASE:
						info.sDirectory = sCurFilesReferenceDirRel + sDirHeader;
						// usually has subfolders, move into directory part
						pos = info.sFileName.rfind(SZPATHSEPUNIX);
						if (stmiss != pos)
						{
							info.sDirectory += info.sFileName.substr(0, pos + 1);
							info.sFileName.erase(0, pos + 1);
						}
						ret.HandlePkgEntry(info);
						break;
					default:
						ASSERT(!"Originally determined type cannot reach this case!");
						break;
					}
				}
				fileList.clear();
			}

			if(CheckStopSignal())
				return true;

		}
	}
	break;

	default:
		SendChunk("<span class=\"WARNING\">"
				"WARNING: unable to read this file (unsupported format)</span>\n<br>\n");
		return false;
	}
	return reader.CheckGoodState(false);
}

void tCacheOperation::ProcessSeenMetaFiles(ifileprocessor &pkgHandler)
{
	LOGSTART("expiration::_ParseVolatileFilesAndHandleEntries");
	for(auto& path2att: m_metaFilesRel)
	{
		if(CheckStopSignal())
			return;

		const tIfileAttribs &att=path2att.second;
		enumMetaType itype = att.eIdxType;
		if(!itype)
			itype=GuessMetaTypeFromURL(path2att.first);
		if(!itype) // still unknown. Where does it come from? Just ignore.
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

		SendChunk(string("Parsing metadata in ")+path2att.first+"<br>\n");

		if( ! ParseAndProcessMetaFile(pkgHandler, path2att.first, itype))
		{
			if(!m_metaFilesRel[path2att.first].forgiveDlErrors)
			{
				m_nErrorCount++;
				SendChunk("<span class=\"ERROR\">An error occured while reading this file, some contents may have been ignored.</span>\n");
				AddDelCbox(path2att.first);
				SendChunk("<br>\n");
			}
			continue;
		}
		else if(!m_bByPath && att.bros)
		{
			for(auto& bro: *att.bros)
			{
				MTLOGDEBUG("Marking " << bro << " as processed");
				SetFlags(bro).alreadyparsed=true;
			}
		}
	}
}

void tCacheOperation::AddDelCbox(cmstring &sFileRel)
{
	auto ref_isnew = m_delCboxFilter.insert(sFileRel);
	if(! ref_isnew.second)
		return;

	SendFmtRemote <<  "<label><input type=\"checkbox\" name=\"kf" << (nKillLfd++)
			<< "\" value=\"" << sFileRel << "\">Tag</label>";

}
void tCacheOperation::TellCount(uint nCount, off_t nSize)
{
	SendFmt << "<br>\n" << nCount <<" package file(s) marked "
			"for removal in few days. Estimated disk space to be released: "
			<< offttosH(nSize) << ".<br>\n<br>\n";
}

void tCacheOperation::SetCommonUserFlags(cmstring &cmd)
{
	m_bErrAbort=(cmd.find("abortOnErrors=aOe")!=stmiss);
	m_bByChecksum=(cmd.find("byChecksum")!=stmiss);
	m_bByPath=(StrHas(cmd, "byPath") || m_bByChecksum);
	m_bVerbose=(cmd.find("beVerbose")!=stmiss);
	m_bForceDownload=(cmd.find("forceRedownload")!=stmiss);
	m_bSkipHeaderChecks=(cmd.find("skipHeadChecks")!=stmiss);
	m_bTruncateDamaged=(cmd.find("truncNow")!=stmiss);
}

void tCacheOperation::PrintStats(cmstring &title)
{
	multimap<off_t, cmstring*> sorted;
	off_t total=0;
	const uint nMax = m_bVerbose ? (UINT_MAX-1) : MAX_TOP_COUNT;
	uint hidden=0;
	for(auto &f: m_metaFilesRel)
	{
		total += f.second.space;
		if(f.second.space)
			EMPLACE_PAIR(sorted,f.second.space, &f.first);
		if(sorted.size()>nMax)
		{
			sorted.erase(sorted.begin());
			hidden++;
		}
	}
	if(!total)
		return;
	m_fmtHelper << "<br>\n<table><tr><td colspan=2><u>" << title;
	if(!m_bVerbose && hidden>0)
		m_fmtHelper << " (Top " << sorted.size() << ", " << hidden <<  " more not displayed)";
	m_fmtHelper << "</u></td></tr>\n";
	for(auto it=sorted.rbegin(); it!=sorted.rend(); ++it)
	{
		m_fmtHelper << "<tr><td><b>" << offttosH(it->first) << "</b></td><td>"
				<< *(it->second) << "</td></tr>\n";
	}
	SendFmt << "</table>";
}

#ifdef DEBUG
int parseidx_demo(LPCSTR file)
{

	class tParser : public tCacheOperation, ifileprocessor
	{
	public:
		tParser() : tCacheOperation({2, tSpecialRequest::workIMPORT, "doImport="}) {};
		inline int demo(LPCSTR file)
		{
			return !ParseAndProcessMetaFile(*this, file, GuessMetaTypeFromURL(file));
		}
		virtual void HandlePkgEntry(const tRemoteFileInfo &entry) override
		{
			cout << "Dir: " << entry.sDirectory << endl << "File: " << entry.sFileName << endl
					<< "Checksum-" << GetCsName(entry.fpr.csType) << ": " << entry.fpr.GetCsAsString()
					<< endl << "ChecksumUncompressed: " << entry.bInflateForCs << endl <<endl;
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


void tCacheOperation::ProgTell()
{
	if (++m_nProgIdx == m_nProgTell)
	{
		SendFmt<<"Scanning, found "<<m_nProgIdx<<" file"
				<< (m_nProgIdx>1?"s":"") << "...<br />\n";
		m_nProgTell*=2;
	}
}
