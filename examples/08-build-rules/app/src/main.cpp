#include <cstdio>

// Provided by the object the `embed` rule generates.
extern "C" const char* greeting_data();

int main() {
    std::printf("%s", greeting_data());
    return 0;
}
