#include <jsonstats.h>

jsonstats::jsonstats(const tRunParms & parms) : expiration(parms)
{
}

jsonstats::~jsonstats()
{
}

#define CHECKABORT if(CheckStopSignal()) return;

void jsonstats::Run()
{
	auto origFD = m_parms.fd;
	m_parms.fd = -1; // no printing until we say so
	m_bIncompleteIsDamaged=StrHas(m_parms.cmd, "incomAsDamaged");
	DirectoryWalk(acfg::cachedir, this);
	CHECKABORT
	ProcessSeenMetaFiles(*this);
	CHECKABORT
	m_parms.fd = origFD;
	SendChunkedPageHeader("200", "application/json");
	PrintJson();
}


void jsonstats::PrintJson()
{
	SendChunk("{\n\t\"assignedSpace\": {\n");
	off_t total=0;
	for(auto &f: m_metaFilesRel)
	{
		if(f.second.space<=0)
			continue;
		if(total != 0)
			SendChunk(",\n");

		total += f.second.space;
		SendFmt << "\t\t\"" << f.first << "\": "  << f.second.space;
	}
	SendFmt<< "\n\t},\n\t\"total\": " << total << "\n}";
}


void jsonstats::Action()
{
}
