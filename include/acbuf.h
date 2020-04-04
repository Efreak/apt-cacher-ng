
#ifndef _acbuf_H
#define _acbuf_H

#include <limits.h>
#include <string.h>
#include <stdio.h>
#include "meta.h"

namespace acng
{

/*! \brief Helper class to maintain a memory buffer, i.e. a continuous block of bytes.
 * It also encapsulates some typical operations on it.
 */

class ACNG_API acbuf
{
    public:
        inline acbuf() : r(0), w(0), m_nCapacity(0), m_buf(nullptr) {};
    	virtual ~acbuf() { free(m_buf); m_buf=nullptr; }
    	inline bool empty() const { return w==r;}
    	//! Count of the data inside
        inline unsigned int size() const { return w-r;}
        //! Returns capacity
        inline unsigned int freecapa() const { return m_nCapacity-w; }
        inline unsigned int totalcapa() const { return m_nCapacity; }
        //! Erase all characters after a certain position  
        inline void erase(size_t pos) { w=r+pos; if(r==w) clear(); }
        //! Invalidate data on the beginning, move reading position  
        inline void drop(size_t count) {  r+=count; if(r==w) clear(); }
        //! Mark externally appended data as valid, move writing position
        inline void got(size_t count) { w+=count;}
        //! for the worst case, move data to beginning to make space to read a complete new header
        inline void move() { if(0==r) return; memmove(m_buf, m_buf+r, w+1-r); w-=r; r=0; }
        //! Return pointer where to append data
        inline char *wptr() { return m_buf+w;};
        //! Return pointer to read valid data from
        inline const char *rptr() const { return m_buf+r; }
        //! like rptr but appends a nullptr terminator
        inline const char *c_str() const { m_buf[w]=0x0; return rptr();}
        //! Equivalent to drop(size()), drops all data
        inline void clear() {w=r=0;}
        
        //! Allocate needed memory
        bool setsize(unsigned int capa);
        bool initFromFile(const char *path);

        /*!
         * Writes to a (non-blocking) file descriptor, cares about EAGAIN and updates
         * position indexes. Optional chunklen variable specifies
         * the maximum of data to write.
         *
         * \param fd File descriptor to write to
         * \param maxlen Maximum amount of data to write
         * \return Number of written bytes, negative on failures, see write(2)
         */
        int syswrite(int fd, unsigned int maxlen=MAX_VAL(unsigned int));

        /*
         * Reads from a file descriptor and append to buffered data, update position indexes.
         * \param fd File descriptor
         * \return Number of read bytes, negative on failures, see read(2)
         */
        int sysread(int fd, unsigned int maxlen=MAX_VAL(unsigned int));


    protected:
        size_t r, w, m_nCapacity; // read/write positions, size
        char *m_buf;
    	acbuf(const acbuf&); // don't copy me
    	acbuf& operator=(const acbuf&);

//        friend class maintenance::tFmtSendObj;

};

/* This is a light-weight and less cumbersome implementation of ostringstream.
 *
 * What it also makes possible: use itself as a string, use alternative add() operators
 * for strings which can also specify the length, and it runs faster with zero-terminated strings.
 */
class ACNG_API tSS : public acbuf
{
public:
// map char array to buffer pointer and size
	inline tSS & operator<<(const char *val) { return add(val); }
	inline tSS & operator<<(cmstring& val) { return add(val); };
	inline tSS & operator<<(const acbuf& val) { return add(val.rptr(), val.size()); };

#define __tss_nbrfmt(x, h, y) { reserve_atleast(22); got(sprintf(wptr(), m_fmtmode == hex ? h : x, y)); return *this; }
	inline tSS & operator<<(int val) __tss_nbrfmt("%d", "%x", val);
	inline tSS & operator<<(unsigned int val) __tss_nbrfmt("%u", "%x", val);
	inline tSS & operator<<(long val) __tss_nbrfmt("%ld", "%lx", val);
	inline tSS & operator<<(unsigned long val) __tss_nbrfmt("%lu", "%lx", val);
	inline tSS & operator<<(long long val) __tss_nbrfmt("%lld", "%llx", val);
	inline tSS & operator<<(unsigned long long val) __tss_nbrfmt("%llu", "%llx", val);
#ifdef DEBUG
	inline tSS & operator<<(void* val) __tss_nbrfmt("ptr:%llu", "ptr:0x%llx", (long long unsigned) val);
#endif

    enum fmtflags : bool { hex, dec };
    inline tSS & operator<<(fmtflags mode) { m_fmtmode=mode; return *this;}

    operator mstring() const { return mstring(rptr(), size()); }
    inline size_t length() const { return size();}
    inline const char * data() const { return rptr();}

    inline tSS() : m_fmtmode(dec){}
    inline tSS(size_t sz) : m_fmtmode(dec) { setsize(sz); }
    inline tSS(const tSS &src) : acbuf(), m_fmtmode(src.m_fmtmode) { add(src.data(), src.size()); }
    // move ctor: steal resources and defuse dtor
    inline tSS(tSS&& src) : m_fmtmode(src.m_fmtmode) { m_buf = src.m_buf; src.m_buf = 0;
    m_nCapacity=src.m_nCapacity; r=src.r; w=src.w; }
    tSS & addEscaped(const char *fmt);
    inline tSS & operator<<(const char c) { reserve_atleast(1); *(wptr())=c; got(1); return *this;}
    inline tSS & clean() { clear(); return *this;}
    inline tSS & append(const char *data, size_t len) { add(data,len); return *this; }
    // similar to syswrite but adapted to socket behavior and reports error codes as HTTP status lines
    bool send(int nConFd, mstring* sErrorStatus=nullptr);
    bool recv(int nConFd, mstring* sErrorStatus=nullptr);

    inline tSS & add(const char *data, size_t len)
	{ reserve_atleast(len); memcpy(wptr(), data, len); got(len); return *this;}
	inline tSS & add(const char *val)
	{ if(val) return add(val, strlen(val)); else return add("(null)", 6); }
	inline tSS & add(cmstring& val) { return add((const char*) val.data(), (size_t) val.size());}


	template <typename Arg>
	static void Chain(tSS& fmter, const std::string& delimiter, Arg arg) {
		(void) delimiter;
		fmter << arg;
	}
	template <typename First, typename... Args>
	static void Chain(tSS& fmter, const std::string& delimiter, First first, Args... args) {
		Chain(fmter, delimiter, first);
		fmter << delimiter;
		Chain(fmter, delimiter, args...);
	}



protected:
	fmtflags m_fmtmode;
	/// make sure to have at least minWriteCapa bytes extra available for writing
	inline void reserve_atleast(size_t minWriteCapa)
	{
		if(w+minWriteCapa+1 < m_nCapacity) return;
		auto capaNew=2*(w+minWriteCapa);
		if(!setsize(capaNew)) throw std::bad_alloc();
	}
	inline tSS & appDosNL() { return add("\r\n", 2);}
};

}

#endif
