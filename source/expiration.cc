
//#define LOCAL_DEBUG
#include "debug.h"

#include "expiration.h"
#include "meta.h"
#include "filereader.h"
#include "fileio.h"

#include <unistd.h>
#include <dirent.h>

#include <map>
#include <string>
#include <iostream>
#include <algorithm>

using namespace MYSTD;

#define ENABLED

#ifndef ENABLED
#warning Unlinking parts defused
#endif

#define ERRMSGABORT if(m_nErrorCount && m_bErrAbort) { SendChunk(sErr); return; }
#define ERRABORT if(m_nErrorCount && m_bErrAbort) { return; }

#define TIMEEXPIRED(t) (t < (m_gMaintTimeNow-acfg::extreshhold*86400))
#define TIME_AGONY (m_gMaintTimeNow-acfg::extreshhold*86400)

#define FNAME_PENDING "_expending_dat"
#define FNAME_DAMAGED "_expending_damaged"
#define sFAIL_INI SABSPATH("_exfail_cnt")
#define FAIL_INI sFAIL_INI.c_str()

static cmstring sIndex("Index");
static cmstring sslIndex("/Index");

void expiration::HandlePkgEntry(const tRemoteFileInfo &entry, bool bUnpackForCsumming)
{
	LOGSTART2("expiration::_HandlePkgEntry:",
			"\ndir:" << entry.sDirectory <<
			"\nname: " << entry.sFileName <<
			"\nsize: " << entry.fpr.size <<
			"\ncsum: " << entry.fpr.GetCsAsString());

	// debian-installer files also need to do path checks
	// checksum mode also needs to be sure about path in order to not display false positives to the user

	// cannot use equal_range() without directory consideration, skip equal entries manually
	tFileNdir startHook(entry.sFileName, m_bNeedsStrictPathsHere ? entry.sDirectory : "");
	for(tS2DAT::iterator it=m_trashCandSet.lower_bound(startHook);
			it!=m_trashCandSet.end();
			/* erases inside -> step forward there */ )
	{
		const tFileNdir &k = it->first; // shortcut
		tDiskFileInfo &desc = it->second;

		// where are we, still at the right filename? or could use range instead of lower_bound...
		if(entry.sFileName != k.file)
			return;

		tFingerprint & fprHave=desc.fpr;
		const tFingerprint & fprNeed=entry.fpr;

		string sPathRel(k.dirRel+k.file);
		string sPathAbs(CACHE_BASE+sPathRel);
		header h;
		tS2IDX::const_iterator j;

		if(ContHas(m_forceKeepInTrash,sPathRel))
			goto keep_in_trash;

		// needs to match the exact file location.
		// And for "Index" files, they have always to be at a well defined location, this
		// constraint is also needed to expire deprecated files
		if(m_bNeedsStrictPathsHere || entry.sFileName == sIndex)
		{
			// compare full paths (physical vs. remote) with their real paths
			string sEntrPath=entry.sDirectory+entry.sFileName;
			LOG("Checking exact path: " << sEntrPath << " vs. " << sPathRel);
			pathTidy(sEntrPath);
			if(sPathRel != sEntrPath)
				goto keep_in_trash; // not for us, try another one
		}

		// Basic header checks. Skip if the file was forcibly updated/reconstructed before.
		if (m_bSkipHeaderChecks || desc.bHeaderTestDone
				/*
				|| (j = m_indexFilesRel.find(sPathRel)) == m_indexFilesRel.end()
				|| j->second.uptodate
				*/
				)
		{
			LOG("Skipped header check for " << sPathRel);
		}
		else
		{
			LOG("Doing basic header checks");
			desc.bHeaderTestDone = true;

			if (0<h.LoadFromFile(sPathAbs+".head"))
			{

#if 0 // too easy - there should be no inconsistencies here. If that happens, someone might report it.
				if(h.h[header::LAST_MODIFIED]
				       && 0==strcmp(h.h[header::LAST_MODIFIED], FAKEDATEMARK))
				{
					// created by us, don't care
					goto head_checked;
				}
#endif

				off_t len=atoofft(h.h[header::CONTENT_LENGTH], -2);
				if(len<0)
				{
					SendFmt << "<span class=\"ERROR\">WARNING, header file of "
							<< sPathRel << " does not contain content length</span>";
					goto keep_in_trash;
				}
				off_t lenInfo=GetFileSize(sPathAbs, -3);
				if(lenInfo<0)
				{
					SendFmt << ": error reading attributes, ignoring" << sPathRel;
					goto keep_in_trash;
				}
				if (len < lenInfo)
				{
					SendFmt << "<span class=\"ERROR\">WARNING, header file of " << sPathRel
							<< " reported too small file size (" << len << " vs. " << lenInfo
							<< "), invalidating and removing header</span><br>\n";
					// kill the header ASAP, no matter which extra exception might apply later
					ignore_value(::unlink((sPathAbs+".head").c_str()));
					goto keep_in_trash;
				}
			}
			else
			{
				tIfileAttribs at = GetFlags(sPathRel);
				if(!at.parseignore && !at.forgiveDlErrors)
				{
					SendFmt<<"<span class=\"WARNING\">WARNING, header file missing or damaged for "
						<<sPathRel<<"</span>\n<br>\n";
				}
			}
		}

		if(m_bByChecksum && fprHave.size != 0)
		{

			bool bSkipDataCheck=false;

			// can tell this early with information form dirscan if unpacking is not needed
			if(!bUnpackForCsumming && fprHave.size < entry.fpr.size)
			{
				if(m_bIncompleteIsDamaged)
					goto l_tell_damaged;
				else
					goto l_tell_incomplete;
			}

			// scan file if not done before or the checksum type differs
			if(fprNeed.csType != fprHave.csType
					&& !fprHave.ScanFile(sPathAbs, fprNeed.csType, bUnpackForCsumming))
			{
					// IO error? better keep it for now
				aclog::err(tSS()<<"An error occurred while checksumming "
						<< sPathAbs	<< ", not touching it.");
				bSkipDataCheck=true;
			}

			if ( !bSkipDataCheck && fprHave != entry.fpr)
			{
				if (fprHave.size < entry.fpr.size && !m_bIncompleteIsDamaged)
				{
					l_tell_incomplete:
					SendFmt << "<span class=\"WARNING\">INCOMPLETE: " << sPathRel
							<< " (ignoring)</span><br>\n";
				}
				else if (m_bTruncateDamaged)
				{
					SendFmt << "<span class=\"WARNING\">BAD: " << sPathRel
							<< " (truncating)</span><br>\n";
					ignore_value(::truncate(sPathAbs.c_str(), 0));
				}
				else
				{
					l_tell_damaged:

					if(GetFlags(sPathRel).parseignore)
					{
						SendFmt << sPathRel << ": incorrect or obsolete contents but marked as "
								"alternative version of another index file "
								"<span class=\"WARNING\">(ignoring)</span>";
					}
					else
					{
						if (m_damageList.is_open())
							m_damageList << sPathRel << endl;
						SendFmt << "<span class=\"ERROR\">BAD: " << sPathRel
								<< " (adding to damage list)</span>";
						AddDelCbox(sPathRel);
					}
					SendChunk("<br>\n");

					goto keep_in_trash;
				}
			}
		}

		// ok, package matched, contents ok if checked, drop it from the removal list
		if (m_bVerbose)
			SendFmt << "<font color=green>OK: " << sPathRel << "</font><br>\n";


		{ // goto side effects :-(
			tFileNdir headKey = it->first;
			headKey.file += ".head";
			m_trashCandHeadSet.erase(headKey);
			// delete&increment using a backup copy
			m_trashCandSet.erase(it++);
		}

		SetFlags(m_processedIfile).space+=entry.fpr.size;

		continue;

		keep_in_trash:
		it++;
	}
}

