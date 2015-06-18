
//#define LOCAL_DEBUG
#include "debug.h"

#include "acfg.h"

#include "header.h"
#include "config.h"
#include <acbuf.h>

#include <cstdio>
#include <iostream>
#include <string.h>
#include <unistd.h>

#include "fileio.h"
#include "filereader.h"

#include <map>

using namespace std;

struct eHeadPos2label
{
	header::eHeadPos pos;
	const char *str;
	size_t len;
};

eHeadPos2label mapId2Headname[] =
{
		{ header::LAST_MODIFIED, WITHLEN("Last-Modified")},
		{ header::CONTENT_LENGTH, WITHLEN("Content-Length")},
		{ header::CONNECTION, WITHLEN("Connection")},
		{ header::CONTENT_TYPE, WITHLEN("Content-Type")},
		{ header::IF_MODIFIED_SINCE, WITHLEN("If-Modified-Since")},
		{ header::RANGE, WITHLEN("Range")},
		{ header::IFRANGE, WITHLEN("If-Range")},
		{ header::CONTENT_RANGE, WITHLEN("Content-Range")},
		{ header::PROXY_CONNECTION, WITHLEN("Proxy-Connection")},
		{ header::TRANSFER_ENCODING, WITHLEN("Transfer-Encoding")},
		{ header::AUTHORIZATION, WITHLEN("Authorization")},
		{ header::LOCATION, WITHLEN("Location")},
		{ header::XFORWARDEDFOR, WITHLEN("X-Forwarded-For")},
		{ header::XORIG, WITHLEN("X-Original-Source")}
};

std::vector<tPtrLen> header::GetKnownHeaders()
{
	std::vector<tPtrLen> ret;
	ret.reserve(_countof(mapId2Headname));
	for (auto& x : mapId2Headname)
		ret.emplace_back(x.str, x.len);
	return ret;
}

#if 0 // nonsense... save a penny, waste an hour
struct tHeadLabelMap
{
	class noCaseComp
	{
	//	bool operator<(const tStringRef &a) { return strncasecmp(a.first, first, second)<0; }
	};
	map<pair<const char*,size_t>, header::eHeadPos> lookup;
	tHeadLabelMap()
	{
		//tHeadLabelMap &x=*this;
		insert(make_pair(tStringRef(NAMEWLEN("foo")), header::XORIG));
	}
} label_map;
#endif

header::header(const header &s)
:type(s.type),
 frontLine(s.frontLine)
{
	for (uint i = 0; i < HEADPOS_MAX; i++)
		h[i] = s.h[i] ? strdup(s.h[i]) : nullptr;
}

header& header::operator=(const header& s)
{
	type=s.type;
	frontLine=s.frontLine;
	for (uint i = 0; i < HEADPOS_MAX; ++i)
	{
		if (h[i])
			free(h[i]);
		h[i] = s.h[i] ? strdup(s.h[i]) : nullptr;
	}
	return *this;
}

header::~header()
{
	for(auto& p:h)
		free(p);
}

void header::clear()
{
	for(uint i=0; i<HEADPOS_MAX; i++)
		del((eHeadPos) i);
	frontLine.clear();
	type=INVALID;
}

void header::del(eHeadPos i)
{
	free(h[i]);
	h[i]=0;
}

int header::Load(LPCSTR const in, uint maxlen,
		std::vector<std::pair<std::string, std::string>> *pNotForUs)
{
	if(maxlen<9)
		return 0;

	if(!in)
		return -1;
	if(!strncmp(in,  "HTTP/1.", 7))
		type=ANSWER;
	else if(!strncmp(in, "GET ", 4))
		type=GET;
	else if (!strncmp(in, "HEAD ", 5))
		type=HEAD;
	else if (!strncmp(in, "POST ", 5))
		type=POST;
	else if (!strncmp(in, "CONNECT ", 8))
		type=CONNECT;
	else
		return -1;

	auto posNext=in;
	auto lastLineIdx = HEADPOS_MAX;

	while (true)
	{
		auto szBegin=posNext;
		uint pos=szBegin-in;
		auto end=(LPCSTR) memchr(szBegin, '\r', maxlen-pos);
		if (!end)
			return 0;
		if (end+1>=in+maxlen)
			return 0; // one newline must fit there, always

		if (szBegin==end)
		{
			if (end[1]=='\n') // DONE HERE!
				return end+2-in;

			return -1; // looks like crap
		}
		posNext=end+2;

		while (isspace((uint)*end))	end--;
		end++;
		
		if (frontLine.empty())
		{
			frontLine.assign(in, end-in);
			trimBack(frontLine);
			continue;
		}

		if(*szBegin == ' ' || *szBegin == '\t') // oh, a multiline?
		{
			auto nlen=end-szBegin;
			if(nlen<2) // empty but prefixed line, there might be continuation
				continue;

			if(lastLineIdx == HEADPOS_NOTFORUS)
			{
				if(pNotForUs)
				{
					if(pNotForUs->empty()) // heh?
						return -4;
					pNotForUs->back().second.append(szBegin, nlen+2);
				}
				continue;
			}
			else if(lastLineIdx == HEADPOS_MAX || !h[lastLineIdx])
				return -4;
			auto xl=strlen(h[lastLineIdx]);
			if( ! (h[lastLineIdx] = (char*) realloc(h[lastLineIdx], xl+nlen + 1)))
				continue;
			memcpy(h[lastLineIdx]+xl, szBegin, nlen);
			h[lastLineIdx][xl]=' ';
			h[lastLineIdx][xl+nlen]='\0';
			continue;
		}

		// end is on the last relevant char now
		const char *sep=(const char*) memchr(szBegin, ':', end-szBegin);
		if (!sep)
			return -1;
		
		auto key = szBegin;
		size_t keyLen=sep-szBegin;

		sep++;
		while (sep<end && isspace((uint)*sep))
			sep++;
		
		lastLineIdx = HEADPOS_NOTFORUS;

		for(const auto& xh : mapId2Headname)
		{
			if (xh.len != keyLen || key[xh.len] != ':' || strncasecmp(xh.str, key, keyLen))
				continue;
			uint l=end-sep;
			lastLineIdx = xh.pos;
			if( ! (h[xh.pos] = (char*) realloc(h[xh.pos], l+1)))
				return -3;
			memcpy(h[xh.pos], sep, l);
			h[xh.pos][l]='\0';
			break;
		}
		if(pNotForUs && lastLineIdx == HEADPOS_NOTFORUS)
			pNotForUs->emplace_back(string(key,keyLen),
					string(szBegin+keyLen, end+2-(szBegin+keyLen)));
	}
	return -2;
}

