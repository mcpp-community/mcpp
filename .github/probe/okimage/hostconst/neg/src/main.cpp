#include "../../shape.hpp"
#if !defined(SHAPE_B)
platform runtime_host() noexcept { return PLATFORM; }
#else
platform host_ok() noexcept { return PLATFORM; }
#endif
// Must be refused: on the ok target the host is not known when compiling.
static_assert(host_ok() == PLATFORM);
int main() { return 0; }
