#include "filelocks.h"

#include <unordered_set>

#include "lockable.h"
#include "meta.h"

namespace filelocks {

struct : public condition, public std::unordered_set<mstring> {} g_mmapLocks;

#warning usecase wann?
filelocks::flock::~flock()
{
	lockguard g(g_mmapLocks);
	g_mmapLocks.erase(this->path);
}

std::unique_ptr<flock> Acquire(const std::string& path)
{
	lockguard g(g_mmapLocks);
	while(true) {
		auto res(g_mmapLocks.emplace(path));
		if(res.second) // true if freshly inserted, false if there was an entry already
			break;
		g_mmapLocks.wait();
	}
	return std::unique_ptr<flock>(new flock(path));
}

}
