#ifndef PKGIMPORT_H_
#define PKGIMPORT_H_

//! Reuses lots of processing code
#include "expiration.h"

#include "fileio.h"
#include <cstring>
#include "csmapping.h"


class pkgimport : public tCacheMan, ifileprocessor
{

public:
	
	pkgimport(int);
	
	void Action(const mstring & src);
	
protected:
	// FileHandler
	bool ProcessRegular(const mstring &sPath, const struct stat &);
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry, bool bUnpackForCsumming);
	void _LoadKeyCache(const mstring & sFileName);

	// NOOP, our files are not changed
	void UpdateFingerprint(cmstring &s, off_t n, uint8_t *p, uint8_t *p2)
	{
	}

private:

	/*!
	 *  Two maps mapping in different directions:
		-	fingerprints pointing to file info description vector,
	 		created by file scan+identification
	 	-	when reusing old fingerprints, a file info description is mapped
	 		to stored fingerprint (cacheMap from tCacheProcessor)
	*/
	tImportMap m_importMap;
	tImportList m_importRest;
	MYSTD::set<mstring> m_precachedList;
	/* tFprCacheMap m_cacheMap;*/
	

	bool m_bLookForIFiles;
	mstring m_sSrcPath;
		
	//void _ExtractLocationsFromVolFiles();
	void _GuessAndGetIfiles();
		
};


#endif /*PKGIMPORT_H_*/
