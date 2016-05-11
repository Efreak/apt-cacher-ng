
#define LOCAL_DEBUG
#include "debug.h"

#include "meta.h"
#include "fileio.h"
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <algorithm>

#ifdef HAVE_WORDEXP
#include <wordexp.h>
#elif defined(HAVE_GLOB)
#include <glob.h>
#endif
#ifdef HAVE_TOMCRYPT
#include <tomcrypt.h>
#endif
using namespace std;

cmstring sPathSep(SZPATHSEP);
cmstring sPathSepUnix(SZPATHSEPUNIX);
#ifndef MINIBUILD
cmstring sDefPortHTTP("80");
cmstring sDefPortHTTPS("443");
#endif

cmstring PROT_PFX_HTTPS(WITHLEN("https://")), PROT_PFX_HTTP(WITHLEN("http://"));
cmstring FAKEDATEMARK(WITHLEN("Sat, 26 Apr 1986 01:23:39 GMT+3"));

/*
int getUUID() {
    lfd=(lfd+1)%65536;
   //cerr << "UUID: " << lfd <<endl;
    return lfd;
}
*/
void set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL);
    //ASSERT(flags != -1);
    flags |= O_NONBLOCK;
    flags = fcntl(fd, F_SETFL, flags);
}
void set_block(int fd) {
    int flags = fcntl(fd, F_GETFL);
    //ASSERT(flags != -1);
    flags &= ~O_NONBLOCK;
    flags = fcntl(fd, F_SETFL, flags);
}


/*
inline tStrPos findHostStart(const std::string & sUri)
{
	tStrPos p=0, l=sUri.size();
	if (0==sUri.compare(0, 7, "http://"))
		p=7;
	while(p<l && sUri[p]=='/') p++;
	return p;
}

void trimProto(std::string & sUri)
{
	sUri.erase(findHostStart(sUri));
}
*/


mstring GetBaseName(const string &in)
{
	if(in.empty())
		return sEmptyString;

	tStrPos end = in.find_last_not_of(CPATHSEP); // must be the last char of basename
	if(end == stmiss) // empty, or just a slash?
		return "/";
	
	tStrPos start = in.rfind(CPATHSEP, end);
	if(stmiss == start)
		start=0;
	
	return in.substr(start, end+1-start);
}

/*
void find_base_name(const char *in, const char * &pos, UINT &len)
{
	int l=strlen(in);
	if(l==0)
	{
		pos=in;
		len=0;
		return;
	}
	
		const char *p, *r;
		
		for(p=in+l-1;*p==cPathSep;p--)
		{	
			if(p==in)
			{
				pos=in;
				len=1;
				return;
			}
		}
		for(r=p;r>=in && *p!=cPathSep;r--)
		{
			if(r==in)
			{
				pos=in;
				len=p-in+1;
			}
		}
	
}
*/

/*!
 * \brief Simple split function, outputs resulting tokens into a string vector, with or without purging the previous contents
 */
tStrVec::size_type Tokenize(const string & in, const char *sep,
		tStrVec & out, bool bAppend, std::string::size_type nStartOffset)
{
	if(!bAppend)
		out.clear();
	tStrVec::size_type nBefore(out.size());
	
	tStrPos pos=nStartOffset, pos2=nStartOffset, oob=in.length();
	while (pos<oob)
	{
		pos=in.find_first_not_of(sep, pos);
		if (pos==stmiss) // no more tokens
			break;
		pos2=in.find_first_of(sep, pos);
		if (pos2==stmiss) // no more terminators, EOL
			pos2=oob;
		out.emplace_back(in.substr(pos, pos2-pos));
		pos=pos2+1;
	}

	return (out.size()-nBefore);
}

void StrSubst(string &contents, const string &from, const string &to, tStrPos pos)
{
	while (stmiss!=(pos=contents.find(from, pos)))
	{
		contents.replace(pos, from.length(), to);
		pos+=to.length();
	}
}

bool ParseKeyValLine(const string & sIn, string & sOutKey, string & sOutVal)
{
	// reuse the output string as buffer
	sOutVal=sIn;
	sOutKey.clear();
	trimFront(sOutVal);
	//cout << "parsing: "<<sOut<<endl;
	if(sOutVal.empty())
		return false;

	/*
	// comments or other crap, not for us
	if(stmiss!=sFilterString.find(sIn[0]))
		return false;
	*/

	string::size_type pos = sOutVal.find(":");
	if (AC_UNLIKELY(pos == 0 || pos==string::npos))
	{
		//cerr << "Bad configuration directive found, looking for: " << szKey << ", found: "<< sOut << endl;
		return false;
	}

	sOutKey = sOutVal.substr(0, pos);
	trimBack(sOutKey);

	sOutVal.erase(0, pos+1);
	trimFront(sOutVal);

	return true;
}


