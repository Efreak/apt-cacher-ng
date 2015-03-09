#include <memory>
int main() { return NULL != std::shared_ptr<int>(new int(1)); }

