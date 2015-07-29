
//#define LOCAL_DEBUG
#include "debug.h"

#include "expiration.h"
#include "meta.h"
#include "filereader.h"
#include "fileio.h"

#include <fstream>

#include <unistd.h>
#include <dirent.h>

#include <map>
#include <string>
#include <iostream>
#include <algorithm>

using namespace std;

#define ENABLED

#ifndef ENABLED
#warning Unlinking parts defused
#endif

#define TIMEEXPIRED(t) (t < (m_gMaintTimeNow-acfg::extreshhold*86400))
#define TIME_AGONY (m_gMaintTimeNow-acfg::extreshhold*86400)

#define FNAME_PENDING "_expending_dat"
#define FNAME_DAMAGED "_expending_damaged"
#define sFAIL_INI SABSPATH("_exfail_cnt")
#define FAIL_INI sFAIL_INI.c_str()

void expiration::HandlePkgEntry(const tRemoteFileInfo &entry)
{
	LOGSTART2("expiration::_HandlePkgEntry:",
			"\ndir:" << entry.sDirectory << "\nname: " << entry.sFileName << "\nsize: " << entry.fpr.size << "\ncsum: " << entry.fpr.GetCsAsString());

#define ECLASS "<span class=\"ERROR\">ERROR: "
#define WCLASS "<span class=\"WARNING\">WARNING: "
#define GCLASS "<span class=\"GOOD\">OK: "
#define CLASSEND "</span><br>\n"

	off_t lenFromHeader=-1;

	// returns true if the file can be trashed, i.e. should stay in the list
	auto DetectUncovered =
			[&](cmstring& filenameHave, cmstring& sDirRel, tDiskFileInfo& descHave) -> bool
			{
				string sPathRel(sDirRel + filenameHave);
				if(ContHas(m_forceKeepInTrash,sPathRel))
					return true;
				string sPathAbs(CACHE_BASE+sPathRel);

				// end line ending starting from a class and add checkbox as needed
				auto finish_bad = [&]()->bool
				{
					if (m_damageList.is_open()) m_damageList << sPathRel << "\n";
					SendChunk(" (treating as damaged file...) ");
					AddDelCbox(sPathRel);
					SendChunk(CLASSEND);
					return true;
				};
#define ADDSPACE(x) SetFlags(m_processedIfile).space+=x
			// finishes a line with span, not leading to invalidating
			auto finish_good = [&](off_t size)->bool
			{
				SendFmt << CLASSEND;
				ADDSPACE(size);
				return false;
			};

			// like finish_good but prints the file, and only in verbose mode
			auto report_good = [&](off_t size)->bool
			{
				// ok, package matched, contents ok if checked, drop it from the removal list
				if (m_bVerbose) SendFmt << GCLASS << sPathRel << CLASSEND;
				ADDSPACE(size);
				return false;
			};

			// volatile files, not certain, print an action checkbox though
			auto report_weird_volatile = [&](off_t size)->bool
			{
				if(m_bVerbose)
				{
					SendFmt << WCLASS << sPathRel << " (invalid but volatile, ignoring...) ";
					AddDelCbox(sPathRel);
					SendChunk(CLASSEND);
				}
				ADDSPACE(size);
				return false;
			};

			Cstat realState(SABSPATH(sPathRel));
			if(!realState)
			{
				SendFmt << WCLASS "File not accessible, ignoring " << sPathRel << CLASSEND;
				return false;
			}
			off_t lenFromStat = realState.st_size;
			//SendFmt << "DBG-disk-size: " << lenFromStat;

			// those file were not updated by index handling, and are most likely not
			// matching their parent indexes. The best way is to see them as zero-sized and
			// handle them the same way.
			if(GetFlags(sPathRel).parseignore)
				goto handle_incomplete;

			// Basic header checks. Skip if the file was forcibly updated/reconstructed before.
			if (m_bSkipHeaderChecks || descHave.bNoHeaderCheck)
			{
				LOG("Skipped header check for " << sPathRel);
			}
			else if(entry.bInflateForCs)
			{
				LOG("Skipped header check for " << sPathRel << ", cannot compare sizes");
			}
			else if(entry.fpr.size>=0)
			{
				LOG("Doing basic header checks");
				header h;
				auto sHeadAbs(sPathAbs+".head");
				if (0<h.LoadFromFile(sHeadAbs))
				{
					lenFromHeader=atoofft(h.h[header::CONTENT_LENGTH], -2);
					if(lenFromHeader<0)
					{
						// better drop it, properly downloaded ones DO have the length
						SendFmt << WCLASS "header file of "
						<< sPathRel << " does not contain content length";
						return finish_bad();
					}
					if (lenFromHeader < lenFromStat)
					{
						ignore_value(::unlink(sHeadAbs.c_str()));

						SendFmt << ECLASS "header file of " << sPathRel
						<< " reported too small file size (" << lenFromHeader <<
						" vs. " << lenFromStat
						<< "); invalidating file, removing header now";
						return finish_bad();
					}
				}
				else
				{
					auto& at = GetFlags(sPathRel);
					if(!at.parseignore && !at.forgiveDlErrors) // just warn
					{
						SendFmt<< WCLASS "header file missing or damaged for "
						<<sPathRel << CLASSEND;
					}
				}
			}

			if(m_bByPath)
			{
				// can check a bit more against directory info for most cases
				// also shortcut without scanning

				if( !entry.bInflateForCs &&
						entry.fpr.size>=0)
				{
					if(lenFromStat > entry.fpr.size) goto report_oversize;
					if(lenFromStat < entry.fpr.size) goto handle_incomplete;
				}
			}

			if(!m_bByChecksum) return report_good(lenFromStat);

			//knowing the expected and real size, try a shortcut without scanning
			if(!entry.bInflateForCs// if we can check quickly with the file size
					&& entry.fpr.size >= 0)
			{
				if(lenFromStat<0) lenFromStat=GetFileSize(sPathAbs, -123);
				if(lenFromStat >=0 && lenFromStat < entry.fpr.size)
				{
					//		descHave.fpr.size=lenFromStat;
					goto handle_incomplete;
				}
			}

			if(entry.fpr.csType != descHave.fpr.csType &&
					!descHave.fpr.ScanFile(sPathAbs, entry.fpr.csType, entry.bInflateForCs))
			{
				// IO error? better keep it for now, not sure how to deal with it
				SendFmt << ECLASS "An error occurred while checksumming "
				<< sPathRel << ", leaving as-is for now.";
				if(entry.bInflateForCs)
					SendFmt << " NOTE: this can be caused by the incomplete compression"
							" header if the download was not finished.";
				aclog::err(tSS() << "Error reading " << sPathAbs );
				AddDelCbox(sPathRel);
				SendFmt<<CLASSEND;
				return false;
			}

			// ok, now fingerprint data must be consistent

			if(!descHave.fpr.csEquals(entry.fpr))
			{
				SendFmt << ECLASS << "checksum mismatch on " << sPathRel;
				return finish_bad();
			}

			// good, or cannot check so must be good
			if(entry.fpr.size<0 || (descHave.fpr.size == entry.fpr.size))
				return report_good(lenFromStat);

			// like the check above but this time we might compare also the uncompressed size
			// which is relevant for some types of vfiles
			if(descHave.fpr.size > entry.fpr.size)
			{
				report_oversize:
				/*
				 *
				 *
ERROR: size mismatch on debrep/dists/jessie/contrib/i18n/Translation-en.bz2 (adding to damage list)Tag
repro: check by path, after a minor distro upgrade
				 */
				// don't really care but let the user truncate
				if(rechecks::GetFiletype(sPathRel) == rechecks::FILE_VOLATILE)
					return report_weird_volatile(lenFromStat);

				SendFmt << ECLASS << "size mismatch on " << sPathRel;
				return finish_bad();
			}

			// all remaining cases mean an incomplete download
			handle_incomplete:

			if (!m_bIncompleteIsDamaged)
			{
				if(m_bVerbose)
				{
					SendFmt << WCLASS << sPathRel
							<< " (incomplete download, ignoring...) ";
					AddDelCbox(sPathRel);
					return finish_good(lenFromStat);
				}
				// just continue silently
				return report_good(lenFromStat);
			}

			// ok... considering damaged...
			if (m_bTruncateDamaged)
			{
				if(lenFromStat >0)
				{
					ignore_value(::truncate(sPathAbs.c_str(), 0));
					SendFmt << WCLASS << " incomplete download, truncating (as requested): "
					<< sPathRel;
					return finish_good(0);
				}
				// otherwise be quiet and don't care
				return false;
			}

			SendFmt << ECLASS << " incomplete download, invalidating (as requested) "<< sPathRel;
			return finish_bad();
		};

	auto rangeIt = m_trashFile2dir2Info.find(entry.sFileName);
	if (rangeIt == m_trashFile2dir2Info.end())
		return;

	auto loopFunc =
			[&](map<mstring,tDiskFileInfo>::iterator& it)
			{
				if (DetectUncovered(rangeIt->first, it->first, it->second))
				it++;
				else
				rangeIt->second.erase(it++);
			};

	// needs to match the exact file location.
	// And for "Index" files, they have always to be at a well defined location, this
	// constraint is also needed to expire deprecated files
	if(m_bByPath || entry.sFileName == sIndex)
	{
		// compare full paths (physical vs. remote) with their real paths
		auto cleanPath(entry.sDirectory);
		pathTidy(cleanPath);
		auto itEntry = rangeIt->second.find(cleanPath);
		if(itEntry == rangeIt->second.end()) // not found, ignore
			return;
		loopFunc(itEntry);
	}
	else for(auto it = rangeIt->second.begin(); it != rangeIt->second.end();)
		loopFunc(it);

}

