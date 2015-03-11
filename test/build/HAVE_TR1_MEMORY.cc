#include <tr1/memory>
int main() { return NULL != std::tr1::shared_ptr<int>(new int(1)); }