struct tVerCompKey
{
	mstring prevName, ver, prevArcSufx;
	tS2DAT::iterator it;

	inline bool Set(tS2DAT::iterator &it)
	{
		// only care about the doomed ones
		//if( ! TIMEEXPIRED(it->second.nLostAt))
		//	return false;

		cmstring &name=it->first.file;
		tSplitWalk split(&name, "_");
		if(!split.Next())
			return false;
		prevName=split;
		if(!split.Next())
			return false;
		ver=split;
		for (const char *p = prevArcSufx.c_str(); *p; ++p)
			if (!isalnum(UINT(*p)) && !strchr(".-+:~", UINT(*p)))
				return false;
		if(!split.Next())
			return false;
		prevArcSufx=split;
		this->it=it;
		return !split.Next(); // no trailing crap
	}
	inline bool SamePkg(tVerCompKey &other) const
	{
		return other.prevName == prevName && other.prevArcSufx == prevArcSufx;
	}
	inline bool operator<(const tVerCompKey &other) const
	{
		return CompDebVerLessThan(ver, other.ver);
	}
};

// this method looks for the validity of additional package files kept in cache after
// the Debian version moved to a higher one
inline void expiration::DropExceptionalVersions()
{
    if(m_trashCandSet.empty() || !acfg::keepnver)
    	return;

	int ec=0;
	MYSTD::set<tVerCompKey> pkgGroup;

	for (tS2DAT::iterator it = m_trashCandSet.begin();; ++it)
	{
		tVerCompKey key;
		if (it != m_trashCandSet.end())
		{
			if( ! TIMEEXPIRED(it->second.nLostAt))
				continue; // already has a life assurance
			if (!key.Set(it))
				continue;
			if (pkgGroup.empty() || pkgGroup.begin()->SamePkg(key))
			{
				pkgGroup.insert(key);
				continue;
			}
		}

		// not same pkg or finishing, cleaning the group
		// no need for tree rotations since count is known
		int nc = int(pkgGroup.size());
		if(acfg::keepnver<nc)
			nc-=acfg::keepnver;

		for (MYSTD::set<tVerCompKey>::iterator lit = pkgGroup.begin();
				lit != pkgGroup.end() && nc-- > 0; ++lit)
		{
			// m_trashCandSet.erase(lit->it);
			// no point in rotating the tree, set it to oldest non-expirable timestamp
			lit->it->second.nLostAt = TIME_AGONY;
			ec++;
		}

		if (it == m_trashCandSet.end())
			break;

		// restart the group
		pkgGroup.clear();
		pkgGroup.insert(key);
	}
	if(ec)
		SendFmt << "<br><b>Kept "
		<< ec << " more package(s) not covered by index references</b><br><br>\n";
}

