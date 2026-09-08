import std;

// THE CONSUMER IMPORTS THE GENERATED MODULE. There is no seam in this project
// and no header in its source tree: `boundary.kernels` is written into the
// build directory by `mcpp.tools.island` during this build.
//
// What arrives is the island's own interface -- pointers and a count, an
// `extern "C"` signature. `../cuda` adds a hand-written seam over exactly this
// module and turns it into spans; the README states what each rung costs.
import boundary.kernels;

int main() {
    const float x[]{ 1, 2, 3, 4 };
    const float y[]{ 10, 20, 30, 40 };
    float out[4]{};

    if (saxpy_device(2.0f, x, y, out, 4) != 0) {
        std::println(std::cerr, "saxpy_device failed");
        return 1;
    }
    for (int i = 0; i < 4; ++i) std::print("{}{}", out[i], i == 3 ? "\n" : " ");
    std::println("ran on: {}", saxpy_device_name());
    return 0;
}
