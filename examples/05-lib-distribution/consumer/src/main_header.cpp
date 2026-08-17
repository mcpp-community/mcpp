// Consuming through the TEXT interface: no `import` of the library at all, so
// nothing of the package is compiled here. The header is preprocessor input;
// the code it declares lives in the prebuilt artifact.
import std;

#include <mathkit_c.h>

int main() {
    std::println("header : mathkit_add(2, 3) = {}", mathkit_add(2, 3));
    return 0;
}