inline void expiration::RemoveAndStoreStatus(bool bPurgeNow)
{
	LOGSTART("expiration::_RemoveAndStoreStatus");
	FILE_RAII f;
    if(!bPurgeNow)
    {

        DropExceptionalVersions();

        string sDbFileAbs=CACHE_BASE+FNAME_PENDING;

        f.p = fopen(sDbFileAbs.c_str(), "w");
        if(!f)
        {
            SendChunk("Unable to open " FNAME_PENDING " for writing, attempting to recreate... ");
            ::unlink(sDbFileAbs.c_str());
            f.p=::fopen(sDbFileAbs.c_str(), "w");
            if(f)
                SendChunk("OK\n<br>\n");
            else
            {
                SendChunk("<span class=\"ERROR\">FAILED. ABORTING. Check filesystem and file permissions.</span>");
                return;
            }
        }
    }

	int n(0);
	off_t tagSpace(0);
	for (tS2DAT::iterator it = m_trashCandSet.begin(); it != m_trashCandSet.end(); it++)
	{
		using namespace rechecks;
		const tFileNdir &k = it->first; // shortcut
		string sPathRel = k.dirRel+k.file;
		DBGQLOG("Checking " << sPathRel);

		if(ContHas(m_forceKeepInTrash,sPathRel))
		{
			LOG("forcetrash flag set, whitelist does not apply, not to be removed");
		}
		else
		{
			if (Match(k.file, WHITELIST) || Match(sPathRel, WHITELIST))
			{
				LOG("Protected file, not to be removed");
				continue;
			}
		}

		if(!it->second.nLostAt) // no shit, it should be assigned at least once
			continue;

		//cout << "Unreferenced: " << it->second.sDirname << it->first <<endl;

		string sPathAbs=CACHE_BASE+sPathRel;

		// file will be removed (with its header) or tagged ASAP,
		// don't consider its header for anything afterwards
	    m_trashCandHeadSet.erase(tFileNdir(k.file+".head", k.dirRel));

	    //cout << "Took " << sWhatHead << " from the list" <<endl;

		if(bPurgeNow || TIMEEXPIRED(it->second.nLostAt))
		{
			SendFmt << "Removing " << sPathRel << "<br>\n";

#ifdef ENABLED
			::unlink(sPathAbs.c_str());
			::unlink((sPathAbs+".head").c_str());
			::rmdir((CACHE_BASE + k.dirRel).c_str());
#endif
        }
		else if(f)
		{
			SendFmt << "Tagging " << sPathRel;
			if(m_bVerbose)
				SendFmt << " (t-"<<(m_gMaintTimeNow-it->second.nLostAt)/3600 << "h)";
			SendChunk("<br>\n");

			n++;
			tagSpace+=it->second.fpr.size;
			fprintf(f, "%lu\t%s\t%s\n",  it->second.nLostAt,
					k.dirRel.c_str(), k.file.c_str());
		}
	}

    // now just kill dangling header files
	for(set<tFileNdir>::iterator it=m_trashCandHeadSet.begin();
            it != m_trashCandHeadSet.end(); it++)
	{
		string sPathRel(it->dirRel + it->file);
		if (rechecks::Match(sPathRel, rechecks::WHITELIST)
				|| (endsWithSzAr(sPathRel, ".head")
				&& rechecks::Match(sPathRel.substr(0, sPathRel.size() - 5), rechecks::WHITELIST)))
		{
			continue;
		}

		if(m_bVerbose)
			SendFmt << "Removing orphaned head file: " << sPathRel;

#ifdef ENABLED
        string sPathAbs=CACHE_BASE+sPathRel;
        ::unlink(sPathAbs.c_str());
        string::size_type pos=sPathAbs.find_last_of(SZPATHSEPUNIX SZPATHSEPWIN);
        if(pos!=stmiss)
        	::rmdir(sPathAbs.substr(0, pos).c_str());
#endif
    }
    if(n>0)
    	TellCount(n, tagSpace);
}


