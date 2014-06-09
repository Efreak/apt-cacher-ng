
#include "filereader.h"

#include <unistd.h>
#include "fileio.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <limits>

#include "md5.h"
#include "sha1.h"
#include "csmapping.h"

#include <iostream>

// must be something sensible, ratio impacts stack size by inverse power of 2
#define BUFSIZEMIN 4095 // makes one page on i386 and should be enough for typical index files
#define BUFSIZEMAX 16*4096


#ifdef MINIBUILD
#undef HAVE_LIBBZ2
#undef HAVE_ZLIB
#undef HAVE_LZMA
#else

#ifndef HAVE_ZLIB
#warning Zlib or its development files are not available. Install them (e.g. zlib1g-dev) and run "make distclean". Gzip format support disabled.
#endif

#ifndef HAVE_LIBBZ2
#warning LibBz2 or its development files are not available. Install them (e.g. libbz2-dev) and run "make distclean". Bzip2 format support disabled.
#endif

#endif

using namespace MYSTD;


class IDecompressor
{
public:
	bool eof=false;
	mstring *psError = NULL;
	virtual ~IDecompressor() {};
	virtual bool UncompMore(char *szInBuf, size_t nBufSize, size_t &nBufPos, acbuf &UncompBuf) =0;
	virtual bool Init() =0;
};

#ifdef HAVE_LIBBZ2
#include <bzlib.h>
class tBzDec : public IDecompressor
{
	bz_stream strm = bz_stream();
public:
	bool Init() override
	{
		if (BZ_OK == BZ2_bzDecompressInit(&strm, 1, EXTREME_MEMORY_SAVING))
			return true;
		// crap, no proper sanity checks in BZ2_bzerror
		if(psError)
			psError->assign("BZIP2 initialization error");
		return false;

	}
	~tBzDec()
	{
		BZ2_bzDecompressEnd(&strm);
	}
	virtual bool UncompMore(char *szInBuf, size_t nBufSize, size_t &nBufPos, acbuf &UncompBuf) override
	{
		strm.next_in = szInBuf + nBufPos;
		strm.avail_in = nBufSize - nBufPos;
		strm.next_out = UncompBuf.wptr();
		strm.avail_out = UncompBuf.freecapa();

		int ret = BZ2_bzDecompress(&strm);
		if (ret == BZ_STREAM_END || ret == BZ_OK)
		{
			nBufPos = nBufSize - strm.avail_in;
			UINT nGotBytes = UncompBuf.freecapa() - strm.avail_out;
			UncompBuf.got(nGotBytes);
			eof = ret == BZ_STREAM_END;
			return true;
		}
		// or corrupted data?
		eof = true;
		// eeeeks bz2, no robust error getter, no prepared error message :-(
		if(psError)
			*psError = mstring("BZIP2 error ")+ltos(ret);
		return false;
	}
};
static const uint8_t bz2Magic[] =
{ 'B', 'Z', 'h' };
#endif

#ifdef HAVE_ZLIB
#include <zlib.h>
class tGzDec : public IDecompressor
{
	z_stream strm = z_stream();
public:
	bool Init() override
	{
		if (Z_OK == inflateInit2(&strm, 47))
			return true;
		if (psError)
			psError->assign("ZLIB initialization error");
		return false;
	}
	~tGzDec()
	{
		deflateEnd(&strm);
	}
	virtual bool UncompMore(char *szInBuf, size_t nBufSize, size_t &nBufPos, acbuf &UncompBuf) override
	{
		strm.next_in = (uint8_t*) szInBuf + nBufPos;
		strm.avail_in = nBufSize - nBufPos;
		strm.next_out = (uint8_t*) UncompBuf.wptr();
		strm.avail_out = UncompBuf.freecapa();

		int ret = inflate(&strm, Z_NO_FLUSH);
		if (ret == Z_STREAM_END || ret == Z_OK)
		{
			nBufPos = nBufSize - strm.avail_in;
			UINT nGotBytes = UncompBuf.freecapa() - strm.avail_out;
			UncompBuf.got(nGotBytes);
			eof = ret == Z_STREAM_END;
			return true;
		}
		// or corrupted data?
		eof = true;
		if(psError)
			*psError = mstring("ZLIB error: ") + (strm.msg ? mstring(strm.msg) : ltos(ret));
		return false;
	}
};
static const uint8_t gzMagic[] =
{ 0x1f, 0x8b, 0x8 };
#endif

