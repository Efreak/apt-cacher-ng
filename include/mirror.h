/*
 * mirror.h
 *
 *  Created on: 25.11.2010
 *      Author: ed
 */

#ifndef MIRROR_H_
#define MIRROR_H_

#include "cacheman.h"

class pkgmirror: public tCacheMan, ifileprocessor
{
public:
	pkgmirror(int);
	virtual ~pkgmirror();

	void Action(const mstring & src);

protected:
	// FileHandler
	bool ProcessRegular(const mstring &sPath, const struct stat &);
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry, bool bUnpackForCsumming);
	void _LoadKeyCache(const mstring & sFileName);

	virtual void UpdateFingerprint(const mstring &sPathRel, off_t nOverrideSize,
				uint8_t *pOverrideSha1, uint8_t *pOverrideMd5);

	bool m_bCalcSize, m_bSkipIxUpdate, m_bDoDownload, m_bAsNeeded, m_bUseDelta;
	off_t m_totalSize, m_totalHave;
	tStrSet m_pathFilter;

	const tHttpUrl *m_pDeltaSrc;
	tStrPos m_repCutLen;

	bool ConfigDelta(cmstring &sPathRel);
};
#endif /* MIRROR_H_ */
