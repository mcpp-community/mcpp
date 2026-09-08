import std;

// Declared by hand here, and defined by C++ that `toyc` wrote from
// `src/kernels/answer.toy`. The declarations are `extern "C"` for the same
// reason a device island's boundary is: the two sides are produced by
// different compilers and do not share a C++ ABI.
extern "C" int toy_gcd(int a, int b);
extern "C" int toy_scale(int v, int k);
extern "C" int toy_answer();

int main() {
    std::println("gcd(1071, 462) = {}", toy_gcd(1071, 462));
    std::println("scale(21, 2)   = {}", toy_scale(21, 2));
    std::println("answer()       = {}", toy_answer());
    return toy_answer() == 42 ? 0 : 1;
}
