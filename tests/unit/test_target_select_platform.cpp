import std;
import mcpplibs.mtest;
import mcpp.toolchain.target_select;

using namespace mcpplibs::mtest;

// A request selects among the rows it reaches and the providers that serve them
// (SPEC-016 §4). `<arch>-freebsd` reaches the system's row, which a platform
// serves, and the musl identity, which openkal-platform serves; with no platform
// chosen the latter is taken. A platform the project chose for the request that
// no built-in provider names serves it: another package's provider does not
// compete, so the request keeps the system's row for the chosen platform.

namespace target = mcpp::toolchain::target;

namespace {
target::Context on_linux() {
    target::Context c;
    c.host = target::host_pair("linux", "x86_64");
    return c;
}
}  // namespace

UTest no_platform_takes_the_musl_identity{"TargetSelectPlatform/NoPlatformTakesTheMuslIdentity", [] {
    auto s = target::select("x86_64-freebsd", "x86_64", "freebsd", "", false, on_linux());
    expect(fatal(s.row != nullptr));
    expect(eq(std::string(s.row->triple), "x86_64-freebsd-musl"));
}};

UTest a_chosen_platform_keeps_the_system_row{"TargetSelectPlatform/AChosenPlatformKeepsTheSystemRow", [] {
    auto c = on_linux();
    c.platformChosen = [](std::string_view ref) { return ref == "path:/toy"; };
    auto s = target::select("x86_64-freebsd", "x86_64", "freebsd", "", false, c);
    expect(fatal(s.row != nullptr));
    expect(eq(std::string(s.row->triple), "x86_64-freebsd"));
    for (auto const& cand : s.candidates)
        if (cand.provider) expect(cand.provider->platform.empty()) << cand.provider->name;
}};

UTest a_written_env_is_kept{"TargetSelectPlatform/AWrittenEnvIsKept", [] {
    auto s = target::select("x86_64-freebsd-musl", "x86_64", "freebsd", "musl", true, on_linux());
    expect(fatal(s.row != nullptr));
    expect(eq(std::string(s.row->triple), "x86_64-freebsd-musl"));
}};
