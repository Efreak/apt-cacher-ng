#include <boost/smart_ptr.hpp>
int main() { boost::shared_ptr<int> x(new int(1)); return *x; }


