#ifndef CSMAPPING_H_
#define CSMAPPING_H_

#include <string.h>
#include "meta.h"
#include "filereader.h"
#include "fileio.h"


#define MAXCSLEN 20

typedef enum {
   CSTYPE_INVALID=0,
   CSTYPE_MD5=1,
   CSTYPE_SHA1=2,
   CSTYPE_SHA256=3
} CSTYPES;

class csumBase
{
public:
	virtual ~csumBase() {};
	virtual void add(const char *data, size_t size) = 0;
	virtual void finish(uint8_t* ret) = 0;
	static std::unique_ptr<csumBase> GetChecker(CSTYPES);
};

// kind of file identity, compares by file size and checksum (MD5 or SHA1)
struct tFingerprint {
	off_t size;
	CSTYPES csType;
	uint8_t csum[MAXCSLEN];
	
	tFingerprint() : size(0), csType(CSTYPE_INVALID) {};
	tFingerprint(const tFingerprint &a) : size(a.size),
	csType(a.csType)
	{
		memcpy(csum, a.csum, 20);
	};
	
	bool SetCs(const mstring & hexString, CSTYPES eCstype)
	{
		if(hexString.empty()
				|| (eCstype!=CSTYPE_MD5 && eCstype!=CSTYPE_SHA1)
				|| (eCstype==CSTYPE_MD5 && 32 != hexString.length())
				|| (eCstype==CSTYPE_SHA1 && 40 != hexString.length()))
			return false;

		csType=eCstype;
		return CsAsciiToBin(hexString.c_str(), csum, CSTYPE_MD5==csType?16:20);
	}
	void Set(uint8_t *pData, CSTYPES eCstype, off_t newsize)
	{
		size=newsize;
		csType=eCstype;
		if(pData)
			memcpy(csum, pData, CSTYPE_MD5==csType?16:20);
	}
	bool ScanFile(const mstring & path, const CSTYPES eCstype, bool bUnpack, FILE *fDump=NULL)
	{
		if(eCstype != CSTYPE_MD5 && CSTYPE_SHA1 != eCstype)
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
		return BytesToHexString(csum, CSTYPE_MD5==csType?16:20);
	}
	LPCSTR GetCsName() const
	{
		switch(csType)
		{
		case CSTYPE_MD5: return "Md5";
		case CSTYPE_SHA1: return "Sha1";
		case CSTYPE_SHA256: return "Sha256";
		default: return "Other";
		}
	}
	operator mstring() const
	{
		return GetCsAsString()+"_"+offttos(size);
	}
	inline bool csEquals(const tFingerprint& other) const
	{
		return 0==memcmp(csum, other.csum, csType==CSTYPE_MD5 ? 16 : 20);
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
	bool CheckFile(cmstring & sFile)
	{
		if(size != GetFileSize(sFile, -2))
			return false;
		tFingerprint probe;
		if(!probe.ScanFile(sFile, csType, false, NULL))
			return false;
		return probe == *this;
	}
	bool operator<(const tFingerprint & other) const
	{
		if(other.csType!=csType)
			return csType<other.csType;
		if(size!=other.size)
			return size<other.size;
		return memcmp(csum, other.csum, csType==CSTYPE_MD5 ? 16 : 20) < 0;
	}
};

struct tRemoteFileInfo
{
	tFingerprint fpr;
	bool bInflateForCs = false;
	mstring sDirectory, sFileName;
	inline void SetInvalid() {
		sFileName.clear();
		sDirectory.clear();
		fpr.csType=CSTYPE_INVALID;
		fpr.size=-1;
		bInflateForCs = false;
	}
	inline bool IsUsable() {
		return (!sFileName.empty() && fpr.csType!=CSTYPE_INVALID && fpr.size>0);
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
    
    time_t mtime;
    bool bFileUsed;
    
    inline tImpFileInfo(const mstring & s, time_t m) :
        sPath(s), mtime(m), bFileUsed(false) {};
    inline tImpFileInfo() : mtime(0), bFileUsed(false) {};
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


#endif /*CSMAPPING_H_*/