bool tHttpUrl::SetHttpUrl(cmstring &sUrlRaw, bool unescape)
{
	clear();
	mstring url = unescape ? UrlUnescape(sUrlRaw) : sUrlRaw;

	trimBack(url);
	trimFront(url);
		
	if(url.empty())
		return false;
	
	tStrPos hStart(0), l=url.length(), hEndSuc(0), pStart(0), p;
	bool bCheckBrac=false;
	
	if(0==strncasecmp(url.c_str(), "http://", 7))
		hStart=7;
	else if(0==strncasecmp(url.c_str(), "https://", 8))
	{
#ifndef HAVE_SSL
	aclog::err("E_NOTIMPLEMENTED: SSL");
	return false;
#else
		hStart=8;
		bSSL=true;
#endif
	}
	else if(isalnum((uint)url[0]))
		hStart=0;
	else if(url[0]=='[')
	{
		hStart=0;
		bCheckBrac=true; // must be closed
	}
	else if(stmiss!=url.find("://"))
		return false; // other protocol or weird stuff
	
	// kill leading slashes in any case
	while(hStart<l && url[hStart]=='/') hStart++;
	if(hStart>=l)
		return false;
	
	hEndSuc=url.find('/', hStart);
	if(stmiss==hEndSuc)
	{
		hEndSuc=l;
		goto extract_host_check_port;
	}
	pStart=hEndSuc;
	while(pStart<l && url[pStart]=='/') pStart++;
	pStart--;
	
	extract_host_check_port:
	if(pStart==0)
		sPath="/";
	else
		sPath=url.substr(pStart);

	if(url[hStart]=='_') // those are reserved
		return false;
	
	sHost=url.substr(hStart, hEndSuc-hStart);

	// credentials might in there, strip them of
	l=sHost.rfind('@');
	if(l!=mstring::npos)
	{
		sUserPass=sHost.substr(0, l);
		sHost.erase(0, l+1);
	}

	l=sHost.size();
	p=sHost.rfind(':');
	if(p==stmiss)
		goto strip_ipv6_junk;
	else if(p==l-1)
		return false; // this is crap, http:/asdf?
	else for(tStrPos r=p+1;r<l;r++)
		if(! isdigit(sHost[r]))
			return false;
	
	sPort=sHost.substr(p+1);
	sHost.erase(p);
	
	strip_ipv6_junk:
	
	if(sHost[0]=='[')
	{
		bCheckBrac=true;
		sHost.erase(0,1);
	}
	
	if(sHost[sHost.length()-1]==']')
		sHost.erase(sHost.length()-1);
	else if(bCheckBrac) // must have been present here
		return false;
	
	return true;
	
}

string tHttpUrl::ToURI(bool bUrlEscaped) const
{
	auto s(GetProtoPrefix());
	// if needs transfer escaping and is not internally escaped
	if (bUrlEscaped)
	{
		UrlEscapeAppend(sHost, s);
		if (!sPort.empty())
		{
			s += ':';
			s += sPort;
		}
		UrlEscapeAppend(sPath, s);
	}
	else
	{
		s += sHost;
		if (!sPort.empty())
		{
			s += ':';
			s += sPort;
		}
		s += sPath;
	}
	return s;
}

#if defined(HAVE_WORDEXP) || defined(HAVE_GLOB)

tStrDeq ExpandFilePattern(cmstring& pattern, bool bSorted, bool bQuiet)
{
	tStrDeq srcs;
#ifdef HAVE_WORDEXP
	auto p=wordexp_t();
	if(0==wordexp(pattern.c_str(), &p, 0))
	{
		for(char **s=p.we_wordv; s<p.we_wordv+p.we_wordc;s++)
			srcs.emplace_back(*s);
		wordfree(&p);
	}
	else if(!bQuiet)
		cerr << "Warning: failed to find files for " << pattern <<endl;
	if(bSorted) std::sort(srcs.begin(), srcs.end());
#elif defined(HAVE_GLOB)
	auto p=glob_t();
	if(0==glob(pattern.c_str(), GLOB_DOOFFS | (bSorted ? 0 : GLOB_NOSORT),
			nullptr, &p))
	{
		for(char **s=p.gl_pathv; s<p.gl_pathv+p.gl_pathc;s++)
			srcs.emplace_back(*s);
		globfree(&p);
	}
	else if(!bQuiet)
		cerr << "Warning: failed to find files for " << pattern <<endl;
#else
#warning Needs a file name expansion function, wordexp or glob
	srcs.emplace_back(pattern);
#endif

	return srcs;
}
#endif

#ifndef MINIBUILD