#ifdef HAVE_LZMA
#include <lzma.h>

class tXzDec : public IDecompressor
{
	lzma_stream strm = lzma_stream();
	bool lzmaFormat;
public:
	tXzDec(bool bLzma) : lzmaFormat(bLzma) {};

	bool Init() override
	{
		auto x = (lzmaFormat ? lzma_alone_decoder(&strm,
					EXTREME_MEMORY_SAVING ? 32000000 : MAX_VAL(uint64_t))
				: lzma_stream_decoder(&strm,
				EXTREME_MEMORY_SAVING ? 32000000 : MAX_VAL(uint64_t),
						LZMA_TELL_UNSUPPORTED_CHECK | LZMA_CONCATENATED));
		if(LZMA_OK == x)
			return true;
		if(psError)
			psError->assign("LZMA initialization error");
		return false;
	}
	~tXzDec()
	{
		lzma_end(&strm);
	}
	virtual bool UncompMore(char *szInBuf, size_t nBufSize, size_t &nBufPos, acbuf &UncompBuf) override
	{
		strm.next_in = (uint8_t*) szInBuf + nBufPos;
		strm.avail_in = nBufSize - nBufPos;
		strm.next_out = (uint8_t*) UncompBuf.wptr();
		strm.avail_out = UncompBuf.freecapa();

		lzma_ret ret=lzma_code(&strm, strm.avail_in ? LZMA_RUN : LZMA_FINISH);
		if (ret == LZMA_STREAM_END || ret == LZMA_OK)
		{
			nBufPos = nBufSize - strm.avail_in;
			UINT nGotBytes = UncompBuf.freecapa() - strm.avail_out;
			UncompBuf.got(nGotBytes);
			eof = ret == LZMA_STREAM_END;
			return true;
		}
		// or corrupted data?
		eof = true;
		if(psError)
			*psError = mstring("LZMA error ")+ltos(ret);
		return false;
	}
};
static const uint8_t xzMagic[] =
{ 0xfd, '7', 'z', 'X', 'Z', 0x0 },
lzmaMagic[] = {0x5d, 0, 0, 0x80};
#endif

filereader::filereader() 
:
	m_bError(false),
	m_bEof(false),
	m_szFileBuf((char*)MAP_FAILED),
	m_nBufSize(0),
	m_nBufPos(0),
	m_nCurLine(0),
	m_fd(-1),
	m_nEofLines(0)
{
};

