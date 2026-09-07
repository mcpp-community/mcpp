import std;
import app.saxpy;

int main() {
    const std::vector<float> x{1, 2, 3, 4}, y{10, 20, 30, 40};
    auto out = app::saxpy(2.0f, x, y);
    if (!out) { std::println("device unavailable"); return 1; }
    for (auto v : *out) std::print("{} ", v);
    std::println("");
    // After the call, never before: the name records a run that happened.
    std::println("device: {}", app::device_name());
    const std::vector<float> want{12, 24, 36, 48};
    return *out == want ? 0 : 1;
}
