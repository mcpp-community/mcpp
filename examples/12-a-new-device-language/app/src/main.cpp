import std;

// Defined by C++ that `toyc` wrote from `src/kernels/answer.toy`. The
// declaration is `extern "C"` for the same reason a device island's is: the two
// sides are produced by different compilers and do not share a C++ ABI.
extern "C" int toy_answer();

int main() {
    const int v = toy_answer();
    std::println("toy_answer() = {}", v);
    return v == 42 ? 0 : 1;
}