void expiration::Action(const string & cmd)
{
	if (m_mode==purge)
	{
		LoadPreviousData(true);
		RemoveAndStoreStatus(true);
		return;
	}
	if (m_mode==list)
	{
		LoadPreviousData(true);
		off_t nSpace(0);
		uint cnt(0);
		for (tS2DAT::iterator it=m_trashCandSet.begin(); it
				!=m_trashCandSet.end(); it++)
		{
			const tFileNdir &k=it->first;
			string rel=k.dirRel+k.file;
			off_t sz=GetFileSize(CACHE_BASE+rel, -2);
			if(sz<0)
				continue;

			cnt++;
			SendChunk(rel+"<br>\n");
			nSpace+=sz;

			sz = GetFileSize(CACHE_BASE + rel+".head", -2);
			if (sz >= 0)
			{
				nSpace += sz;
				SendChunk(rel + ".head<br>\n");
			}
		}
		TellCount(cnt, nSpace);

		mstring delURL(cmd);
		StrSubst(delURL, "justShow", "justRemove");
		SendFmtRemote << "<a href=\""<<delURL<<"\">Delete all listed files</a> "
				"(no further confirmation)<br>\n";
		return;
	}

	if(m_mode==purgeDamaged || m_mode==listDamaged || m_mode==truncDamaged)
	{
		filereader f;
		if(!f.OpenFile(SABSPATH(FNAME_DAMAGED)))
		{
			SendChunk("List of damaged files not found");
			return;
		}
		mstring s;
		while(f.GetOneLine(s))
		{
			if(s.empty())
				continue;

			if(m_mode == purgeDamaged)
			{
				SendFmt << "Removing " << s << "<br>\n";
				::unlink(SZABSPATH(s));
				::unlink(SZABSPATH(s+".head"));
			}
			else if(m_mode == truncDamaged)
			{
				SendFmt << "Truncating " << s << "<br>\n";
				ignore_value(::truncate(SZABSPATH(s), 0));
			}
			else
				SendFmt << s << "<br>\n";
		}
		return;
	}

	SetCommonUserFlags(cmd);

	m_bIncompleteIsDamaged=(cmd.find("incomAsDamaged")!=stmiss);

	SendChunk("<b>Locating potentially expired files in the cache...</b><br>\n");

	DirectoryWalk(acfg::cachedir, this);
	if(CheckStopSignal())
		goto save_fail_count;
	SendFmt<<"Found "<<m_nProgIdx<<" files.<br />\n";

	//cout << "found package files: " << m_trashCandidates.size()<<endl;
	//for(tS2DAT::iterator it=m_trashCandSet.begin(); it!=m_trashCandSet.end(); it++)
	//	SendChunk(tSS()<<it->second.sDirname << "~~~" << it->first << " : " << it->second.fpr.size<<"<br>");

	LoadHints();
	UpdateIndexFiles();

	if(CheckAndReportError() || CheckStopSignal())
		goto save_fail_count;

	m_damageList.open(SZABSPATH(FNAME_DAMAGED), ios::out | ios::trunc);

	SendChunk("<b>Validating cache contents...</b><br>\n");
	ProcessSeenIndexFiles(*this);

	if(CheckAndReportError() || CheckStopSignal())
		goto save_fail_count;

	// update timestamps of pending removals
	LoadPreviousData(false);

	RemoveAndStoreStatus(cmd.find("purgeNow")!=stmiss);
	PurgeMaintLogs();

	DelTree(CACHE_BASE+"_actmp");

	PrintStats("Allocated disk space");

	SendChunk("<br>Done.</br>");

	save_fail_count:

	if (m_nErrorCount <= 0)
	{
		::unlink(FAIL_INI);
	}
	else
	{
		FILE *f = fopen(FAIL_INI, "a");
		if (!f)
		{
			SendFmt << "Unable to open " <<
			sFAIL_INI << " for writing, attempting to recreate... ";
			::unlink(FAIL_INI);
			f = ::fopen(FAIL_INI, "w");
			if (f)
				SendChunk("OK\n<br>\n");
			else
			{
				SendChunk(
						"<span class=\"ERROR\">FAILED. ABORTING. Check filesystem and file permissions.</span>");
			}
		}
		if (f)
		{
			::fprintf(f, "%lu\n", (long unsigned int) GetTime());
			checkForceFclose(f);
		}
	}

}

