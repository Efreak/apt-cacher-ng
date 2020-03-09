/*
 * astrop.h
 *
 *  Created on: 28.02.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_ASTROP_H_
#define INCLUDE_ASTROP_H_

#include <string>
#include "string_view.hpp"

#define SPACECHARS " \f\n\r\t\v"

namespace acng
{

using string_view = nonstd::string_view;

inline void trimFront(std::string &s, const char* junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(0, pos);
}

inline void trimFront(string_view& s, const char* junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	s.remove_prefix(pos == std::string::npos ? s.length() : pos);
}


inline void trimBack(std::string &s, const char* junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(pos+1);
}

inline void trimBack(string_view& s, const char* junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	s.remove_suffix(pos != std::string::npos ? s.size() - pos - 1 : s.length());
}

inline void trimBoth(std::string &s, const char* junk=SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}

inline void trimBoth(string_view &s, const char* junk=SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}


//! iterator-like helper for string splitting, for convenient use with for-loops
// Works exactly once!
class tSplitWalk
{
	string_view m_input;
	mutable std::string::size_type m_sliece_len;
	const char* m_seps;
	bool m_strict_delimiter, m_first;

public:
	/**
	 * @param line The input
	 * @param separators Characters which are considered delimiters (any char in that string)
	 * @param strictDelimiter By default, a sequence of separator chars are considered as one delimiter. This is normally good for whitespace but bad for specific visible separators. Setting this flag makes them be considered separately, returning empty strings as value is possible then.
	 */
	inline tSplitWalk(string_view line, const char* separators, bool strictDelimiter)
	: m_input(line), m_sliece_len(0), m_seps(separators), m_strict_delimiter(strictDelimiter), m_first(true)
	{}
	void reset(string_view line)
	{
		m_input=line;
		m_sliece_len=0;
		m_first=true;
	}
	inline bool Next()
	{
		if (m_input.length() == m_sliece_len)
			return false;
		m_input.remove_prefix(m_sliece_len);

		if (m_strict_delimiter)
		{
			if(!m_first)
				m_input.remove_prefix(1);
			else
				m_first = false;
			m_sliece_len = m_input.find_first_of(m_seps);
			// XXX: for the strict mode, the calculation of slice length could be made in lazy way. So, either in slice getter or here in Next but not beforehand.
			// However, it is quite likely that the next slice will also be needed, and additional branches here will cost more than they bring.
			if (m_sliece_len == std::string::npos)
				m_sliece_len = m_input.length();
			return true;
		}
		else
		{
			trimFront(m_input, m_seps);
			if(m_input.empty())
				return false;
			m_sliece_len = m_input.find_first_of(m_seps);
			if (m_sliece_len == std::string::npos)
				m_sliece_len = m_input.length();
			return true;
		}
	}
	inline std::string str() const { return std::string(m_input.data(), m_sliece_len); }
	inline operator std::string() const { return str(); }
//	inline string_view remainder() const { return s.c_str() + start; }
	inline string_view view() const { return string_view(m_input.data(), m_sliece_len); }

	struct iterator :
			public std::iterator<
			                        std::input_iterator_tag,   // iterator_category
			                        string_view,                      // value_type
			                        string_view,                      // difference_type
			                        const long*,               // pointer
			                        string_view                       // reference
			                                      >
	{
		tSplitWalk* _walker = nullptr;
		// default is end sentinel
		bool bEol = true;
		iterator() {}
		iterator(tSplitWalk& walker) : _walker(&walker) { bEol = !walker.Next(); }
		// just good enough for basic iteration and end detection
		bool operator==(const iterator& other) const { return (bEol && other.bEol); }
		bool operator!=(const iterator& other) const { return !(other == *this); }
		iterator& operator++() { bEol = !_walker->Next(); return *this; }
	    iterator operator++(int) {iterator retval = *this; ++(*this); return retval;}
		auto operator*() { return _walker->view(); }
	};
	iterator begin() {return iterator(*this); }
	iterator end() { return iterator(); }
};

inline int strcasecmp(string_view a, string_view b)
{
	if(a.length() < b.length())
		return INT_MAX;
	if(a.length() > b.length())
		return INT_MIN;
	return strcasecmp(a, b);
}

#define trimLine(x) { trimFront(x); trimBack(x); }

#define startsWith(where, what) (0==(where).compare(0, (what).size(), (what)))
#define endsWith(where, what) ((where).size()>=(what).size() && \
		0==(where).compare((where).size()-(what).size(), (what).size(), (what)))
#define startsWithSz(where, what) (0==(where).compare(0, sizeof((what))-1, (what)))
#define endsWithSzAr(where, what) ((where).size()>=(sizeof((what))-1) && \
		0==(where).compare((where).size()-(sizeof((what))-1), (sizeof((what))-1), (what)))
#define stripSuffix(where, what) if(endsWithSzAr(where, what)) where.erase(where.size()-sizeof(what)+1);
#define stripPrefixChars(where, what) where.erase(0, where.find_first_not_of(what))

#define setIfNotEmpty(where, cand) { if(where.empty() && !cand.empty()) where = cand; }
#define setIfNotEmpty2(where, cand, alt) { if(where.empty()) { if(!cand.empty()) where = cand; else where = alt; } }

std::string GetBaseName(const std::string &in);
std::string GetDirPart(const std::string &in);
std::pair<std::string,std::string> SplitDirPath(const std::string& in);


void fish_longest_match(const char *stringToScan, size_t len, const char sep,
		std::function<bool(const std::string&)> check_ex);

void replaceChars(std::string &s, const char* szBadChars, char goodChar);
inline std::string to_string(string_view s) {return std::string(s.data(), s.length());}



}


#endif /* INCLUDE_ASTROP_H_ */
