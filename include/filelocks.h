
#ifndef FILELOCKS_H_
#define FILELOCKS_H_

#include <memory>
#include <string>

namespace filelocks
{
struct flock
{
	~flock();
private:
	friend std::unique_ptr<flock> Acquire(const std::string& path);
	std::string path;
	flock(const std::string& p) : path(p){};
};
std::unique_ptr<flock> Acquire(const std::string& path);
};



#endif /* FILELOCKS_H_ */