void expiration::PurgeMaintLogs()
{
	tStrDeq logs = ExpandFilePattern(acfg::logdir + SZPATHSEP MAINT_PFX "*.log*");
	if (logs.size() > 2)
		SendChunk(
				"Found required cleanup tasks: purging maintenance logs...<br>\n");
	for (tStrDeq::const_iterator it = logs.begin(); it != logs.end(); it++)
	{
		time_t id = atoofft(it->c_str() + acfg::logdir.size() + 7);
		//cerr << "id ist: "<< id<<endl;
		if (id == GetTaskId())
			continue;
		//cerr << "Remove: "<<globbuf.gl_pathv[i]<<endl;
#ifdef ENABLED
		::unlink(it->c_str());
#endif
	}
}

void expiration::UpdateFingerprint(const MYSTD::string &sPathRel,
		off_t nOverrideSize, uint8_t *pOverrideSha1, uint8_t *pOverrideMd5)
{
	// navigating to the data set...
	tStrPos nCutPos = sPathRel.rfind(CPATHSEP);
	tFileNdir key((stmiss == nCutPos) ? sPathRel : sPathRel.substr(nCutPos+1),
			(stmiss == nCutPos) ? "" : sPathRel.substr(0, nCutPos+1));
	tDiskFileInfo &finfo = m_trashCandSet[key];

	if (!finfo.nLostAt) // just has been created? smells like a bug
		return;

	if(pOverrideMd5)
		finfo.fpr.Set(pOverrideMd5, CSTYPE_MD5, nOverrideSize);
	else if(pOverrideSha1)
		finfo.fpr.Set(pOverrideSha1, CSTYPE_SHA1, nOverrideSize);
	else if(nOverrideSize != off_t(-1))
		finfo.fpr.Set(NULL, CSTYPE_INVALID, nOverrideSize);
	/*
	else if (!pDataSrc)
			finfo.fpr.ScanFile(sPath, finfo.fpr.csType, false);
	else if (CSTYPE_MD5 == finfo.fpr.csType) // or use the file object which is already mmap'ed
		pDataSrc->GetMd5Sum(sPath, finfo.fpr.csum, false, finfo.fpr.size);
	else if (CSTYPE_SHA1 == finfo.fpr.csType)
		pDataSrc->GetSha1Sum(sPath, finfo.fpr.csum, false, finfo.fpr.size);
		*/
	else // just update the file size
	{
		finfo.fpr.csType=CSTYPE_INVALID;
		Cstat stbuf(CACHE_BASE+sPathRel);
		if (!stbuf)
			aclog::err(sPathRel + " << FAILED TO READ");
		else
			finfo.fpr.Set(NULL, CSTYPE_INVALID, stbuf.st_size);
	}
}


