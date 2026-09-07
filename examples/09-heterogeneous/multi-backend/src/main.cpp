import std;
import opkit;

int main() {
    const std::vector<float> x{1, 2, 3, 4}, y{10, 20, 30, 40};

    auto out = opkit::saxpy(2.0f, x, y);
    if (!out) { std::println("no backend served the call"); return 1; }

    // Printed after the call, never before: the name records a run that
    // happened rather than predicting one.
    std::println("backend: {}", opkit::backend());
    for (auto v : *out) std::print("{} ", v);
    std::println("");

    const std::vector<float> want{12, 24, 36, 48};
    return *out == want ? 0 : 1;
}
