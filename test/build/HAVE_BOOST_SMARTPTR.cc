#include <boost/smart_ptr.hpp>
int main() { return NULL != boost::shared_ptr<int>(new int(1)); }

