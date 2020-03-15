
#ifndef __FILEREADER_H_
#define __FILEREADER_H_

#include "config.h"
#include "acbuf.h"
#include "fileio.h"
#include "filelocks.h"

namespace acng
{

class IDecompressor;
struct tChecksum;

/*!
 * Helper class used to read files.
 * 
 * Could use boost::iostream templates for most of that, but Boost became such a monster nowadays.
 * And for my work, my class behaves smarter.
 */
class ACNG_API filereader
{
	
public:
	filereader();
	~filereader();
	
	//! Opens any supported file.
	/* @param sFilename File to open. Writes the base 
	 * 			filename w/o suffix or path prefix back into it
	 * @param bCriticalOpen Terminate program on failure
	 */ 
	bool OpenFile(const mstring & sFilename, bool bNoMagic=false, unsigned nFakeTrailingNewlines=0);
	
	//////! Filename with all prepended path and compressed suffix stripped
	//////void GetBaseFileName(mstring & sOut);
	//! Returns lines when beginning with non-space, otherwise empty string. 
	//! @return False on errors.
	bool GetOneLine(mstring & sOut, bool bForceUncompress=false);
    unsigned GetCurrentLine() const { return m_nCurLine;}
	bool IsGood() const { return !m_bError; }
	/**
	 * Calculate checksum of the specified type
	 */
	bool GetChecksum(tChecksum& inout_cs, off_t &out_size, FILE *pDumpFile=nullptr);
	static bool GetChecksum(const mstring & sFileName, tChecksum& inout_cs,
			bool bTryUnpack, off_t &scannedSize, FILE *pDumpFile=nullptr);

    inline const char *GetBuffer() const { return m_szFileBuf; }
    inline size_t GetSize() const { return m_nBufSize; }

	void Close();

	const mstring& getSErrorString() const
	{
		return m_sErrorString;
	}

private:

	bool m_bError, m_bEof;
	// XXX: not totally happy, this could be a simple const char* for most usecases
	mstring m_sErrorString;

	char *m_szFileBuf;
	size_t m_nBufSize, m_nBufPos;
	
	acbuf m_UncompBuf; // uncompressed window
	
	// visible position reporting
	unsigned m_nCurLine;
	
	int m_fd;
	
	int m_nEofLines;

	std::unique_ptr<IDecompressor> m_Dec;

	// not to be copied
	filereader& operator=(const filereader&);
	filereader(const filereader&);
	std::unique_ptr<TFileShrinkGuard> m_mmapLock;
};

bool Bz2compressFile(const char *pathIn, const char*pathOut);

}

#endif // __FILEREADER_H
