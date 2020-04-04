#ifndef CSMAPPING_H_
#define CSMAPPING_H_

#include <string.h>
#include "meta.h"
#include "filereader.h"
#include "fileio.h"

// XXX: allocate this dynamically?
#define MAXCSLEN 64

namespace acng
{

typedef enum : char {
   CSTYPE_INVALID=0,
   CSTYPE_MD5=1,
   CSTYPE_SHA1=2,
   CSTYPE_SHA256=3,
   CSTYPE_SHA512=4
} CSTYPES;

inline CSTYPES GuessCStype(unsigned short len)
{
	switch(len)
	{
	case 16: return CSTYPE_MD5;
	case 20: return CSTYPE_SHA1;
	case 32: return CSTYPE_SHA256;
	case 64: return CSTYPE_SHA512;
	default: return CSTYPE_INVALID;
	}
}
inline unsigned short GetCSTypeLen(CSTYPES t)
{
	switch(t)
	{
	case CSTYPE_MD5: return 16;
	case CSTYPE_SHA1: return 20;
	case CSTYPE_SHA256: return 32;
	case CSTYPE_SHA512: return 64;
	default: return 0;
	}
}
inline LPCSTR GetCsName(CSTYPES csType)
{
	switch(csType)
	{
	case CSTYPE_MD5: return "Md5";
	case CSTYPE_SHA1: return "Sha1";
	case CSTYPE_SHA256: return "Sha256";
	case CSTYPE_SHA512: return "Sha512";
	default: return "Other";
	}
}
inline LPCSTR GetCsNameReleaseFile(CSTYPES csType)
{
	switch(csType)
	{
	case CSTYPE_MD5: return "MD5Sum";
	case CSTYPE_SHA1: return "SHA1";
	case CSTYPE_SHA256: return "SHA256";
	case CSTYPE_SHA512: return "SHA512";
	default: return "Other";
	}
}

class csumBase
{
public:
	virtual ~csumBase() {};
	virtual void add(const char *data, size_t size) = 0;
	virtual void finish(uint8_t* ret) = 0;
	static std::unique_ptr<csumBase> GetChecker(CSTYPES);
};

// kind of file identity, compares by file size and checksum (MD5 or SHA*)
struct tFingerprint {
	off_t size =0;
	CSTYPES csType =CSTYPE_INVALID;
	uint8_t csum[MAXCSLEN];
	
	tFingerprint() =default;
	tFingerprint(const tFingerprint &a) : size(a.size),
	csType(a.csType)
	{
		memcpy(csum, a.csum, sizeof(csum));
	};
	tFingerprint & operator=(const tFingerprint& a)
	{
		if(this == &a) return *this;
		size = a.size;
		csType = a.csType;
		memcpy(csum, a.csum, sizeof(csum));
		return *this;
	}
	
	bool SetCs(const mstring & hexString, CSTYPES eCstype = CSTYPE_INVALID)
	{
		auto l = hexString.size();
		if(!l || l%2) // weird sizes...
			return false;
		if(eCstype == CSTYPE_INVALID)
		{
			eCstype = GuessCStype(hexString.size() / 2);
			if(eCstype == CSTYPE_INVALID)
				return false;
		}
		else if(l != 2*GetCSTypeLen(eCstype))
			return false;

		csType=eCstype;
		return CsAsciiToBin(hexString.c_str(), csum, l/2);
	}
	void Set(uint8_t *pData, CSTYPES eCstype, off_t newsize)
	{
		size=newsize;
		csType=eCstype;
		if(pData)
			memcpy(csum, pData, GetCSTypeLen(eCstype));
	}

	inline bool Set(cmstring & hexString, CSTYPES eCstype, off_t newsize)
	{
		if(!SetCs(hexString, eCstype))
			return false;
		size=newsize;
		return true;

	}

