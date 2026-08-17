// Both at once, from the same package. The two interface modes do not
// interfere: one is preprocessor input, the other is compiler input.
#include <cstdio>
#include <mathkit_c.h>

import mathkit;

int main() {
    std::printf("both   : c=%d module=%d\n", mathkit_add(2, 3), mk::add(2, 3));
    return 0;
}
