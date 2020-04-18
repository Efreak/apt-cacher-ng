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
#include <list>

using namespace std;

namespace acng
{
bool pkgmirror::ProcessRegular(const string &sPath, const struct stat &)
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

void pkgmirror::Action()
{
	if(cfg::mirrorsrcs.empty())
	{
		SendChunk("<b>PrecacheFor not set, check configuration!</b><br>\n");
		return;
	}

	SendChunk("<b>Locating index files, scanning...</b><br>\n");

	m_bCalcSize=(m_parms.cmd.find("calcSize=cs")!=stmiss);
	m_bDoDownload=(m_parms.cmd.find("doDownload=dd")!=stmiss);
	m_bAsNeeded=(m_parms.cmd.find("asNeeded=an")!=stmiss);
	m_bUseDelta=(m_parms.cmd.find("useDebDelta=ud")!=stmiss);

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
	{
		if(!StartDlder())
			return;
	}
	BuildCacheFileList();

	if(CheckStopSignal())
		return;

	if(m_metaFilesRel.empty())
	{
		SendChunk("<div class=\"ERROR\">No index files detected. Unable to continue, cannot map files to internal locations.</div>");
		return;
	}

	if(CheckStopSignal())
		return;

	if(!m_bSkipIxUpdate)
		UpdateVolatileFiles();

	if(CheckStopSignal())
		return;

	// prepare wildcard matching and collecting
	tStrSet srcs;
	tStrVec matchList;
	Tokenize(cfg::mirrorsrcs, SPACECHARS, matchList);
	auto TryAdd = [&matchList, &srcs](cmstring &s)
			{
				for(const auto& match : matchList)
					if(0==fnmatch(match.c_str(), s.c_str(), FNM_PATHNAME))
					{
#ifdef COMPATGCC47
						srcs.insert(s);
#else
						srcs.emplace(s);
#endif
						break;
					}
			};

	mstring sErr;

	SendChunk("<b>Identifying relevant index files...</b><br>");
	// ok, now go through all release files and pickup all appropriate index files
	for(auto& path2x: m_metaFilesRel)
	{
		if(endsWithSzAr(path2x.first, "Release"))
		{
			if(!m_bSkipIxUpdate && !GetFlags((cmstring)path2x.first).uptodate)
				Download((cmstring)path2x.first, true, eMsgShow);
			ParseAndProcessMetaFile([&TryAdd](const tRemoteFileInfo &entry) {
				TryAdd(entry.sDirectory+entry.sFileName); },
				(cmstring) path2x.first, EIDX_RELEASE);
		}
		else
			TryAdd((cmstring)path2x.first);
	}

	SendChunk("<b>Identifying more index files in cache...</b><br>");
	// unless found in release files, get the from the local system
	for (const auto& match: matchList)
		for(const auto& path : ExpandFilePattern(CACHE_BASE+match, false))
			TryAdd(path);

	auto delBros = [&srcs](cmstring& mine)
	{
		string base;
		int nDeleted = 0;
		cmstring* pMySuf=nullptr;
		for(const auto& suf: sfxXzBz2GzLzmaNone)
		{
			if(endsWith(mine, suf))
			{
				base=mine.substr(0, mine.length()-suf.length());
				pMySuf=&suf;
				break;
			}
		}
		for(const auto& suf : sfxXzBz2GzLzmaNone)
		{
			if(&suf == pMySuf)
				continue;
			nDeleted += srcs.erase(base+suf);
		}
		return nDeleted;
	};

	// ok, if there are still multiple variants of certain files, strip the
	// ones which are equivalent to some file which we have on disk

	// start over if the set changed while having a hot iterator
	// XXX: this is still O(n^2) but good enough for now
	restart_clean:
	for(const auto& src : srcs)
		if (GetFlags(src).uptodate && delBros(src)) // this is the one
			goto restart_clean;

	// now there may still be something like Sources and Sources.bz2 if they
	// were added by Release file scan. Choose the preferred one simply by extension.
	restart_clean2: // start over if the set changed while having a hot iterator
	for (const auto& s: sfxXzBz2GzLzma)
		for (const auto& src : srcs)
			if (endsWith(src, s)&& delBros(src)) // this is the one
				goto restart_clean2;

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

		unsigned dcount=0;

		SendFmt << "<b>Counting downloadable content size..."
				<< (m_bAsNeeded? " (filtered)" : "")  << "</b><br>";

		for (auto& src : srcs)
		{
#ifdef DEBUG
			if(log::LOG_MORE & cfg::debug)
				SendFmt << "mirror check: " << src;
#endif
			off_t needBefore=(m_totalSize-m_totalHave);

			ParseAndProcessMetaFile([this](const tRemoteFileInfo &e) {
				HandlePkgEntry(e); }, src, GuessMetaTypeFromURL(src));

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
			ParseAndProcessMetaFile([this](const tRemoteFileInfo &e) {
				HandlePkgEntry(e); }, src, GuessMetaTypeFromURL(src));
		}
	}
}