	/**
	 * Reads first two tokens from splitter, first considered checksum, second the size.
	 * Keeps the splitter pointed at last token, expects splitter be set at previous position.
	 * @return false if data is crap or wantedType was set but does not fit what's in the first token.
	 */
	inline bool Set(const tSplitWalk & splitInput, CSTYPES wantedType = CSTYPE_INVALID)
	{
		if(!splitInput.Next())
			return false;
		if(!SetCs(splitInput.str(), wantedType))
			return false;
		if(!splitInput.Next())
			return false;
		size = atoofft(splitInput.str().c_str(), -1);
		if(size < 0)
			return false;
		return true;
	}
#if 0
	/**
	 * Warning: this function only exists to work around C++ stupidity.
	 * The const modifier is void, it will still modify splitter state.
	 */
	inline bool Set(const tSplitWalk & splitInput, CSTYPES wantedType = CSTYPE_INVALID)
	{ return Set(std::move(splitInput), wantedType); }
#endif

	bool ScanFile(const mstring & path, const CSTYPES eCstype, bool bUnpack = false, FILE *fDump=nullptr)
	{
		if(! GetCSTypeLen(eCstype))
			return false; // unsupported
		csType=eCstype;
		return filereader::GetChecksum(path, eCstype, csum, bUnpack, size, fDump);
	}
	void Invalidate()
	{
		csType=CSTYPE_INVALID;
		size=-1;
	}
	mstring GetCsAsString() const
	{
		return BytesToHexString(csum, GetCSTypeLen(csType));
	}
	operator mstring() const
	{
		return GetCsAsString()+"_"+offttos(size);
	}
	inline bool csEquals(const tFingerprint& other) const
	{
		return 0==memcmp(csum, other.csum, GetCSTypeLen(csType));
	}
	inline bool operator==(const tFingerprint & other) const
	{
		if(other.csType!=csType || size!=other.size)
			return false;
		return csEquals(other);
	}
	inline bool operator!=(const tFingerprint & other) const
	{
		return !(other == *this);
	}
	bool CheckFile(cmstring & sFile) const
	{
		if(size != GetFileSize(sFile, -2))
			return false;
		tFingerprint probe;
		if(!probe.ScanFile(sFile, csType, false, nullptr))
			return false;
		return probe == *this;
	}
	bool operator<(const tFingerprint & other) const
	{
		if(other.csType!=csType)
			return csType<other.csType;
		if(size!=other.size)
			return size<other.size;
		return memcmp(csum, other.csum, GetCSTypeLen(csType)) < 0;
	}
};

struct tRemoteFileInfo
{
	tFingerprint fpr;
	mstring sDirectory, sFileName;
	inline void SetInvalid() {
		sFileName.clear();
		sDirectory.clear();
		fpr.csType=CSTYPE_INVALID;
		fpr.size=-1;
	}
	inline bool IsUsable() {
		return (!sFileName.empty() && fpr.csType!=CSTYPE_INVALID && fpr.size>0);
	}
	bool SetFromPath(cmstring &sPath, cmstring &sBaseDir)
	{
		if (sPath.empty())
			return false;

		tStrPos pos = sPath.rfind(SZPATHSEPUNIX);
		if (pos == stmiss)
		{
			sFileName = sPath;
			sDirectory = sBaseDir;
		}
		else
		{
			sFileName = sPath.substr(pos + 1);
			sDirectory = sBaseDir + sPath.substr(0, pos + 1);
		}
		return true;
	}
	inline bool SetSize(LPCSTR szSize)
	{
		auto l = atoofft(szSize, -2);
		if(l<0)
			return false;
		fpr.size = l;
		return true;
	}
};


/** For IMPORT:
 * Helper class to store attributes for different purposes. 
 * They need to be stored somewhere, either in fingerprint or
 * another struct including fingerprint, or just here :-(.
 */
struct tImpFileInfo
{
    mstring sPath;
    
    time_t mtime = 0;
    bool bFileUsed = false;
    
    inline tImpFileInfo(const mstring & s, time_t m) :
        sPath(s), mtime(m) {};
    tImpFileInfo() =default;
};
struct ltCacheKeyComp
{
  inline bool operator()(const tImpFileInfo &a, const tImpFileInfo &b) const
  {
	  return (a.mtime!=b.mtime) ? (a.mtime<b.mtime) :
#if defined(WINDOWS) || defined(WIN32)
			  (strcasecmp(a.sPath.c_str(), b.sPath.c_str()) < 0);
#else
			  (a.sPath<b.sPath);
#endif
  }
};

typedef std::map<tImpFileInfo, tFingerprint, ltCacheKeyComp> tFprCacheMap;

}

#endif /*CSMAPPING_H_*/
