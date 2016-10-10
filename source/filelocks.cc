#include "filelocks.h"

#include <utility>

#include "lockable.h"
#include "meta.h"

using namespace std;

namespace acng
{
base_with_condition g_mmapLocksMx;

decltype(TFileShrinkGuard::g_mmapLocks) TFileShrinkGuard::g_mmapLocks;

// little helper to exclude competing mmap and file shrinking operations
TFileShrinkGuard::~TFileShrinkGuard()
{
	lockguard g(g_mmapLocksMx);
	g_mmapLocks.erase(m_it);
	g_mmapLocksMx.notifyAll();
}

unique_ptr<TFileShrinkGuard> TFileShrinkGuard::Acquire(const struct stat& info)
{
	lockuniq g(g_mmapLocksMx);
	while(true) {
#ifdef COMPATGCC47
		auto res = g_mmapLocks.insert(make_pair(info.st_dev, info.st_ino));
#else
		auto res = g_mmapLocks.emplace(info.st_dev, info.st_ino);
#endif
		if(res.second) // true if freshly inserted, false if there was an entry already
		{
			auto ret = new TFileShrinkGuard();
			ret->m_it = res.first;
			return unique_ptr<TFileShrinkGuard>(ret);
		}
		g_mmapLocksMx.wait(g);
	}
}

}
