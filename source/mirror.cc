/*
 * mirror.cpp
 *
 *  Created on: 25.11.2010
 *      Author: ed
 */


#define LOCAL_DEBUG
#include "debug.h"

#include "mirror.h"
#include "header.h"
#include "dirwalk.h"
#include "meta.h"
#include "acfg.h"

#include <fnmatch.h>
#include <algorithm>

using namespace MYSTD;

bool pkgmirror::ProcessRegular(const string &sPath, const struct stat &stinfo)
{
	if (endsWithSzAr(sPath, ".head"))
		return true;

	if (sPath.size() <= CACHE_BASE_LEN+1) // heh?
		return false;

	mstring sPathRel(sPath, CACHE_BASE_LEN);

	if (sPathRel[0] == '_')
		return true; // not for us, also skips _import

	ProgTell();

	AddIFileCandidate(sPathRel);

	if(m_bAsNeeded)
	{
		tStrPos pos = sPathRel.rfind('/');
		if(stmiss != pos)
		{
			// also include the base package name for Debian-like packages? otherwise cut after /
			tStrPos usPos=sPathRel.find("_", pos);
			mstring filtStr(sPathRel, 0, (usPos!=stmiss && usPos>pos) ? usPos : pos+1);

#ifdef DEBUG
			pair<tStrSet::iterator,bool> res= m_pathFilter.insert(filtStr);
			if(res.second)
				SendFmt << "target cand: " << filtStr << "<br>\n";
#else
			m_pathFilter.insert(filtStr);
#endif
		}
	}

	return ! CheckStopSignal();
}

tStrDeq GetVariants(cmstring &mine)
{
	tStrDeq ret;
	string base;
	for(cmstring *p=compSuffixesAndEmpty; p<compSuffixesAndEmpty+_countof(compSuffixesAndEmpty); p++)
	{
		if(endsWith(mine, *p))
		{
			base=mine.substr(0, mine.size()-p->size());
			break;
		}
	}
	for(cmstring *p=compSuffixesAndEmpty; p<compSuffixesAndEmpty+_countof(compSuffixesAndEmpty); p++)
	{
		string cand=base+*p;
		if(cand!=mine)
			ret.push_back(cand);
	}
	return ret;
}

