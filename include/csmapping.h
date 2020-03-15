#ifndef CSMAPPING_H_
#define CSMAPPING_H_

#include <array>

#include <string.h>
#include "meta.h"
#include "filereader.h"
#include "fileio.h"

#define MAXCSLEN 64

namespace acng
{


enum class CSTYPES : unsigned char
{
   INVALID=0,
   MD5=16,
   SHA1=20,
   SHA256=32,
   SHA512=64
};

inline CSTYPES GuessCStype(unsigned short len)
{
	switch(len)
	{
	case 16: return CSTYPES::MD5;
	case 20: return CSTYPES::SHA1;
	case 32: return CSTYPES::SHA256;
	case 64: return CSTYPES::SHA512;
	default: return CSTYPES::INVALID;
	}
}
inline unsigned short GetCSTypeLen(CSTYPES t)
{
	// this is not a plain return for security reasons!
	switch(t)
	{
	case CSTYPES::MD5: return 16;
	case CSTYPES::SHA1: return 20;
	case CSTYPES::SHA256: return 32;
	case CSTYPES::SHA512: return 64;
	default: return 0;
	}
}
inline LPCSTR GetCsName(CSTYPES csType)
{
	switch(csType)
	{
	case CSTYPES::MD5: return "Md5";
	case CSTYPES::SHA1: return "Sha1";
	case CSTYPES::SHA256: return "Sha256";
	case CSTYPES::SHA512: return "Sha512";
	default: return "Other";
	}
}
inline LPCSTR GetCsNameReleaseFile(CSTYPES csType)
{
	switch(csType)
	{
	case CSTYPES::MD5: return "MD5Sum";
	case CSTYPES::SHA1: return "SHA1";
	case CSTYPES::SHA256: return "SHA256";
	case CSTYPES::SHA512: return "SHA512";
	default: return "Other";
	}
}

struct tChecksum;

extern uint_fast16_t hexmap[];
extern char h2t_map[];

#if 0
inline bool CsEqual(const char *sz, uint8_t b[], size_t binLen)
{
	CUCHAR *a=(CUCHAR*)sz;
	if(!a)
		return false;
	for(decltype(binLen) i=0; i<binLen;i++)
	{
		if(!*a)
			return false;

		uint_fast16_t r=hexmap[a[i*2]] * 16 + hexmap[a[i*2+1]];
		if(r != b[i]) return false;
	}
	return true;
};
#endif

class csumBase
{
public:

	// Factory for a specific implementation
	static std::unique_ptr<csumBase> GetChecker(CSTYPES);

	virtual ~csumBase() {};
	virtual void add(const uint8_t* data, size_t size) = 0;
	/**
	 * Calculate the resulting checksum and store in ret.
	 * @param ret Storage for resulting checksum
	 */
	virtual tChecksum finish() = 0;
	inline void add(string_view data) { return add((const uint8_t*)data.data(), data.size()); }
};

// type suitable for holding a checksum of any supported type
struct tChecksum
{
	CSTYPES csType;
	using tChecksumContainer = std::array<uint8_t, MAXCSLEN>;
	tChecksumContainer csum;
	tChecksum(CSTYPES t) : csType(t) {}
	tChecksum() : csType(CSTYPES::INVALID) {}
	bool Set(string_view hexString, CSTYPES eCstype = CSTYPES::INVALID)
	{
		auto l = hexString.size();
		if(!l || l%2) // weird sizes...
			return false;
		if(eCstype == CSTYPES::INVALID)
		{
			eCstype = GuessCStype(hexString.size() / 2);
			if(eCstype == CSTYPES::INVALID)
				return false;
		}
		else if(l != 2*GetCSTypeLen(eCstype))
			return false;

		csType=eCstype;
		return DecodeHexString(hexString); //CsAsciiToBin(hexString.c_str(), csum, l/2);
	}

	std::string to_string() const { return BytesToHexString(csum.data(), GetCSTypeLen(csType)); }
	size_t length() const { return GetCSTypeLen(csType); }

	bool operator<(const tChecksum& other) const
	{
		if(other.csType!=csType)
			return csType<other.csType;
		return memcmp(csum.data(), other.csum.data(), GetCSTypeLen(csType)) < 0;

	}
	bool operator==(const tChecksum& other) const
	{
		return csType == other.csType && csum == other.csum;
	}
	bool operator!=(const tChecksum& other) const
	{
		return csType != other.csType || csum != other.csum;
	}
	bool operator==(string_view refString) const
	{
		const auto l = length();
		if (!l || refString.length() != 2 * l)
			return false;
		for (size_t i = 0; i < l; ++i)
		{
			auto cLeft = h2t_map[csum[i] >> 4];
			if (cLeft != refString[i * 2])
				return false;
			auto cRight = h2t_map[csum[i] & 0xf];
			if (cRight != refString[i * 2 + 1])
				return false;
		}
		return true;
	}
	bool operator!=(string_view refString) const
	{
		return !((*this) == refString);
	}
private:
	bool DecodeHexString(string_view);
};


// kind of file identity, compares by file size and checksum (MD5 or SHA*)
struct tFingerprint {
	off_t size =0;
	tChecksum csum;

	tFingerprint() =default;
	tFingerprint(const tFingerprint &a) = default;

	tFingerprint & operator=(const tFingerprint& a) =default;

	inline bool Set(cmstring & hexString, CSTYPES eCstype, off_t newsize)
	{
		size=newsize;
		return csum.Set(hexString, eCstype);
	}

	inline void Set(uint8_t *pData, CSTYPES eCstype, off_t newsize)
	{
		size=newsize;
		csum.csType = eCstype;
		if(pData)
			memcpy(csum.csum.data(), pData, GetCSTypeLen(eCstype));
	}

	/**
	 * Reads first two tokens from splitter, first considered checksum, second the size.
	 * Keeps the splitter pointed at last token, expects splitter be set at previous position.
	 * @return false if data is crap or wantedType was set but does not fit what's in the first token.
	 */
	inline bool Set(tSplitWalk splitInput, CSTYPES wantedType = CSTYPES::INVALID)
	{
		if(!splitInput.Next())
			return false;
		if(!csum.Set(splitInput.view(), wantedType))
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
	inline bool Set(const tSplitWalk & splitInput, CSTYPES wantedType = CSTYPES::INVALID)
	{ return Set(std::move(splitInput), wantedType); }
#endif

	bool ReadFile(const mstring &path, const CSTYPES eCstype, bool bUnpack = false, FILE *fDump =
			nullptr);
	void Invalidate()
	{
		csum.csType=CSTYPES::INVALID;
		size=-1;
	}
	operator mstring() const
	{
		return csum.to_string()+"_"+offttos(size);
	}
	inline bool operator==(const tFingerprint & other) const
			{
		return other.size == size && other.csum == csum;

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
		if(!probe.ReadFile(sFile, csum.csType, false, nullptr))
			return false;
		return probe == *this;
	}
	bool operator<(const tFingerprint & other) const
	{
		if(size!=other.size)
			return size<other.size;
		return csum < other.csum;
	}
};

struct tRemoteFileInfo
{
	tFingerprint fpr;
	mstring sDirectory, sFileName;
	inline void SetInvalid() {
		sFileName.clear();
		sDirectory.clear();
		fpr.csum.csType=CSTYPES::INVALID;
		fpr.size=-1;
	}
	inline bool IsUsable() {
		return (!sFileName.empty() && fpr.csum.csType!=CSTYPES::INVALID && fpr.size>0);
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

void check_algos();

}

#endif /*CSMAPPING_H_*/