inline bool pkgmirror::ConfigDelta(cmstring &sPathRel)
{
	// ok... having a source for deltas?

	m_pDeltaSrc = nullptr;
	m_repCutLen = 0;

	if (!m_bUseDelta)
		return false;

	DelTree(SABSPATH("_actmp"));
	mstring vname = sPathRel;
	m_repCutLen = vname.find("/");
	if (m_repCutLen != stmiss)
	{
		vname.resize(m_repCutLen);
		const cfg::tRepoData *pRepo = cfg::GetRepoData(vname);
#ifdef DEBUG
		if(cfg::debug & log::LOG_MORE)
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

			//std::set<mstring, CompDebVerLessThan> sortedVersions;

			tStrVec parts;
			Tokenize(entry.sFileName, "_", parts);
			if(parts.size() != 3)
				goto cannot_debpatch;

			// pick only the same architecture
			oldebs = ExpandFilePattern(CACHE_BASE +
					entry.sDirectory + parts[0] + "_*_" + parts[2], false);

			// filter dangerous strings, invalid version strings, higher/same version
			for(const auto& oldeb: oldebs)
			{
				tSplitWalk split(&oldeb, "_");
				if(split.Next() && split.Next())
				{
					mstring s(split);
					const char *p = s.c_str();
					if(!p || !*p || !isdigit(uint(*p)))
						continue;
					for(++p; *p; ++p)
						if(!isalnum(uint(*p)) && !strchr(".-+:~",uint(*p)))
							break;
					if( !*p && CompDebVerLessThan(s, parts[1]))
						sorted.emplace_back(s);
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
				tSS srcAbs;
				auto uri=*m_pDeltaSrc;
				uri.sPath+=entry.sDirectory.substr(m_repCutLen+1)
						+ parts[0] + "_"
						+ sorted.back() + "_" +
						parts[1] + "_" + parts[2] + "delta";

#ifdef DEBUG
				SendFmt << uri.ToURI(false) << "<br>\n";
#endif
				srcAbs << CACHE_BASE << entry.sDirectory<< parts[0] << "_"
						<< sorted.back() << "_"  << parts[2];

				sorted.pop_back();

#define TEMPDELTA "_actmp/debdelta"
#define TEMPRESULT TEMPDELTA ".result.deb"

				cmstring sDeltaPathAbs(SZABSPATH(TEMPDELTA));

				::unlink(sDeltaPathAbs.c_str());
				::unlink((sDeltaPathAbs+".head").c_str());

				if(Download(TEMPDELTA, false, eMsgHideAll, nullptr, &uri))
				{
					::setenv("delta", SZABSPATH(TEMPDELTA), true);
					::setenv("from", srcAbs.c_str(), true);
					::setenv("to", SZABSPATH(TEMPRESULT), true);

				SendFmt << "Fetched: " << uri.ToURI(false) << "<br>\n";
				if(m_bVerbose)
				SendFmt << "debpatch " << getenv("delta") << " " << getenv("from")
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

}