bool expiration::ProcessRegular(const string & sPathAbs, const struct stat &stinfo)
{

	if(CheckStopSignal())
		return false;

	if(sPathAbs.size()<=CACHE_BASE_LEN) // heh?
		return false;

	ProgTell();

	string sPathRel(sPathAbs, CACHE_BASE_LEN);
	DBGQLOG(sPathRel);

	// detect strings which are only useful for shell or js attacks
	if(sPathRel.find_first_of("\r\n'\"<>")!=stmiss)
		return true;

	if(sPathRel[0] == '_' && !m_bScanInternals)
		return true; // not for us

	// handle the head files separately
    if (endsWithSzAr(sPathRel, ".head"))
		m_trashCandHeadSet.insert(sPathRel);
	else
	{
		if(AddIFileCandidate(sPathRel))
		{
			tIfileAttribs &attr = SetFlags(sPathRel);
			attr.space+=stinfo.st_size;
			if(endsWith(sPathRel, sslIndex))
			{
				attr.forgiveDlErrors = true;
				//attr.hideDlErrors = true;
			}
		}

//#warning TODO: when Lenny is history, add additional MD5SUMS file to the list for d-i image files
		//SendChunk(string("<br>hm?<br>")+sDirnameAndSlash + " -- " + sBasename);
		tDiskFileInfo &finfo = m_trashCandSet[sPathRel];
		finfo.nLostAt = m_gMaintTimeNow;
		finfo.fpr.size = stinfo.st_size;
	}
    return true;
}


expiration::expiration(int fd, workType type) :
		tCacheMan(fd), m_mode(type), m_bIncompleteIsDamaged(false), m_nPrevFailCount(0)
{
	switch(type)
	{
	case list: m_sTypeName="Listing unreferenced"; break;
	case listDamaged: m_sTypeName="Listing damaged files"; break;
	case purgeDamaged: m_sTypeName="Removing damaged files"; break;
	case truncDamaged: m_sTypeName="Truncating damaged files to zero size"; break;
	case expire:
	default:
		m_sTypeName="Expiration"; break;
	}
}

