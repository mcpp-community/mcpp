
import std;
import mcpplibs.mtest;
import mcpp.build.prepare;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

namespace legs = mcpp::build::legs;

UTest native_architecture_first_the_rest_in_order{"BuildLegs/NativeArchitectureFirstTheRestInOrder", [] {
    const auto host = mcpp::toolchain::triple::host_triple().arch;
    const std::string other = host == "x86_64" ? "aarch64" : "x86_64";
    std::vector<std::string> in{other + "-openkal", host + "-openkal", other + "-linux-musl",
                                host + "-linux-musl"};
    auto out = legs::native_first(in);
    expect(eq(out, (std::vector<std::string>{host + "-openkal", host + "-linux-musl",
                                             other + "-openkal", other + "-linux-musl"})));
}};

UTest first_failure_is_the_first_non_zero{"BuildLegs/FirstFailureIsTheFirstNonZero", [] {
    expect(eq(legs::first_failure(std::vector<int>{0, 0}), 0));
    expect(eq(legs::first_failure(std::vector<int>{0, 2, 1}), 2));
    expect(eq(legs::first_failure(std::vector<int>{}), 0));
}};

// One directory is one ninja's: work in it runs one item after another, and
// distinct directories run at the same time.
UTest one_directory_is_worked_one_item_at_a_time{"BuildLegs/OneDirectoryIsWorkedOneItemAtATime", [] {
    std::vector<std::filesystem::path> dirs{"a", "b", "a", "b", "a"};
    std::mutex m;
    std::map<std::string, int> inside, most;
    std::atomic<int> total{0};
    auto results = legs::run_by_directory(dirs, [&](std::size_t i) {
        const auto key = dirs[i].string();
        {
            std::scoped_lock l(m);
            most[key] = std::max(most[key], ++inside[key]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        {
            std::scoped_lock l(m);
            --inside[key];
        }
        ++total;
        return static_cast<int>(i);
    });
    expect(eq(total.load(), 5));
    expect(eq(most["a"], 1));
    expect(eq(most["b"], 1));
    for (std::size_t i = 0; i < results.size(); ++i) expect(eq(results[i], static_cast<int>(i)));
}};