// this method looks for the validity of additional package files kept in cache after
// the Debian version moved to a higher one. Still a very simple algorithm and may not work
// as expected when there are multiple Debian/Blends/Ubuntu/GRML/... branches inside with lots
// of gaps in the "package history" when proceeded in linear fashion.
inline void expiration::DropExceptionalVersions()
{
    if(m_trashFile2dir2Info.empty() || !acfg::keepnver)
    	return;
    if(system("dpkg --version >/dev/null 2>&1"))
	{
		SendFmt << "dpkg not available on this system, cannot identify latest versions to keep "
				"only " << acfg::keepnver << " of them.";
		return;
    }
    struct tPkgId
    {
    	mstring prevName, ver, prevArcSufx;
    	map<mstring,tDiskFileInfo>* group;
    	~tPkgId() {group=0;};

    	inline bool Set(cmstring& fileName, decltype(group) newgroup)
    	{
    		group = newgroup;
    		tSplitWalk split(&fileName, "_");
    		if(!split.Next())
    			return false;
    		prevName=split;
    		if(!split.Next())
    			return false;
    		ver=split;
    		for (const char *p = prevArcSufx.c_str(); *p; ++p)
    			if (!isalnum(uint(*p)) && !strchr(".-+:~", uint(*p)))
    				return false;
    		if(!split.Next())
    			return false;
    		prevArcSufx=split;
    		return !split.Next(); // no trailing crap there
    	}
    	inline bool SamePkg(tPkgId &other) const
    	{
    		return other.prevName == prevName && other.prevArcSufx == prevArcSufx;
    	}
    	// move highest versions to beginning
    	inline bool operator<(const tPkgId &other) const
    	{
    		int r=::system((string("dpkg --compare-versions ")+ver+" gt "+other.ver).c_str());
    		return 0==r;
    	}
    };
    vector<tPkgId> version2trashGroup;
    auto procGroup = [&]()
		{
    	// if more than allowed, keep the highest versions for sure, others are expired as usual
    	if(version2trashGroup.size() > (uint) acfg::keepnver)
        	std::sort(version2trashGroup.begin(), version2trashGroup.end());
    	for(uint i=0; i<version2trashGroup.size() && i<uint(acfg::keepnver); i++)
    		for(auto& j: * version2trashGroup[i].group)
    			j.second.nLostAt=m_gMaintTimeNow;
    	version2trashGroup.clear();
		};
    for(auto& trashFile2Group : m_trashFile2dir2Info)
    {
    	if(!endsWithSzAr(trashFile2Group.first, ".deb"))
			continue;
    	tPkgId newkey;
    	if(!newkey.Set(trashFile2Group.first, &trashFile2Group.second))
    		continue;
    	if(!version2trashGroup.empty() && !newkey.SamePkg(version2trashGroup.back()))
    		procGroup();
    	version2trashGroup.emplace_back(newkey);
    }
    if(!version2trashGroup.empty())
    	procGroup();
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
            SendChunk(WITHLEN("Unable to open " FNAME_PENDING
            		" for writing, attempting to recreate... "));
            ::unlink(sDbFileAbs.c_str());
            f.p=::fopen(sDbFileAbs.c_str(), "w");
            if(f)
                SendChunk(WITHLEN("OK\n<br>\n"));
            else
            {
                SendChunk(WITHLEN(
                		"<span class=\"ERROR\">"
                		"FAILED. ABORTING. Check filesystem and file permissions."
                		"</span>"));
                return;
            }
        }
    }

	int nCount(0);
	off_t tagSpace(0);

	for (auto& fileGroup : m_trashFile2dir2Info)
	{
		for (auto& dir_props : fileGroup.second)
		{
			string sPathRel = dir_props.first + fileGroup.first;
			auto& desc = dir_props.second;
			DBGQLOG("Checking " << sPathRel);
			using namespace rechecks;

			if (ContHas(m_forceKeepInTrash, sPathRel))
			{
				LOG("forcetrash flag set, whitelist does not apply, not to be removed");
			}
			else if (Match(fileGroup.first, FILE_WHITELIST) || Match(sPathRel, FILE_WHITELIST))
			{
				LOG("Protected file, not to be removed");
				continue;
			}

			if (dir_props.second.nLostAt<=0) // heh, accidentally added?
				continue;
			//cout << "Unreferenced: " << it->second.sDirname << it->first <<endl;

			string sPathAbs = SABSPATH(sPathRel);

			if (bPurgeNow || TIMEEXPIRED(dir_props.second.nLostAt))
			{
				SendFmt << "Removing " << sPathRel << "<br>\n";

#ifdef ENABLED
				::unlink(sPathAbs.c_str());
				::unlink((sPathAbs + ".head").c_str());
				::rmdir(SZABSPATH(dir_props.first));
#endif
			}
			else if (f)
			{
				SendFmt << "Tagging " << sPathRel;
				if (m_bVerbose)
					SendFmt << " (t-" << (m_gMaintTimeNow - desc.nLostAt) / 3600 << "h)";
				SendChunk("<br>\n");

				nCount++;
				tagSpace += desc.fpr.size;
				fprintf(f, "%lu\t%s\t%s\n",
						(unsigned long) desc.nLostAt,
						dir_props.first.c_str(),
						fileGroup.first.c_str());
			}
		}
	}
    if(nCount)
    	TellCount(nCount, tagSpace);
}