bool filereader::OpenFile(const string & sFilename, bool bNoMagic)
{
	Close(); // reset to clean state
	
	m_fd = open(sFilename.c_str(), O_RDONLY);

	if (m_fd < 0)
	{
		m_sErrorString=tErrnoFmter();
		return false;
	}

	if (bNoMagic)
		m_Dec.reset();
#ifdef HAVE_LIBBZ2
	else if (endsWithSzAr(sFilename, ".bz2"))
		m_Dec.reset(new tBzDec);
#endif
#ifdef HAVE_ZLIB
	else if (endsWithSzAr(sFilename, ".gz"))
		m_Dec.reset(new tGzDec);
#endif
#ifdef HAVE_LZMA
	else if(endsWithSzAr(sFilename, ".xz"))
		m_Dec.reset(new tXzDec(false));
	else if (endsWithSzAr(sFilename, ".lzma"))
		m_Dec.reset(new tXzDec(true));
#endif
	else // unknown... ok, probe it
	{
		filereader fh;
		if (fh.OpenFile(sFilename, true) && fh.GetSize() >= 10)
		{
			if(false) {}
#ifdef HAVE_ZLIB
			else if (0 == memcmp(gzMagic, fh.GetBuffer(), _countof(gzMagic)))
				m_Dec.reset(new tGzDec);
#endif
#ifdef HAVE_LIBBZ2
			else if (0 == memcmp(bz2Magic, fh.GetBuffer(), _countof(bz2Magic)))
				m_Dec.reset(new tBzDec);
#endif
#ifdef HAVE_LZMA
			else if (0 == memcmp(xzMagic, fh.GetBuffer(), _countof(xzMagic)))
				m_Dec.reset(new tXzDec(false));
			else if (0 == memcmp(lzmaMagic, fh.GetBuffer(), _countof(lzmaMagic)))
				m_Dec.reset(new tXzDec(true));
#endif
		}
	}

	if (m_Dec.get())
	{
		m_Dec->psError = &m_sErrorString;
		if(!m_Dec->Init())
			return false;
		m_UncompBuf.clear();
		m_UncompBuf.setsize(BUFSIZEMIN);
	}

	tErrnoFmter fmt;

	struct stat statbuf;
	if(0!=fstat(m_fd, &statbuf))
	{
		m_sErrorString=tErrnoFmter();
		return false;
	}

	// LFS on 32bit? That's not good for mmap. Don't risk incorrect behaviour.
	if(uint64_t(statbuf.st_size) >  MAX_VAL(size_t))
    {
        errno=EFBIG;
        m_sErrorString=tErrnoFmter();
        return false;
    }
	
	if(statbuf.st_size>0)
	{
		m_szFileBuf = (char*) mmap(0, statbuf.st_size, PROT_READ, MAP_SHARED, m_fd, 0);
		if(m_szFileBuf==MAP_FAILED)
		{
				m_sErrorString=tErrnoFmter();
				return false;
		}
		m_nBufSize = statbuf.st_size;
	}
	else if(m_Dec.get())
	{
		// compressed file but empty -> error
		m_sErrorString="Truncated compressed file";
		return false;
	}
	else
	{
		m_szFileBuf = NULL;
		m_nBufSize = 0;
	}
	
#ifdef HAVE_MADVISE
	// if possible, prepare to read that
	posix_madvise(m_szFileBuf, statbuf.st_size, POSIX_MADV_SEQUENTIAL);
#endif
	
	m_nBufPos=0;
	m_nCurLine=0;
	m_bError = m_bEof = false;
	return true;
}

bool filereader::CheckGoodState(bool bErrorsConsiderFatal, cmstring *reportFilePath) const
{
	if (!m_bError)
		return true;
	if (!bErrorsConsiderFatal)
		return false;
	cerr << "Error opening file";
	if (reportFilePath)
		cerr << " " << *reportFilePath;
	cerr << " (" << m_sErrorString << "), terminating." << endl;
	exit(EXIT_FAILURE);

}

void filereader::Close()
{
	m_nCurLine=0;
	
	if (m_szFileBuf != MAP_FAILED)
	{
		munmap(m_szFileBuf, m_nBufSize);
		m_szFileBuf = (char*) MAP_FAILED;
	}

	checkforceclose(m_fd);
	m_Dec.reset();

	m_nBufSize=0;

	m_bError = m_bEof = true; // will be cleared in Open()
	m_sErrorString=cmstring("Not initialized");
}

filereader::~filereader() {
	Close();
}

