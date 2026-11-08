
import std;
import mcpplibs.mtest;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL. This package imports nothing from mcpp, so these tests can
// only see what the package itself promises — which is the point. The
// engine-side tests in the root `tests/unit/` exercise the same vocabulary
// through resolution, so a failure here means the vocabulary changed, while a
// failure there means a consumer was relying on more than it was told.

UTest the_host_platform_key_is_never_empty{"PlatformAxis/TheHostPlatformKeyIsNeverEmpty", [] {
    // Every descriptor lookup is keyed on this. An empty key would not fail —
    // it would match the section named "", which no descriptor has, and the
    // package would simply appear to be unavailable everywhere.
    auto host = mcpp::toolchain::platform::HostPlatform::current();
    expect(!(host.key().empty()));
}};

UTest two_keys_compare_by_value{"PlatformAxis/TwoKeysCompareByValue", [] {
    // `PlatformKey` is only obtainable through an axis type, so possession of
    // one already says which axis it came from. Equality therefore has to be
    // about the key and not the object.
    auto a = mcpp::toolchain::platform::HostPlatform::current();
    auto b = mcpp::toolchain::platform::HostPlatform::current();
    expect(a == b);
}};

UTest a_target_reads_its_rows_blocks_and_never_the_hosts{"PlatformAxis/ATargetReadsItsRowsBlocksAndNeverTheHosts", [] {
    // Design 2026-10-29 §5.1. Before 2026.10.30.1 a token naming none of the
    // three fell back to the HOST's block (#851).
    using mcpp::toolchain::platform::TargetPlatform;
    auto keys = [](std::string_view os) {
        auto t = TargetPlatform::for_os(os);
        return std::vector<std::string>(t.keys().begin(), t.keys().end());
    };
    expect(eq(keys("linux"), (std::vector<std::string>{"linux"})));
    expect(eq(keys("macos"), (std::vector<std::string>{"macosx"})));
    expect(eq(keys("windows"), (std::vector<std::string>{"windows"})));
    expect(eq(keys("emscripten"), (std::vector<std::string>{"emscripten", "linux"})));
    expect(eq(keys("ios"), (std::vector<std::string>{"ios", "macosx"})));
    expect(eq(keys("openkal"), (std::vector<std::string>{"openkal", "linux"})));
    expect(eq(keys("none"), (std::vector<std::string>{"none"})));
    // The primary key is the one messages name.
    expect(eq(TargetPlatform::for_os("emscripten").key(), "emscripten"));
    // A platform's own keys replace the row's.
    auto stated = TargetPlatform::with_keys({"openkal"});
    expect(eq(stated.key(), "openkal"));
    expect(!(stated == TargetPlatform::for_os("openkal")));
}};