void expiration::Action()
{
	if (m_parms.type==workExPurge)
	{
		LoadPreviousData(true);
		RemoveAndStoreStatus(true);
		return;
	}
	if (m_parms.type==workExList)
	{
		LoadPreviousData(true);
		off_t nSpace(0);
		uint cnt(0);
		for (auto& i : m_trashFile2dir2Info)
		{
			for (auto& j : i.second)
			{
				auto rel = (j.first + i.first);
				auto abspath = SABSPATH(rel);
				off_t sz = GetFileSize(abspath, -2);
				if (sz < 0)
					continue;

				cnt++;
				SendChunk(rel + "<br>\n");
				nSpace += sz;

				sz = GetFileSize(abspath + ".head", -2);
				if (sz >= 0)
				{
					nSpace += sz;
					SendChunk(rel + ".head<br>\n");
				}
			}
		}
		TellCount(cnt, nSpace);

		mstring delURL(m_parms.cmd);
		StrSubst(delURL, "justShow", "justRemove");
		SendFmtRemote << "<a href=\""<<delURL<<"\">Delete all listed files</a> "
				"(no further confirmation)<br>\n";
		return;
	}

	if(m_parms.type==workExPurgeDamaged || m_parms.type==workExListDamaged || m_parms.type==workExTruncDamaged)
	{
		filereader f;
		if(!f.OpenFile(SABSPATH(FNAME_DAMAGED)))
		{
			SendChunk(WITHLEN("List of damaged files not found"));
			return;
		}
		mstring s;
		while(f.GetOneLine(s))
		{
			if(s.empty())
				continue;

			if(m_parms.type == workExPurgeDamaged)
			{
				SendFmt << "Removing " << s << "<br>\n";
				::unlink(SZABSPATH(s));
				::unlink(SZABSPATH(s+".head"));
			}
			else if(m_parms.type == workExTruncDamaged)
			{
				SendFmt << "Truncating " << s << "<br>\n";
				ignore_value(::truncate(SZABSPATH(s), 0));
			}
			else
				SendFmt << s << "<br>\n";
		}
		return;
	}

	SetCommonUserFlags(m_parms.cmd);

	m_bIncompleteIsDamaged=StrHas(m_parms.cmd, "incomAsDamaged");
	m_bScanVolatileContents=StrHas(m_parms.cmd, "scanVolatile");

	SendChunk("<b>Locating potentially expired files in the cache...</b><br>\n");

	//dump_proc_status();
	DirectoryWalk(acfg::cachedir, this);
	//dump_proc_status();

	if(CheckStopSignal())
		goto save_fail_count;
	SendFmt<<"Found "<<m_nProgIdx<<" files.<br />\n";

#if 0 //def DEBUG
	for(auto& i: m_trashFile2dir2Info)
	{
		SendFmt << "<br>File: " << i.first <<"<br>\n";
		for(auto& j: i.second)
			 SendFmt << "Dir: " << j.first << " [ "<<j.second.fpr.size << " / " << j.second.nLostAt << " ]<br>\n";
	}
#endif

/*	if(m_bByChecksum)
		m_fprCache.rehash(1.25*m_trashFile2dir2Info.size());
*/
	//cout << "found package files: " << m_trashCandidates.size()<<endl;
	//for(tS2DAT::iterator it=m_trashCandSet.begin(); it!=m_trashCandSet.end(); it++)
	//	SendChunk(tSS()<<it->second.sDirname << "~~~" << it->first << " : " << it->second.fpr.size<<"<br>");

	LoadHints();
	UpdateVolatileFiles();

	if(CheckAndReportError() || CheckStopSignal())
		goto save_fail_count;

	m_damageList.open(SZABSPATH(FNAME_DAMAGED), ios::out | ios::trunc);

	SendChunk(WITHLEN("<b>Validating cache contents...</b><br>\n"));
	ProcessSeenMetaFiles(*this);

	if(CheckAndReportError() || CheckStopSignal())
		goto save_fail_count;

	// update timestamps of pending removals
	LoadPreviousData(false);

	SendChunk(WITHLEN("<b>Reviewing candidates for removal...</b><br>\n"));
	RemoveAndStoreStatus(StrHas(m_parms.cmd, "purgeNow"));
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
				SendChunk(WITHLEN("OK\n<br>\n"));
			else
			{
				SendChunk(WITHLEN("<span class=\"ERROR\">FAILED. ABORTING. "
						"Check filesystem and file permissions.</span>"));
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
		SendChunk(WITHLEN(
				"Found required cleanup tasks: purging maintenance logs...<br>\n"));
	for (const auto &s: logs)
	{
		time_t id = atoofft(s.c_str() + acfg::logdir.size() + 7);
		//cerr << "id ist: "<< id<<endl;
		if (id == GetTaskId())
			continue;
		//cerr << "Remove: "<<globbuf.gl_pathv[i]<<endl;
#ifdef ENABLED
		::unlink(s.c_str());
#endif
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
	if(sPathRel.find_first_of("\r\n'\"<>{}")!=stmiss)
		return true;

	if(sPathRel[0] == '_' && !m_bScanInternals)
		return true; // not for us

	// special handling for the installer files, we need an index for them which might be not there
	tStrPos pos2, pos = sPathRel.rfind("/installer-");
	if(pos!=stmiss && stmiss !=(pos2=sPathRel.find("/images/", pos)))
	{
		auto idir = sPathRel.substr(0, pos2 + 8);
		auto& flags = m_metaFilesRel[idir +"SHA256SUMS"];

		/* pretend that it's there but not usable so the refreshing code will try to get at
		 * least one copy for that location if it's needed there
		 */
		if(!flags.vfile_ondisk)
		{
			flags.eIdxType = EIDX_SHA256DILIST;
			flags.vfile_ondisk = true;
			flags.uptodate = false;
		}
		// and last but not least - care only about the modern version of that index
		m_metaFilesRel.erase(idir + "MD5SUMS");
	}
	uint stripLen=0;
    if (endsWithSzAr(sPathRel, ".head"))
		stripLen=5;
	else if (AddIFileCandidate(sPathRel))
	{
		auto &attr = SetFlags(sPathRel);
		attr.space += stinfo.st_size;
		attr.forgiveDlErrors = endsWith(sPathRel, sslIndex);
	}
	else if (rechecks::Match(sPathRel, rechecks::FILE_VOLATILE))
		return true; // cannot check volatile files properly so don't care

	// ok, split to dir/file and add to the list
	tStrPos nCutPos = sPathRel.rfind(CPATHSEP);
	nCutPos = (nCutPos == stmiss) ? 0 : nCutPos+1;

	auto& finfo = m_trashFile2dir2Info[sPathRel.substr(nCutPos, sPathRel.length() - stripLen - nCutPos)]
	                                   [sPathRel.substr(0, nCutPos)];
	finfo.nLostAt = m_gMaintTimeNow;
	// remember the size for content data, ignore for the header file
	if(!stripLen)
		finfo.fpr.size = stinfo.st_size;

	return true;
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

	while(reader.GetOneLine(sLine))
	{
		char *eptr(nullptr);
		auto s = sLine.c_str();
		time_t timestamp = strtoull(s, &eptr, 10);
		if (!eptr || *eptr != '\t' || !timestamp || timestamp > m_gMaintTimeNow) // where is the DeLorean?
			continue;
		auto sep = strchr(++eptr, (unsigned) '\t');
		if (!sep)
			continue;
		string dir(eptr, sep - eptr);
		if (!dir.empty() && '/' != *(sep - 1))
			dir += "/";
		auto term = strchr(++sep, (unsigned) '\t'); // just to be sure
		if (term)
			continue;

		auto& desc = m_trashFile2dir2Info[sep][dir];
		// maybe add with timestamp from the last century (implies removal later)
		if(bForceInsert)
			desc.nLostAt=1;
		// considered file was already considered garbage, use the old date
		else if(desc.nLostAt>0)
			desc.nLostAt = timestamp;
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
		SendFmt << sAbortMsg <<
				((m_nPrevFailCount+(m_nErrorCount>0) > acfg::exsupcount)
				? "<!-- TELL:THE:ADMIN -->"
						: "<!-- NOT:TELLING:THE:ADMIN:YET -->");
		return true;
	}
	return false;
}