bool filereader::GetOneLine(string & sOut, bool bForceUncompress) {
	
	sOut.clear();
	
	if(m_bEof && m_nEofLines-- >0)
		return true; // just the empty line

	// stop flags set in previous run
	if(m_bError || m_bEof)
		return false;
	
	const char *rbuf;
	size_t nRest;

	// navigate to the place where the reading starts

	// needing an uncompression step? Either in the beginning or when doing the subcall
	if(m_Dec.get())
	{
		if ((!m_nBufPos || bForceUncompress) && !m_Dec->eof)
		{
			m_UncompBuf.move(); // make free as much as possible

			// must uncompress but the buffer is full. Resize it, if not possible, fail only then.
			if(bForceUncompress && 0==m_UncompBuf.freecapa())
			{
				if (m_UncompBuf.totalcapa() >= BUFSIZEMAX
						|| !m_UncompBuf.setsize(m_UncompBuf.totalcapa() * 2))
				{
					m_bError = true;
					m_sErrorString=mstring("Failed to allocate decompression buffer memory");
					return false;
				}
			}

			m_bError = ! m_Dec->UncompMore(m_szFileBuf, m_nBufSize, m_nBufPos, m_UncompBuf);
		}

		nRest=m_UncompBuf.size();
		rbuf=m_UncompBuf.rptr();
	}
	else
	{
		if(m_nBufPos>=m_nBufSize)
			m_bEof=true;
		// detect eof and remember that, for now or later calls
		nRest = m_bEof ? 0 : m_nBufSize-m_nBufPos;
		rbuf=m_szFileBuf+m_nBufPos;
	}
	
	// look for end in the rest of buffer (may even be nullsized then it fails implicitely, newline decides), 
	// on miss -> try to get more, check whether the available size changed, 
	// on success -> retry
	
	//const char *newline=mempbrk(rbuf, "\r\n", nRest);
  //const char *crptr=(const char*) memchr(rbuf, '\r', nRest);
  //const char *lfptr=(const char*) memchr(rbuf, '\n', nRest);
  //const char *newline = (crptr&&lfptr) ? MYSTD::min(crptr,lfptr) : MYSTD::max(crptr,lfptr);
	const char *newline=0;
	for(const char *x=rbuf; x<rbuf+nRest; ++x)
	{
		if('\r' == *x || '\n' == *x) // that's what compilers like most :-(
		{
			newline=x;
			break;
		}
	}
	
	tStrPos nLineLen, nDropLen;
	
	if(newline)
	{
		nLineLen=newline-rbuf;
		nDropLen=nLineLen+1;
		// cut optional \r or \n but only when it's from another kind
		if(nRest > nDropLen &&  newline[0]+newline[1]== '\r'+'\n')
			nDropLen++;
	}
	else
	{
		if(m_Dec.get())
		{
			// it's the final buffer...
			if(m_Dec->eof)
				m_bEof=true; // won't uncompress more anyhow
			else // otherwise has to unpack more with a larger buffer or fail
				return GetOneLine(sOut, true);
		}

		// otherwise can continue to the finish 
		nDropLen=nLineLen=nRest;
	}
	
	sOut.assign(rbuf, nLineLen);
	
	if(!m_Dec.get())
		m_nBufPos+=nDropLen;
	else
		m_UncompBuf.drop(nDropLen);
	
	m_nCurLine++;
	return true;
}

#ifndef MINIBUILD
class csumSHA1 : public csumBase, public SHA_INFO
{
public:
	csumSHA1() { sha_init(this); }
	void add(const char *data, size_t size) override { sha_update(this, (SHA_BYTE*) data, size); }
	void finish(uint8_t* ret) override { sha_final(ret, this); }
};
class csumMD5 : public csumBase, public md5_state_s
{
public:
	csumMD5() { md5_init(this); }
	void add(const char *data, size_t size) override { md5_append(this, (md5_byte_t*) data, size); }
	void finish(uint8_t* ret) override { md5_finish(this, ret); }
};

