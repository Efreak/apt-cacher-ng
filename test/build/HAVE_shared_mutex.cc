#include <shared_mutex>
int main()
{ std::shared_mutex m; return sizeof(m);}
