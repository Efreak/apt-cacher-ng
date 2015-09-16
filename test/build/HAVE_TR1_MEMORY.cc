#include <tr1/memory>
int main() { std::tr1::shared_ptr<int> x(new int(1)); return *x; }


