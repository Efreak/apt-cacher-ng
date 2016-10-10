#ifndef _HEADER_H
#define _HEADER_H

#include <string>
#include <unordered_set>
#include <map>
#include "meta.h"

class header {
   public:
      enum eHeadType : char
	  {
         INVALID,
         HEAD,
         GET,
         POST,
         CONNECT,
         ANSWER
      };
      enum eHeadPos : char
	  {
    	  CONNECTION,			// 0
    	  CONTENT_LENGTH,
    	  IF_MODIFIED_SINCE,
    	  RANGE,
    	  IFRANGE,				// 4
    	  CONTENT_RANGE,
    	  LAST_MODIFIED,
    	  PROXY_CONNECTION,
    	  TRANSFER_ENCODING,
    	  XORIG,
    	  AUTHORIZATION,		// 10
    	  XFORWARDEDFOR,
    	  LOCATION,
    	  CONTENT_TYPE,
    	  // unreachable entry and size reference
    	  HEADPOS_MAX,
		  HEADPOS_NOTFORUS
      };
#define ACNGFSMARK XORIG

      eHeadType type = INVALID;
      mstring frontLine;
      
      char *h[HEADPOS_MAX] = {0};
                           
      inline header(){};
      ~header();
      header(const header &);
      header& operator=(const header&); 
      
      static mstring GenInfoHeaders();
      static bool ParseDate(const char *, struct tm*);

      int LoadFromFile(const mstring & sPath);
      
      //! returns byte count or negative errno value
      int StoreToFile(cmstring &sPath) const;
      
      void set(eHeadPos, const mstring &value);
      void set(eHeadPos, const char *val);
      void set(eHeadPos, const char *s, size_t len);
      void set(eHeadPos, off_t nValue);
      void prep(eHeadPos, size_t length);
      void del(eHeadPos);
      inline void copy(const header &src, eHeadPos pos) { set(pos, src.h[pos]); };

      inline const char * getCodeMessage() const {
    	  return frontLine.length()>9 ? frontLine.c_str()+9 : "";
      }
      inline int getStatus() const { int r=atoi(getCodeMessage()); return r ? r : 500; }
      void clear();
      
      tSS ToString() const;
      static std::vector<tPtrLen> GetKnownHeaders();
      /**
       * Read buffer to parse one string. Optional offset where to begin to
       * scan.
       *
       * @param src Pointer to raw input
       * @param length Maximum considered input length
       * @param unkFunc Optional callback for ignored headers (called with key name and rest of the line including CRLF)
       * @return Length of processed data,
       * 0: incomplete, needs more data
       * <0: error
       * >0: length of the processed data
       */
      // XXX: maybe redesign the unkFunc to just use pointer and length instead of strings
      int Load(const char *src, unsigned length, const std::function<void(cmstring&, cmstring&)> &unkFunc = std::function<void(cmstring&, cmstring&)>());
};

inline bool BODYFREECODE(int status)
{
	// no response if not-modified or similar, following http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
	return (304 == status || (status>=100 && status<200) || 204==status);
}

#endif
