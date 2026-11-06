// [temp] probe 885: mcpplibs.os lock::acquire under contention, as mcpp's
// compiler slots take it, counting each error it reports.
import std;
import mcpplibs.os.fs;

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::map<std::string, int> seen;
    for (int round = 0; round < 3000; ++round) {
        for (int i = 0; i < 4; ++i) {
            auto lock = mcpplibs::os::fs::lock::acquire(dir / std::format("slot-{}.lock", i),
                                                        mcpplibs::os::fs::lock::kind::exclusive, false);
            if (lock) {
                ++seen["acquired"];
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                break;
            }
            const auto message = lock.error().message();
            ++seen[message.substr(message.rfind(':') + 1)];
        }
    }
    for (auto& [what, n] : seen) std::println("{}: {}", what, n);
}
