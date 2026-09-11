// One source, three platforms. Nothing here is platform-aware: no `cfg`, no
// preprocessor branch, no per-target source. The only thing that changes
// between a Linux binary, a WebAssembly module and an Android artifact is the
// `--target` on the command line.
//
// `import std` is the point. A target whose toolchain cannot compile a module
// interface unit would be worse than its absence in a module-first build tool,
// so this deliberately exercises the standard library module rather than a
// header, on every target the README lists.
import std;

int main() {
    std::vector<int> v{3, 1, 2};
    std::ranges::sort(v);
    std::print("{}-{}-{}\n", v[0], v[1], v[2]);
}
