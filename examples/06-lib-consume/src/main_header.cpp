// Consuming through the TEXT interface: no `import`, nothing of the package is
// compiled. The header is preprocessor input; the code is in the prebuilt lib.
#include <cstdio>
#include <mathkit_c.h>

int main() {
    std::printf("header : mathkit_add(2, 3) = %d\n", mathkit_add(2, 3));
    return 0;
}