expiration::~expiration()
{
}

void expiration::LoadHints()
{
	filereader reader;
	if(!reader.OpenFile(acfg::confdir+SZPATHSEP+"ignore_list"))
	{
		if(acfg::suppdir.empty() || !reader.OpenFile(acfg::suppdir+SZPATHSEP+"ignore_list"))
				return;
	}
	string sTmp;
	while (reader.GetOneLine(sTmp))
	{
		trimLine(sTmp);
		if (startsWithSz(sTmp, "#"))
			continue;
		if(startsWith(sTmp, CACHE_BASE))
			sTmp.erase(CACHE_BASE_LEN);
		if(sTmp.empty())
			continue;
		SetFlags(sTmp).forgiveDlErrors=true;
	}

	reader.Close();
	reader.OpenFile(sFAIL_INI);
	while(reader.GetOneLine(sTmp))
		m_nPrevFailCount += (atol(sTmp.c_str())>0);



}
void expiration::LoadPreviousData(bool bForceInsert)
{
	filereader reader;
	reader.OpenFile(SABSPATH(FNAME_PENDING));

	string sLine;

#if 0
	// stuff for user info
	time_t now=time(NULL);
	time_t oldest(now), newest(0);
#endif

	while(reader.GetOneLine(sLine))
	{
		char *eptr(NULL);
		const char *s = sLine.c_str();
		time_t timestamp = strtoull(s, &eptr, 10);
		if (!eptr || *eptr != '\t' || !timestamp || timestamp > m_gMaintTimeNow) // where is the DeLorean?
			continue;
		const char *sep = strchr(++eptr, (unsigned) '\t');
		if (!sep)
			continue;
		string dir(eptr, sep - eptr);
		if (!dir.empty() && '/' != *(sep - 1))
			dir += "/";
		const char *term = strchr(++sep, (unsigned) '\t'); // just to be sure
		if (term)
			continue;
		tFileNdir key(sep, dir);

		if (bForceInsert)
		{
			// add with timestamp from the last century (implies removal later)
			m_trashCandSet[key].nLostAt = 1;
			continue;
		}

		tS2DAT::iterator it = m_trashCandSet.find(key);

		// file is in trash candidates now and was back then, set the old date
		if (it != m_trashCandSet.end())
			it->second.nLostAt = timestamp;

#if 0
		if(timestamp < oldest)
		oldest=timestamp;
		if(timestamp > newest)
		newest=timestamp;
#endif

	}

#if 0
	/*
	cout << "Unreferenced: ";
	for(trashmap_t::iterator it=m_trashCandidates.begin(); it!=m_trashCandidates.end(); it++)
		fprintf(stdout, "%lu\t%s\t%s\n",  it->second.first, it->second.second.c_str(), it->first.c_str());
	*/

	if(m_trashCandidates.size()>0)
	{
		// pretty printing for impatient users
		char buf[200];

		// map to to wait
		int nMax=acfg::extreshhold-((now-oldest)/86400);
		int nMin=acfg::extreshhold-((now-newest)/86400);

		snprintf(buf, _countof(buf), "Previously detected: %lu rotten package file(s), "
				"to be deleted in about %d-%d day(s)<br>\n",
				(unsigned long) m_trashCandidates.size(),
				nMin, nMax==nMin?nMax+1:nMax); // cheat a bit for the sake of code simplicity
		SendChunk(buf);
	}
#endif

}

inline bool expiration::CheckAndReportError()
{
	if (m_nErrorCount > 0 && m_bErrAbort)
	{
		SendChunk("<span class=\"ERROR\">Found errors during processing, "
				"aborting as requested.</span>");
		SendChunk(m_nPrevFailCount+(m_nErrorCount>0) > acfg::exsupcount
				? "<!-- TELL:THE:ADMIN -->"
						: "<!-- NOT:TELLING:THE:ADMIN:YET -->");
		return true;
	}
	return false;
}

