
#ifndef FILELOCKS_H_
#define FILELOCKS_H_

#include <memory>
#include <set>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct TFileShrinkGuard
{
	static std::unique_ptr<TFileShrinkGuard> Acquire(const struct stat&);
	~TFileShrinkGuard();

private:
	static std::set<std::pair<dev_t,ino_t > > g_mmapLocks;
	decltype(g_mmapLocks)::iterator m_it;
	TFileShrinkGuard() =default;
};


#endif /* FILELOCKS_H_ */
