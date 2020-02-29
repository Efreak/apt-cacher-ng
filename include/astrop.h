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

//! iterator-like helper for string splitting, for convenient use with for-loops
// Works exactly once!
class tSplitWalk
{
	const std::string &s;
	mutable std::string::size_type start, len, oob;
	const char* m_seps;

public:
	inline tSplitWalk(const std::string *line, const char* separators=SPACECHARS, unsigned begin=0)
	: s(*line), start(begin), len(std::string::npos), oob(line->size()), m_seps(separators) {}
	inline bool Next() const
	{
		if(len != std::string::npos) // not initial state, find the next position
			start = start + len + 1;

		if(start>=oob)
			return false;

		start = s.find_first_not_of(m_seps, start);

		if(start<oob)
		{
			len = s.find_first_of(m_seps, start);
			len = (len == std::string::npos) ? oob-start : len-start;
		}
		else if (len != std::string::npos) // not initial state, end reached
			return false;
		else if(s.empty()) // initial state, no parts
			return false;
		else // initial state, use the whole string
		{
			start = 0;
			len = oob;
		}

		return true;
	}
	inline std::string str() const { return s.substr(start, len); }
	inline operator std::string() const { return str(); }
	inline const char* remainder() const { return s.c_str() + start; }
	inline string_view view() const { return string_view(s.data() + start, len); }

	struct iterator
	{
		tSplitWalk* _walker = nullptr;
		// default is end sentinel
		bool bEol = true;
		iterator() {}
		iterator(tSplitWalk& walker) : _walker(&walker) { bEol = !walker.Next(); }
		// just good enough for basic iteration and end detection
		bool operator==(const iterator& other) const { return (bEol && other.bEol); }
		bool operator!=(const iterator& other) const { return !(other == *this); }
		iterator operator++() { bEol = !_walker->Next(); return *this; }
		std::string operator*() { return _walker->str(); }
	};
	iterator begin() {return iterator(*this); }
	iterator end() { return iterator(); }
};

inline void trimFront(std::string &s, const char* junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(0, pos);
}

inline string_view trimFront(string_view s, const char* junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	if(pos == std::string::npos)
		return string_view(s.data(),0);
	return string_view(s.data() + pos, s.size()-pos);
}

inline void trimBack(std::string &s, const char* junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(pos+1);
}

inline string_view trimBack(string_view s, const char* junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	if(pos == std::string::npos)
		return string_view(s.data(),0);
	return string_view(s.data(), pos+1);
}

inline void trimBoth(std::string &s, const char* junk=SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
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

}


#endif /* INCLUDE_ASTROP_H_ */
