
#ifndef __FILEREADER_H_
#define __FILEREADER_H_

#include "config.h"
#include "acbuf.h"
#include "fileio.h"
#include "filelocks.h"

class IDecompressor;


/*!
 * Helper class used to read files.
 * 
 * Could use boost::iostream templates for most of that, but Boost became such a monster nowadays.
 * And for my work, my class behaves smarter.
 */
class filereader {
	
public:
	filereader();
	~filereader();
	
	//! Opens any supported file.
	/* @param sFilename File to open. Writes the base 
	 * 			filename w/o suffix or path prefix back into it
	 * @param bCriticalOpen Terminate program on failure
	 */ 
	bool OpenFile(const mstring & sFilename, bool bNoMagic=false, uint nFakeTrailingNewlines=0);
	
	//////! Filename with all prepended path and compressed suffix stripped
	//////void GetBaseFileName(mstring & sOut);
	//! Returns lines when beginning with non-space, otherwise empty string. 
	//! @return False on errors.
	bool GetOneLine(mstring & sOut, bool bForceUncompress=false);
	uint GetCurrentLine() const { return m_nCurLine;} ;
	bool CheckGoodState(bool bTerminateOnErrors, cmstring *reportFilePath=NULL) const;
	
	bool GetChecksum(int csType, uint8_t out[], off_t &scannedSize, FILE *pDumpFile=NULL);
	static bool GetChecksum(const mstring & sFileName, int csType, uint8_t out[],
			bool bTryUnpack, off_t &scannedSize, FILE *pDumpFile=NULL);

	inline const char *GetBuffer() const { return m_szFileBuf; };
	inline size_t GetSize() const { return m_nBufSize; };

	void Close();

private:

	bool m_bError, m_bEof;
	// XXX: not totally happy, this could be a simple const char* for most usecases
	mstring m_sErrorString;

	char *m_szFileBuf;
	size_t m_nBufSize, m_nBufPos;
	
	acbuf m_UncompBuf; // uncompressed window
	
	// visible position reporting
	uint m_nCurLine;
	
	int m_fd;
	
	int m_nEofLines;

	std::unique_ptr<IDecompressor> m_Dec;

	// not to be copied
	filereader& operator=(const filereader&);
	filereader(const filereader&);

	std::unique_ptr<filelocks::flock> m_mmapLock;
};

extern uint_fast16_t hexmap[];

inline bool CsEqual(const char *sz, uint8_t b[], unsigned short binLen)
{
	CUCHAR *a=(CUCHAR*)sz;
	if(!a)
		return false;
	for(int i=0; i<binLen;i++)
	{
		if(!*a)
			return false;

		uint_fast16_t r=hexmap[a[i*2]] * 16 + hexmap[a[i*2+1]];
		if(r != b[i]) return false;
	}
	return true;
};

bool Bz2compressFile(const char *pathIn, const char*pathOut);

#endif // __FILEREADER_H

