#include <memory>
int main() { std::shared_ptr<int> x(new int(1)); return *x; }

