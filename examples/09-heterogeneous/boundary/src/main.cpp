import std;

// THE CONSUMER IMPORTS THE GENERATED MODULE. There is no seam in this project
// and no header in its source tree: `boundary.kernels` is written into the
// build directory by `mcpp.tools.island` during this build.
//
// What arrives is the island's own interface -- pointers and a count, an
// `extern "C"` signature -- in the namespace the module name spells, extended
// by the directories under the root. `../cuda` adds a hand-written seam over
// exactly this module and turns it into spans; the README states what each
// rung costs.
import boundary.kernels;

int main() {
    const float x[]{ 1, 2, 3, 4 };
    const float y[]{ 10, 20, 30, 40 };
    float out[4]{};

    // `saxpy` is the short spelling of `boundary_saxpy`, which is the symbol.
    // Both are exported and both name one entity.
    if (boundary::kernels::saxpy(2.0f, x, y, out, 4) != 0) {
        std::println(std::cerr, "boundary_saxpy failed");
        return 1;
    }
    // One directory deeper, so one namespace deeper. Nothing in this call
    // names a file.
    if (boundary::kernels::vec::scale(0.5f, out, 4) != 0) {
        std::println(std::cerr, "boundary_scale failed");
        return 1;
    }
    for (int i = 0; i < 4; ++i) std::print("{}{}", out[i], i == 3 ? "\n" : " ");
    std::println("ran on: {}", boundary::kernels::ran_on());
    return 0;
}
