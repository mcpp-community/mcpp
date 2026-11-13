import std;
import canary;

int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](bool held, std::string_view what) {
        std::println("{} {}", held ? "ok  " : "FAIL", what);
        if (!held) ++failures;
    };
    std::println("canary-cxx: {} argument(s)", argc);
    check(canary::parallel_sum(1000) == 500500, "four threads sum 1..1000");
    check(canary::thrown_and_caught() == "carried", "an exception is thrown and caught");
    check(std::format("{:>4}", 42) == "  42", "std::format");
    check(argc < 2 || std::string_view(argv[argc - 1]) == "last", "the arguments arrive");
    return failures;
}