bool IsAbsolute(cmstring &dirToFix)
{
	bool bAbs=false;
#ifdef WIN32
	bAbs=dirToFix.length()> 2 && CPATHSEPWIN==dirToFix[2] && ':'==dirToFix[1] && isalpha(dirToFix[0];
	if(!bAbs) // maybe unc path?
		bAbs=(dirToFix[0] == CPATHSEPWIN &&dirToFix[1] == CPATHSEPWIN);
#else
	bAbs = (!dirToFix.empty() && CPATHSEPUNX==dirToFix[0]);
#endif
	return bAbs;
}

/*
void MakeAbsolutePath(std::string &dirToFix, const std::string &reldir)
{
	if(!IsAbsolute(dirToFix))
		dirToFix=reldir+CPATHSEP+dirToFix;
}
*/

extern uint_fast16_t hexmap[];

cmstring sEmptyString("");

/*
int GetSimilarity(cmstring& wanted, cmstring& candidate)
{
	const char *w=wanted.c_str(), *c=candidate.c_str();
	while(w&&c)
	{

	}
}
*/



#endif


/*
foreach $b (0..255) {
   print "\n" if($b%16==0);
   if( $b>=48 && $b<58 ) { $b-=48;}
   elsif($b>=97 && $b<103) { $b-=87;}
   elsif($b>=65 && $b<71) { $b-=55;}
   else {$b= --$dummy}
   print "$b,";
}
print "\n";
*/

#define _inv MAX_VAL(uint_fast16_t)

uint_fast16_t hexmap[] = {
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                0,1,2,3,4,5,6,7,8,9,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,10,11,12,13,14,15,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,10,11,12,13,14,15,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,
                _inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv,_inv
                };

bool CsAsciiToBin(const char *a, uint8_t b[], unsigned short binLength)
{
	const unsigned char *uA = (const unsigned char*) a;
	for(int i=0; i<binLength;i++)
	{
		if(hexmap[uA[i*2]]>15 || hexmap[uA[i*2+1]] > 15)
			return false;
		b[i]=hexmap[uA[i*2]] * 16 + hexmap[uA[i*2+1]];
	}
	return true;
}
bool Hex2buf(const char *a, size_t len, acbuf& ret)
{
	if(len%2)
		return false;
	ret.clear();
	ret.setsize(len/2+1);
	auto *uA = (const unsigned char*) a;
	for(auto end=uA+len;uA<end;uA+=2)
	{
		if(!*uA || !uA[1])
			return false;
		if(hexmap[uA[0]]>15 || hexmap[uA[1]] > 15)
			return false;
		*(ret.wptr()) = hexmap[uA[0]] * 16 + hexmap[uA[1]];
	}
	ret.got(len/2);
	return true;
}

char h2t_map[] =
	{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd',
			'e', 'f' };

string BytesToHexString(const uint8_t sum[], unsigned short lengthBin)
{
	string sRet;
	for (int i=0; i<lengthBin; i++)
	{
		sRet+=(char) h2t_map[sum[i]>>4];
		sRet+=(char) h2t_map[sum[i]&0xf];
	}
	return sRet;
}


inline bool is_safe_url_char(char c)
{
// follows rfc3986 except of ~ which some servers seem to handle incorrectly,
	// https://alioth.debian.org/tracker/?func=detail&aid=314030&group_id=100566&atid=413111
	// and the $?= which some SF servers cannot decode in their forwarding scheme
	switch (c)
	{
	case '/':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case 'a':
	case 'b':
	case 'c':
	case 'd':
	case 'e':
	case 'f':
	case 'g':
	case 'h':
	case 'i':
	case 'j':
	case 'k':
	case 'l':
	case 'm':
	case 'n':
	case 'o':
	case 'p':
	case 'q':
	case 'r':
	case 's':
	case 't':
	case 'u':
	case 'v':
	case 'w':
	case 'x':
	case 'y':
	case 'z':
	case 'A':
	case 'B':
	case 'C':
	case 'D':
	case 'E':
	case 'F':
	case 'G':
	case 'H':
	case 'I':
	case 'J':
	case 'K':
	case 'L':
	case 'M':
	case 'N':
	case 'O':
	case 'P':
	case 'Q':
	case 'R':
	case 'S':
	case 'T':
	case 'U':
	case 'V':
	case 'W':
	case 'X':
	case 'Y':
	case 'Z':
	case '-':
	case '.':
	case '_':
	case '?':
	case '&':
	case '=':
		return true;
	default:
		return false;
	}
}

void UrlEscapeAppend(cmstring &s, mstring &sTarget)
{
	for(const auto& c: s)
	{
		if(is_safe_url_char(c))
			sTarget+=c;
		else
		{
			char buf[4] = { '%', h2t_map[uint8_t(c) >> 4], h2t_map[uint8_t(c) & 0x0f],'\0'};
			sTarget+=buf;
		}
	}
}

mstring UrlEscape(cmstring &s)
{
	mstring ret;
	ret.reserve(s.size());
	UrlEscapeAppend(s, ret);
	return ret;
}

mstring DosEscape(cmstring &s)
{
	mstring ret;
	for(const auto& c: s)
	{
		if('/' == c)
			ret+=SZPATHSEP;
		else if(is_safe_url_char(c))
			ret+=c;
		else
		{
			char buf[4] = { '%', h2t_map[uint8_t(c) >> 4], h2t_map[uint8_t(c) & 0x0f],'\0'};
			ret += buf;
		}
	}
	return ret;
}

bool UrlUnescapeAppend(cmstring &from, mstring & to)
{
	bool ret=true;
	for(tStrPos i=0; i<from.length(); i++)
	{
		if(from[i] != '%')
			to+=from[i];
		else if(i>from.length()-3 // cannot start another sequence here (too short), keep as-is
				|| hexmap[(UCHAR)from[i+1]]>15 // not hex?
				|| hexmap[(UCHAR)from[i+2]]>15)
		{
			ret=false;
		}
		else
		{
			to+=char(16*hexmap[(unsigned char) from[i+1]] + hexmap[(unsigned char) from[i+2]]);
			i+=2;
		}
	}
	return ret;
}

mstring EncodeBase64Auth(cmstring& sPwdString)
{
	auto sNative=UrlUnescape(sPwdString);
	return EncodeBase64(sNative.data(), sNative.size());
}

#ifdef HAVE_TOMCRYPT
// XXX: fix usage, no proper error checking, data duplication...
string EncodeBase64(LPCSTR data, unsigned len)
{
	unsigned long reslen=len*4/3+3;
	vector<unsigned char>buf;
	buf.reserve(reslen);
	string ret;
	if(base64_encode((const unsigned char*) data, (unsigned long) len, &buf[0], &reslen) == CRYPT_OK)
		ret.assign((LPCSTR)&buf[0], reslen);
	return ret;
}
bool DecodeBase64(LPCSTR data, size_t len, acbuf& binData)
{
	unsigned long reslen=len;
	binData.clear();
	binData.setsize(len);
	auto rc=base64_decode((const unsigned char*) data,
			(unsigned long) len, (unsigned char*)binData.wptr(), &reslen);
	if(rc!=CRYPT_OK)
		return false;
	binData.got(reslen);
	return true;
}
#else // not HAVE_TOMCRYPT, use internal version and SSL
string EncodeBase64(LPCSTR data, unsigned len)
{
	uint32_t bits=0;
	unsigned char_count=0;
	char alphabet[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	string out;
	for(auto p=data; p<data+len; ++p)
	{
		uint8_t c=*p;
		/*
		if('%' == c && p<data+len-1
				&& hexmap[(uint8_t) *(p+1)]<=15
				&& hexmap[(uint8_t) *(p+2)]<=15)
		{
			c=uint8_t(hexmap[(uint8_t) *(p+1)]*16 + hexmap[(uint8_t) *(p+2)]);
			p+=2;
		}
		*/
		bits += c;
		char_count++;
		if (char_count == 3)
		{
			out+=(alphabet[unsigned(bits) >> 18]);
			out+=(alphabet[(unsigned(bits) >> 12) & 0x3f]);
			out+=(alphabet[(unsigned(bits) >> 6) & 0x3f]);
			out+=(alphabet[unsigned(bits) & 0x3f]);
			bits = 0;
			char_count = 0;
		}
		else
			bits <<= 8;
	}
	if (char_count != 0)
	{
		bits <<= 16 - (8 * char_count);
		out+=(alphabet[bits >> 18]);
		out+=(alphabet[(bits >> 12) & 0x3f]);
		if (char_count == 1)
		{
			out+=('=');
			out+=('=');
		}
		else
		{
			out+=(alphabet[(bits >> 6) & 0x3f]);
			out+=('=');
		}
	}
	return out;
}

#ifdef HAVE_SSL
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <cstring>
bool DecodeBase64(LPCSTR pAscii, size_t len, acbuf& binData)
{
   if(!pAscii)
      return false;
   binData.setsize(len);
   binData.clear();
   FILE* memStrm = ::fmemopen( (void*) pAscii, len, "r");
   auto strmBase = BIO_new(BIO_f_base64());
   auto strmBin = BIO_new_fp(memStrm, BIO_NOCLOSE);
   strmBin = BIO_push(strmBase, strmBin);
   BIO_set_flags(strmBin, BIO_FLAGS_BASE64_NO_NL);
   binData.got(BIO_read(strmBin, binData.wptr(), len));
   BIO_free_all(strmBin);
   checkForceFclose(memStrm);
   return binData.size();
}
#endif
#endif
