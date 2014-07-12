
#ifndef FILELOCKS_H_
#define FILELOCKS_H_

#include <memory>
#include <string>

namespace filelocks
{
struct flock
{
	~flock();
	flock(const std::string& p) : path(p){};
	std::string path;
};
std::unique_ptr<flock> Acquire(const std::string& path);
};



#endif /* FILELOCKS_H_ */