void pkgmirror::Action(const string &cmd)
{
	if(acfg::mirrorsrcs.empty())
	{
		SendChunk("<b>PrecacheFor not set, check configuration!</b><br>\n");
		return;
	}

	SendChunk("<b>Locating index files, scanning...</b><br>\n");

	SetCommonUserFlags(cmd);
	m_bErrAbort=false; // does not f...ing matter, do what we can

	m_bCalcSize=(cmd.find("calcSize=cs")!=stmiss);
	m_bDoDownload=(cmd.find("doDownload=dd")!=stmiss);
	m_bSkipIxUpdate=(cmd.find("skipIxUp=si")!=stmiss);
	m_bAsNeeded=(cmd.find("asNeeded=an")!=stmiss);
	m_bUseDelta=(cmd.find("useDebDelta=ud")!=stmiss);

	if(m_bUseDelta)
	{
		if(::system("dpkg --version"))
		{
			SendChunk("<b>dpkg not found, Debdelta support disabled</b><br>\n");
			m_bUseDelta=false;
		}
		else if(::system("debpatch -h"))
		{
			SendChunk("<b>debpatch not found, Debdelta support disabled</b><br>\n");
			m_bUseDelta=false;
		}
	}
	if(m_bUseDelta)
		StartDlder();

	DirectoryWalk(acfg::cachedir, this);

	if(CheckStopSignal())
		return;

	if(m_indexFilesRel.empty())
	{
		SendChunk("<div class=\"ERROR\">No index files detected. Unable to continue, cannot map files to internal locations.</div>");
		return;
	}

	if(CheckStopSignal())
		return;

	if(!m_bSkipIxUpdate)
		UpdateIndexFiles();

	if(CheckStopSignal())
		return;

	// prepare wildcard matching and collecting
	tStrSet srcs;
	class __srcpicker : public ifileprocessor
	{
	public:
		tStrSet *pSrcs;
		tStrVec matchList;
		__srcpicker(tStrSet *x) : pSrcs(x)
		{
			Tokenize(acfg::mirrorsrcs, SPACECHARS, matchList);
		};
		void TryAdd(cmstring &s)
		{
			for(const auto& match : matchList)
				if(0==fnmatch(match.c_str(), s.c_str(), FNM_PATHNAME))
				{
					pSrcs->insert(s);
					break;
				}
		}
		void HandlePkgEntry(const tRemoteFileInfo &entry)
		{
			TryAdd(entry.sDirectory+entry.sFileName);
		}
	} picker(&srcs);

	mstring sErr;

	SendChunk("<b>Identifying relevant index files...</b><br>");
	// ok, now go through all release files and pickup all appropriate index files
	for(auto& path2x: m_indexFilesRel)
	{
		if(endsWithSzAr(path2x.first, "Release"))
		{
			if(!m_bSkipIxUpdate && !GetFlags(path2x.first).uptodate)
				Download(path2x.first, true, eMsgShow);
			ParseAndProcessIndexFile(picker, path2x.first, EIDX_RELEASE);
		}
		else
			picker.TryAdd(path2x.first);
	}

	SendChunk("<b>Identifying more index files in cache...</b><br>");
	// unless found in release files, get the from the local system
	for (const auto& match: picker.matchList)
		for(const auto& path : ExpandFilePattern(CACHE_BASE+match, false))
			picker.TryAdd(path);

	restart_clean: // start over if the set changed while having a hot iterator
	for(tStrSet::iterator it=srcs.begin(); it!=srcs.end(); it++)
	{
		if(GetFlags(*it).uptodate) // this is the one
		{
		tStrDeq bros(GetVariants(*it));
		int nDeleted=0;
		for(tStrDeq::iterator b=bros.begin(); b!=bros.end(); b++)
			nDeleted+=srcs.erase(*b);
		if(nDeleted)
			goto restart_clean;
		}
	}

	// now there may still be something like Sources and Sources.bz2 if they
	// were added by Release file scan. Choose the prefered simply by extension.

	restart_clean2: // start over if the set changed while having a hot iterator
	for (const string *p = compSuffixesAndEmptyByRatio; p < compSuffixesAndEmptyByRatio
	+ _countof(compSuffixesAndEmptyByRatio); p++)
	{
		for (tStrSet::iterator it = srcs.begin(); it != srcs.end(); it++)
		{
			if(endsWith(*it, *p))
			{
				tStrDeq bros(GetVariants(*it));
			int nDeleted=0;
			for(tStrDeq::iterator b=bros.begin(); b!=bros.end(); b++)
				nDeleted+=srcs.erase(*b);
			if(nDeleted)
				goto restart_clean2;
			}
		}
	}

	for (auto& src: srcs)
	{
		SendFmt << "File list: " << src << "<br>\n";

		if(m_bSkipIxUpdate)
			continue;

		if(!GetFlags(src).uptodate)
			Download(src, true, eMsgShow);

		if(CheckStopSignal())
			return;
	}

	m_totalHave = m_totalSize = 0;


	if (m_bCalcSize)
	{

		UINT dcount=0;

		SendFmt << "<b>Counting downloadable content size..."
				<< (m_bAsNeeded? " (filtered)" : "")  << "</b><br>";

		for (auto& src : srcs)
		{
#ifdef DEBUG
			if(LOG_MORE&acfg::debug)
				SendFmt << "mirror check: " << src;
#endif
			off_t needBefore=(m_totalSize-m_totalHave);

			ParseAndProcessIndexFile(*this, src, GuessIndexTypeFromURL(src));

			SendFmt << src << ": "
					<< offttosH((m_totalSize-m_totalHave)-needBefore)
					<< " to download<br>\n";

			if(m_bUseDelta)
				dcount+=ConfigDelta(src);

			if(CheckStopSignal())
				return;
		}
		SendFmt << "Total size: " << offttosH(m_totalSize) << ", to download: about "
				<< offttosH(m_totalSize-m_totalHave) << "<br>\n";

		if(m_bUseDelta && !dcount)
			SendChunk("WARNING: <b>no deltasrc setting was found for any specified source</b><br>\n");
	}

	if(m_bDoDownload && (!m_bCalcSize || m_totalSize!=m_totalHave))
	{
		SendFmt << "<b>Starting download...</b><br>";

		m_bCalcSize=false;

		for (auto& src: srcs)
		{
			if(CheckStopSignal())
				return;
			ConfigDelta(src);
			ParseAndProcessIndexFile(*this, src, GuessIndexTypeFromURL(src));
		}
	}
}

