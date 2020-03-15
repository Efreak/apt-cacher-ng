/*
 * csmapping.cc
 *
 *  Created on: 15.03.2020
 *      Author: Eduard Bloch
 */


#include "csmapping.h"

#ifdef HAVE_SSL
#include <openssl/sha.h>
#include <openssl/md5.h>
#elif defined(HAVE_TOMCRYPT)
#include <tomcrypt.h>
#endif

namespace acng
{

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

bool tChecksum::DecodeHexString(string_view text_in)
{
	if ((text_in.length() % 2) != 0 || text_in.length()/2 > csum.size())
		return false;
	auto pIn = (unsigned char*) text_in.data();
	for (unsigned i = 0, ii = 0; i < text_in.length(); i+=2, ++ii)
	{
		auto l=hexmap[pIn[i]];
		auto r=hexmap[pIn[i + 1]];
		if (l > 15 || r > 15) return false;
		csum[ii] = (l << 4) + r;
	}
	return true;
}
#warning move to acbuf::add_hex_chars - but only after merging refactored version from the may branch

#if 0
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
#endif

char ACNG_API h2t_map[] =
	{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd',
			'e', 'f' };

std::string BytesToHexString(const uint8_t sum[], unsigned short lengthBin)
{
	std::string sRet;
	for (int i=0; i<lengthBin; i++)
	{
		sRet+=(char) h2t_map[sum[i]>>4];
		sRet+=(char) h2t_map[sum[i]&0xf];
	}
	return sRet;
}

bool tFingerprint::ReadFile(const mstring &path, const CSTYPES eCstype, bool bUnpack,
		FILE *fDump)
{
	if (0 == GetCSTypeLen(eCstype))
		return false; // unsupported

	csum.csType = eCstype;
	return filereader::GetChecksum(path, csum, bUnpack, size, fDump);
}

#ifdef HAVE_SSL
class csumSHA1: public csumBase
{
	SHA_CTX ctx;
public:
	csumSHA1()
	{
		SHA1_Init(&ctx);
	}
	void add(const uint8_t *data, size_t size) override
	{
		SHA1_Update(&ctx, data, size);
	}
	tChecksum finish() override
	{
		tChecksum ret(CSTYPES::SHA1);
		SHA1_Final(ret.csum.data(), &ctx);
		return ret;
	}
};
class csumSHA256: public csumBase
{
	SHA256_CTX ctx;
public:
	csumSHA256()
	{
		SHA256_Init(&ctx);
	}
	void add(const uint8_t *data, size_t size) override
	{
		SHA256_Update(&ctx, data, size);
	}
	tChecksum finish() override
	{
		tChecksum ret(CSTYPES::SHA256);
		SHA256_Final(ret.csum.data(), &ctx);
		return ret;
	}
};
class csumSHA512 : public csumBase
{
	SHA512_CTX ctx;
public:
	csumSHA512()
	{
		SHA512_Init(&ctx);
	}
	void add(const uint8_t *data, size_t size) override
	{
		SHA512_Update(&ctx, data, size);
	}
	tChecksum finish() override
	{
		tChecksum ret(CSTYPES::SHA512);
		SHA512_Final(ret.csum.data(), &ctx);
		return ret;
	}
};
class csumMD5 : public csumBase
{
	MD5_CTX ctx;
public:
	csumMD5()
	{
		MD5_Init(&ctx);
	}
	void add(const uint8_t *data, size_t size) override
	{
		MD5_Update(&ctx, data, size);
	}
	tChecksum finish() override
	{
		tChecksum ret(CSTYPES::MD5);
		MD5_Final(ret.csum.data(), &ctx);
		return ret;
	}
};
#elif defined(HAVE_TOMCRYPT)
class csumSHA1 : public csumBase
{
	hash_state st;
public:
	csumSHA1() { sha1_init(&st); }
	void add(const char *data, size_t size) override { sha1_process(&st, (const unsigned char*) data, (unsigned long) size); }
	void finish(uint8_t* ret) override { sha1_done(&st, ret); }
};
class csumSHA256 : public csumBase
{
	hash_state st;
public:
	csumSHA256() { sha256_init(&st); }
	void add(const char *data, size_t size) override { sha256_process(&st, (const unsigned char*) data, (unsigned long) size); }
	void finish(uint8_t* ret) override { sha256_done(&st, ret); }
};
class csumSHA512 : public csumBase
{
	hash_state st;
public:
	csumSHA512() { sha512_init(&st); }
	void add(const char *data, size_t size) override { sha512_process(&st, (const unsigned char*) data, (unsigned long) size); }
	void finish(uint8_t* ret) override { sha512_done(&st, ret); }
};
class csumMD5 : public csumBase
{
	hash_state st;
public:
	csumMD5() { md5_init(&st); }
	void add(const char *data, size_t size) override { md5_process(&st, (const unsigned char*) data, (unsigned long) size); }
	void finish(uint8_t* ret) override { md5_done(&st, ret); }
};
#endif

std::unique_ptr<csumBase> csumBase::GetChecker(CSTYPES type)
{
	switch(type)
	{
	case CSTYPES::MD5:
		return std::unique_ptr<csumBase>(new csumMD5);
	case CSTYPES::SHA1:
		return std::unique_ptr<csumBase>(new csumSHA1);
	case CSTYPES::SHA256:
		return std::unique_ptr<csumBase>(new csumSHA256);
	case CSTYPES::SHA512:
		return std::unique_ptr<csumBase>(new csumSHA512);
	default: // for now
		return std::unique_ptr<csumBase>();
	}
}

// test checksum wrapper classes and their algorithms, also test conversion methods
#warning catch exceptions! and test it
void ACNG_API check_algos()
{
	string_view testvec("abc");
	std::pair<CSTYPES, string_view> test_sets[] =
	{
			{ CSTYPES::SHA1, "a9993e364706816aba3e25717850c26c9cd0d89d"},
			{ CSTYPES::SHA256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
			{CSTYPES::SHA512, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
			{CSTYPES::MD5, "900150983cd24fb0d6963f7d28e17f72"}
	};
	for (auto xtype : test_sets)
	{
		auto ap(csumBase::GetChecker(xtype.first));
		ap->add(testvec);
		auto res = ap->finish();
		auto resHex = res.to_string();
		if (res != xtype.second || xtype.second != resHex)
		{
			throw tStartupException(TSS << "Incorrect " << GetCsName(xtype.first)
							<< " implementation detected, check compilation settings");
		}
	}
}


}