auto_ptr<csumBase> csumBase::GetChecker(CSTYPES type)
{
	switch(type)
	{
	case CSTYPE_MD5:
		return auto_ptr<csumBase>(new csumMD5);
	case CSTYPE_SHA1:
	default: // for now
		return auto_ptr<csumBase>(new csumSHA1);
	}
}

bool filereader::GetChecksum(const mstring & sFileName, int csType, uint8_t out[],
		bool bTryUnpack, off_t &scannedSize, FILE *fDump)
{
	filereader f;
	return (f.OpenFile(sFileName, !bTryUnpack)
			&& f.GetChecksum(csType, out, scannedSize, fDump));
}

bool filereader::GetChecksum(int csType, uint8_t out[], off_t &scannedSize, FILE *fDump)
{
	unique_ptr<csumBase> summer(csumBase::GetChecker(CSTYPES(csType)));
	scannedSize=0;
	
	if(!m_Dec.get())
	{
		summer->add(m_szFileBuf, m_nBufSize);
		//sha_update(&ctx, (SHA_BYTE*) m_szFileBuf, m_nBufSize);
		if(fDump)
			fwrite(m_szFileBuf, sizeof(char), m_nBufSize, fDump);
		scannedSize=m_nBufSize;
	}
	else
	{
		for(;;)
		{

			if(!m_Dec->UncompMore(m_szFileBuf, m_nBufSize, m_nBufPos, m_UncompBuf))
			{
				m_bError=true;
				return false;
			}

			UINT nPlainSize=m_UncompBuf.size();
			summer->add(m_UncompBuf.rptr(), nPlainSize);
			if(fDump)
				fwrite(m_UncompBuf.rptr(), sizeof(char), nPlainSize, fDump);
			scannedSize+=nPlainSize;
			m_UncompBuf.clear();

			if(m_Dec->eof)
			{
				m_bEof=true;
				break;
			}
		}

	}
	//sha_final(out, &ctx);
	summer->finish(out);
	
	return CheckGoodState(false);
}

// test checksum wrapper classes and their algorithms, also test conversion methods
void check_algos()
{
	const char testvec[]="abc";
	uint8_t out[20];
	unique_ptr<csumBase> ap(csumBase::GetChecker(CSTYPE_SHA1));
	ap->add(testvec, sizeof(testvec)-1);
	ap->finish(out);
	if(!CsEqual("a9993e364706816aba3e25717850c26c9cd0d89d", out, 20))
	{
		cerr << "Incorrect SHA1 implementation detected, check compilation settings!\n";
		exit(EXIT_FAILURE);
	}

	ap = csumBase::GetChecker(CSTYPE_MD5);
	ap->add(testvec, sizeof(testvec) - 1);
	ap->finish(out);
	if (BytesToHexString(out, 16) != "900150983cd24fb0d6963f7d28e17f72")
	{
		cerr << "Incorrect MD5 implementation detected, check compilation settings!\n";
		exit(EXIT_FAILURE);
	}
}



#endif

#ifdef HAVE_LIBBZ2

bool Bz2compressFile(const char *pathIn, const char*pathOut)
{
	bool bRet=false;
	filereader reader;
	FILE *f(NULL);
	BZFILE *bzf(NULL);
	int nError(0);

	if(!reader.OpenFile(pathIn, true))
		return false;

	if(NULL !=(f = fopen(pathOut, "w")))
	{
		if(!ferror(f))
		{
			if(NULL != (bzf = BZ2_bzWriteOpen( &nError, f, 9, 0, 30)))
			{
				if(BZ_OK == nError)
				{
					BZ2_bzWrite(&nError, bzf, (void*) reader.GetBuffer(), reader.GetSize());
					if(BZ_OK == nError)
						bRet=true;
				}
				BZ2_bzWriteClose(&nError, bzf, 0, 0, 0);
				if(BZ_OK != nError)
					bRet=false;
			}
		}
		if(ferror(f))
			bRet=false;

		checkForceFclose(f);
	}
	return bRet;
}

#endif
