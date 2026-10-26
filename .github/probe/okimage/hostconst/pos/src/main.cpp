#include "../../shape.hpp"
#include <cstdio>
platform runtime_host() noexcept { return PLATFORM; }
static_assert(host() == PLATFORM);                     // usable at compile time
int main() {
    if constexpr (host() == platform::windows) std::puts("if constexpr (host() == windows): taken");
    else std::puts("if constexpr (host() == windows): not taken");
    const platform h = host_ok();                         // run time: asks
    std::printf("host_ok() at run time = %d (PLATFORM %d)\n", (int)h, (int)PLATFORM);
    return h == PLATFORM ? 0 : 1;
}
