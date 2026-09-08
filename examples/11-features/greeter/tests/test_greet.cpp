// `mcpp test` compiles every `tests/**/*.cpp` as its own program. This one uses
// `counters`, which reaches it through `[dev-dependencies]` -- a dependency the
// artifact never links.
import std;
import greeter;
import counters;

int main() {
    const auto s = greeter::greet("x");
    if (s.empty()) { std::println(std::cerr, "greet returned nothing"); return 1; }
    counters::record();
    if (counters::calls != 1) { std::println(std::cerr, "counters did not record"); return 1; }
    std::println("ok: {} / {}", s, counters::report());
    return 0;
}