int header::LoadFromBuf(const char * const in, uint maxlen)
{
	clear();
	int ret=Load(in, maxlen);
	if(ret<0)
		clear();
	return ret;
}

int header::LoadFromFile(const string &sPath)
{
	clear();
#if 0
	filereader buf;
	return buf.OpenFile(sPath, true) && LoadFromBuf(buf.GetBuffer(), buf.GetSize());
#endif
	acbuf buf;
	if(!buf.initFromFile(sPath.c_str()))
		return -1;
	return LoadFromBuf(buf.rptr(), buf.size());
}


void header::set(eHeadPos i, const char *val)
{
	if (h[i])
	{
		free(h[i]);
		h[i]=nullptr;
	}
	if(val)
		h[i] = strdup(val);
}

void header::set(eHeadPos i, const char *val, size_t len)
{
	if(!val)
	{
		free(h[i]);
		h[i]=nullptr;
		return;
	}
	h[i] = (char*) realloc(h[i], len+1);
	if(h[i])
	{
		memcpy(h[i], val, len);
		h[i][len]='\0';
	}
}

void header::set(eHeadPos key, cmstring &value)
{
	string::size_type l=value.size()+1;
	h[key]=(char*) realloc(h[key], l);
	if(h[key])
		memcpy(h[key], value.c_str(), l);
}

void header::set(eHeadPos key, off_t nValue)
{	
	char buf[3*sizeof(off_t)];
	int len=sprintf(buf, OFF_T_FMT, nValue);
    set(key, buf, len);
}

tSS header::ToString() const
{
	tSS s;
	s<<frontLine << "\r\n";
	for(const auto& pos2key : mapId2Headname)
		if (h[pos2key.pos])
			s << pos2key.str << ": " << h[pos2key.pos] << "\r\n";
	s<< "Date: " << tCurrentTime() << "\r\n\r\n";
	return s;
}

int header::StoreToFile(cmstring &sPath) const
{
	int nByteCount(0);
	const char *szPath=sPath.c_str();
	int fd=open(szPath, O_WRONLY|O_CREAT|O_TRUNC, acfg::fileperms);
	if(fd<0)
	{
		fd=-errno;
		// maybe there is something in the way which can be removed?
		if(::unlink(szPath))
			return fd;

		fd=open(szPath, O_WRONLY|O_CREAT|O_TRUNC, acfg::fileperms);
		if(fd<0)
			return -errno;
	}
	
	auto hstr=ToString();
	const char *p=hstr.rptr();
	nByteCount=hstr.length();
	
	for(string::size_type pos=0; pos<(uint)nByteCount;)
	{
		int ret=write(fd, p+pos, nByteCount-pos);
		if(ret<0)
		{
			if(EAGAIN == errno || EINTR == errno)
				continue;
			if(EINTR == errno)
				continue;
			
			ret=errno;
			forceclose(fd);
			return -ret;
		}
		pos+=ret;
	}

	while(0!=close(fd))
	{
		if(errno != EINTR)
			return -errno;
	}
	
	return nByteCount;
}

std::string header::GenInfoHeaders()
{
	    string ret="Date: ";
	    ret+=tCurrentTime();
	    ret+="\r\nServer: Debian Apt-Cacher NG/" ACVERSION "\r\n";
	    return ret;
}

static const char* fmts[] =
{
		"%a, %d %b %Y %H:%M:%S GMT",
		"%A, %d-%b-%y %H:%M:%S GMT",
		"%a %b %d %H:%M:%S %Y"
};

bool header::ParseDate(const char *s, struct tm *tm)
{
	if(!s || !tm)
		return false;
	for(const auto& fmt : fmts)
		if(::strptime(s, fmt, tm))
			return true;

	return false;
}
