#include <gtest/gtest.h>

import std;
import mcpp.build.schedule.detach_codegen;

// A detached compile reports its status in `<slot>.rc`, which appears by a
// rename once the number is written. One that holds no number is a write that
// failed (a full disk); the waiter takes the unit as failed at once instead of
// waiting for a number that will never come.

namespace fs = std::filesystem;
namespace detach = mcpp::build::schedule::detach;

namespace {
struct Slot {
    fs::path dir, slot;
    Slot() {
        dir = fs::temp_directory_path() / std::format("mcpp-detach-{:08x}", std::random_device{}());
        fs::create_directories(dir);
        slot = dir / "unit.m.o.sched";
        std::ofstream(slot.string() + ".log") << "";
    }
    ~Slot() { std::error_code ec; fs::remove_all(dir, ec); }
};
}  // namespace

TEST(DetachStatus, AStatusFileWithNoNumberIsAFailedUnit) {
    Slot s;
    std::ofstream(s.slot.string() + ".rc") << "";
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NE(detach::await_unit(s.slot, {}), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
}

TEST(DetachStatus, AWrittenStatusIsTheUnits) {
    Slot s;
    std::ofstream(s.slot.string() + ".rc") << "3\n";
    EXPECT_EQ(detach::await_unit(s.slot, {}), 3);
    Slot t;
    std::ofstream(t.slot.string() + ".rc") << "0\n";
    EXPECT_EQ(detach::await_unit(t.slot, {}), 0);
}
