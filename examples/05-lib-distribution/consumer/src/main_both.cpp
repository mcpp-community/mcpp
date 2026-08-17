// Both at once, from the same package. The two interface modes do not
// interfere: one is preprocessor input, the other is compiler input.
import std;
import mathkit;

#include <mathkit_c.h>

int main() {
    std::println("both   : c={} module={}", mathkit_add(2, 3), mk::add(2, 3));
    return 0;
}