inline bool pkgmirror::ConfigDelta(cmstring &sPathRel)
{
	// ok... having a source for deltas?

	m_pDeltaSrc = NULL;
	m_repCutLen = 0;

	if (!m_bUseDelta)
		return false;

	DelTree(SABSPATH("_actmp"));
	mstring vname = sPathRel;
	m_repCutLen = vname.find("/");
	if (m_repCutLen != stmiss)
	{
		vname.resize(m_repCutLen);
		const acfg::tRepoData *pRepo = acfg::GetBackendVec(vname);
#ifdef DEBUG
		if(acfg::debug & LOG_MORE)
		{
			if(!pRepo)
				SendFmt << "hm, no delta provider for " << sPathRel;
		}
		else
			SendFmt<< "hm, delta for " << sPathRel << " is what? " <<pRepo->m_deltasrc.sHost;
#endif
		if (pRepo && !pRepo->m_deltasrc.sHost.empty())
			m_pDeltaSrc = &pRepo->m_deltasrc;
	}
	return m_pDeltaSrc;
}

bool CompDebVerLessThan(cmstring &s1, cmstring s2)
{
	int r=::system(string("dpkg --compare-versions "+s1+" lt "+s2).c_str());
	return 0==r;
};

void pkgmirror::HandlePkgEntry(const tRemoteFileInfo &entry)
{
	if (m_bAsNeeded)
	{
		mstring filter = entry.sDirectory;
		tStrPos pos = entry.sFileName.find('_');
		if (pos != stmiss)
			filter.append(entry.sFileName, 0, pos);
//#ifdef DEBUG
//		SendFmt << "filter: " << filter << "<br>";
//#endif

		if (!ContHas(m_pathFilter, filter))
			return;
	}

	cmstring tgtRel = entry.sDirectory + entry.sFileName;
	cmstring targetAbs = SABSPATH(tgtRel);
	off_t haveSize=GetFileSize(targetAbs, 0);

	if(m_bCalcSize)
	{
		m_totalSize += entry.fpr.size;
		m_totalHave += haveSize;
	}
	else
	{
		bool bhaveit = (haveSize == entry.fpr.size);

		if(!bhaveit && m_pDeltaSrc && endsWithSzAr(entry.sFileName, ".deb")
		&& entry.sDirectory.size() > m_repCutLen && CPATHSEP == entry.sDirectory[m_repCutLen]
		&& haveSize <= (entry.fpr.size/8)*7 ) // don't patch if original file is almost complete
		{
			tStrDeq oldebs, sorted;

			//MYSTD::set<mstring, CompDebVerLessThan> sortedVersions;

			tStrVec parts;
			Tokenize(entry.sFileName, "_", parts);
			if(parts.size() != 3)
				goto cannot_debpatch;

			// pick only the same architecture
			oldebs = ExpandFilePattern(CACHE_BASE + entry.sDirectory + parts[0] + "_*_" + parts[2], false);

			// filter dangerous strings, invalid version strings, higher/same version
			for(UINT i=0; i<oldebs.size();++i)
			{
				tSplitWalk split(&oldebs[i], "_");
				if(split.Next() && split.Next())
				{
					mstring s(split);
					const char *p = s.c_str();
					if(!p || !*p || !isdigit(UINT(*p)))
						continue;
					for(++p; *p; ++p)
						if(!isalnum(UINT(*p)) && !strchr(".-+:~",UINT(*p)))
							break;
					if( !*p && CompDebVerLessThan(s, parts[1]))
						sorted.push_back(s);
				}
			}

			if(sorted.empty())
				goto cannot_debpatch;

			sort(sorted.begin(), sorted.end(), CompDebVerLessThan);

#ifdef DEBUG
			SendFmt << "Found " << sorted.size() << " from " << oldebs.size() << " debs suitable for patching<br>";
#endif
			while(!sorted.empty() && !bhaveit)
			{
				tSS uri, srcAbs;
				uri << m_pDeltaSrc->ToURI(false) << entry.sDirectory.substr(m_repCutLen+1)
						<< parts[0] << "_"
						<< sorted.back() << "_" <<
						parts[1] << "_" << parts[2] << "delta";

#ifdef DEBUG
				SendFmt << uri << "<br>\n";
#endif
				srcAbs << CACHE_BASE << entry.sDirectory<< parts[0] << "_"
						<< sorted.back() << "_"  << parts[2];

				sorted.pop_back();

#define TEMPDELTA "_actmp/debdelta"
#define TEMPRESULT TEMPDELTA ".result.deb"

				cmstring sDeltaPathAbs(SZABSPATH(TEMPDELTA));

				::unlink(sDeltaPathAbs.c_str());
				::unlink((sDeltaPathAbs+".head").c_str());

				if(Download(TEMPDELTA, false, eMsgHideAll, NULL, uri.c_str()))
				{
					::setenv("delta", SZABSPATH(TEMPDELTA), true);
					::setenv("from", srcAbs.c_str(), true);
					::setenv("to", SZABSPATH(TEMPRESULT), true);

				SendFmt << "Fetched: " << uri << "<br>\n";
				cerr << "debpatch " << getenv("delta") << " " << getenv("from")
						<< " " << getenv("to");

					if (0 == ::system("debpatch \"$delta\" \"$from\" \"$to\""))
					{
						header h;
						if (haveSize && h.LoadFromFile(targetAbs + ".head") > 0
								&& atoofft(h.h[header::CONTENT_LENGTH], -2) == entry.fpr.size)
						{
							//LOG("keeping original head data");
						}
						else
						{
							h.frontLine = "HTTP/1.1 200 OK";
							h.set(header::LAST_MODIFIED, FAKEDATEMARK);
							h.set(header::CONTENT_LENGTH, entry.fpr.size);

							// construct x-orig from original head
							srcAbs << ".head";
							header ho;
							if (ho.LoadFromFile(srcAbs.c_str()) > 0 && ho.h[header::XORIG])
							{
								mstring xo(ho.h[header::XORIG]);
								tStrPos pos = xo.rfind(sPathSep);
								if (pos < xo.size())
								{
									xo.replace(pos + 1, xo.size(), entry.sFileName);
									h.set(header::XORIG, xo);
								}
							}
						}

						bhaveit = Inject(TEMPRESULT, tgtRel, false, &h, true);

						if(bhaveit)
						{
							off_t nPatchTransferSize = GetFileSize(sDeltaPathAbs, -123);
							SendFmt << "Rebuilt " << tgtRel << " using Debdelta "
									<< "<i>(" << nPatchTransferSize / 1024
									<< ":" << entry.fpr.size / 1024
									<< "KiB)</i>\n<br>\n";
						}
					}
					else
						SendFmt << "Debpatch couldn't rebuild " << tgtRel<<"<br>\n";
				}
			}
		}

		cannot_debpatch:

		if(!bhaveit)
			Download(entry.sDirectory + entry.sFileName, false, eMsgShow);

		if (m_bVerbose && m_totalSize)
		{
			off_t newSize = GetFileSize(CACHE_BASE + entry.sDirectory + entry.sFileName, 0);
			if (haveSize != newSize)
			{
				m_totalSize -= (newSize - haveSize);
				SendFmt << "Remaining download size: " << offttosH(m_totalSize) << "<br>\n";
			}
		}
	}
}
