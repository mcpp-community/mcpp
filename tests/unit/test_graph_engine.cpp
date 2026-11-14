// The engine a build directory's graph runs (design 2026-11-12 §17, W1): the
// directory's link to the running engine, whose path carries the epoch and
// holds no version.
import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.build.graph_engine;

using namespace mcpplibs::mtest;
namespace ge = mcpp::build::graph_engine;

namespace {
struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_graph_engine_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

UTest the_link_is_the_running_engine{"GraphEngine/TheLinkIsTheRunningEngine", [] {
    Tmp dir;
    const auto named = ge::refresh(dir.path, 4);
    expect(eq(named, ge::link(dir.path, 4)));
    expect(eq(named.parent_path().filename().string(), "epoch-4"));
    expect(ge::is_this_engine(named));
    expect(ge::is_this_engine(mcpp::invocation::self_exe_path()));
    // Made again, it is left as it is.
    const auto again = ge::refresh(dir.path, 4);
    expect(eq(again, named));
}};

UTest another_epoch_replaces_the_link{"GraphEngine/AnotherEpochReplacesTheLink", [] {
    Tmp dir;
    (void)ge::refresh(dir.path, 4);
    const auto named = ge::refresh(dir.path, 5);
    expect(eq(named.parent_path().filename().string(), "epoch-5"));
    std::error_code ec;
    expect(!std::filesystem::exists(ge::link(dir.path, 4), ec));
}};

UTest another_program_is_not_this_engine{"GraphEngine/AnotherProgramIsNotThisEngine", [] {
    Tmp dir;
    const auto other = dir.path / "mcpp";
    std::ofstream(other, std::ios::binary) << "not the engine";
    expect(!ge::is_this_engine(other));
    expect(!ge::is_this_engine(dir.path / "absent"));
}};
