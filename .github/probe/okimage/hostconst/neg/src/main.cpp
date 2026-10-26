#include "../../shape.hpp"
platform runtime_host() noexcept { return PLATFORM; }
// Must be refused: on the ok target the host is not known when compiling.
static_assert(host_ok() == PLATFORM);
int main() { return 0; }
