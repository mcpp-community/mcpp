// Consuming through the MODULE interface. mcpp compiles the package's
// published `.cppm` to get a BMI — cheap, they are declarations — and links
// the definitions out of the prebuilt archive.
//
// NB: `#include` before `import`. GCC 16 mis-scopes headers included after a
// module import in a non-module TU, and the error it reports names neither.
#include <cstdio>

import mathkit;

int main() {
    std::printf("module : mk::add(2, 3) = %d, mk::scale(2.0) = %.1f\n",
                mk::add(2, 3), mk::scale(2.0));
    return 0;
}
