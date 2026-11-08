// The selection rule of SPEC-016 §4 over the built-in rows and providers: a
// request is a pattern, explicit choices win, and the candidates are ordered by
// priority and taken by availability on the host. The host is a parameter here,
// so every host's answer is stated on every host.
import std;
import mcpplibs.mtest;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_select;

using namespace mcpplibs::mtest;
namespace target = mcpp::toolchain::target;

namespace {

struct Ask {
    std::string_view request, arch, os, env;
    bool envWritten = false;
};

target::Selection on(std::string_view host, Ask a, target::Context ctx = {}) {
    ctx.host = std::string(host);
    return target::select(a.request, a.arch, a.os, a.env, a.envWritten, ctx);
}

std::string provider_of(const target::Selection& s) {
    return s.provider ? std::string(s.provider->name) : std::string("-");
}

const Ask kLinux{"x86_64-linux", "x86_64", "linux", "gnu"};
const Ask kWindows{"x86_64-windows", "x86_64", "windows", "gnu"};
const Ask kMacos{"aarch64-macos", "aarch64", "macos", ""};

} // namespace

UTest a_pattern_takes_the_hosts_own_provider{"TargetSelect/APatternTakesTheHostsOwnProvider", [] {
    auto s = on("linux-x86_64", kLinux);
    expect(eq(s.identity(), "x86_64-linux-gnu"));
    expect(eq(provider_of(s), "glibc"));
    expect(!s.fallback);
    auto m = on("macos-aarch64", kMacos);
    expect(eq(m.identity(), "aarch64-macos"));
    expect(eq(provider_of(m), "apple"));
}};

UTest a_pattern_falls_back_to_the_next_available{"TargetSelect/APatternFallsBackToTheNextAvailable", [] {
    // No glibc and no musl-gcc payload exists for a macOS host: openkal-platform
    // serves the musl identity there, and the selection says it fell back.
    auto s = on("macos-aarch64", kLinux);
    expect(eq(s.identity(), "x86_64-linux-musl"));
    expect(eq(provider_of(s), "openkal"));
    expect(s.fallback);
    auto m = on("linux-x86_64", kMacos);
    expect(eq(m.identity(), "aarch64-macos-musl"));
    expect(eq(provider_of(m), "openkal"));
}};

UTest windows_takes_msvc_then_openkal_then_mingw{"TargetSelect/WindowsTakesMsvcThenOpenkalThenMingw", [] {
    target::Context withMsvc;
    withMsvc.probe = [](std::string_view p) { return p == "msvc"; };
    expect(eq(on("windows-x86_64", kWindows, withMsvc).identity(), "x86_64-windows-msvc"));
    // Q4: a Windows host without MSVC, and every other host, take the openkal
    // platform, whose priority is above MinGW's.
    expect(eq(on("windows-x86_64", kWindows).identity(), "x86_64-windows-musl"));
    auto l = on("linux-x86_64", kWindows);
    expect(eq(l.identity(), "x86_64-windows-musl"));
    expect(eq(provider_of(l), "openkal"));
}};

UTest a_declared_family_keeps_the_providers_it_pairs_with{"TargetSelect/ADeclaredFamilyKeepsTheProvidersItPairsWith", [] {
    target::Context gcc;
    gcc.family = "gcc";
    auto s = on("linux-x86_64", kWindows, gcc);
    expect(eq(s.identity(), "x86_64-windows-gnu"));
    expect(eq(provider_of(s), "mingw"));
    target::Context llvm;
    llvm.family = "llvm";
    auto g = on("linux-x86_64", {"x86_64-windows-gnu", "x86_64", "windows", "gnu", true}, llvm);
    expect(eq(g.identity(), "x86_64-windows-gnu"));
    expect(eq(provider_of(g), "mingw"));
}};

UTest an_explicit_identity_is_not_replaced{"TargetSelect/AnExplicitIdentityIsNotReplaced", [] {
    const Ask musl{"x86_64-linux-musl", "x86_64", "linux", "musl", true};
    expect(eq(provider_of(on("linux-x86_64", musl)), "musl-gcc"));
    auto m = on("macos-aarch64", musl);
    expect(eq(m.identity(), "x86_64-linux-musl"));
    expect(eq(provider_of(m), "openkal"));
    const Ask gnu{"x86_64-windows-gnu", "x86_64", "windows", "gnu", true};
    expect(eq(on("linux-x86_64", gnu).identity(), "x86_64-windows-gnu"));
}};

UTest a_provider_chosen_by_name_serves_alone{"TargetSelect/AProviderChosenByNameServesAlone", [] {
    target::Context mingw;
    mingw.provider = "mingw";
    expect(eq(on("linux-x86_64", kWindows, mingw).identity(), "x86_64-windows-gnu"));
    target::Context none;
    none.provider = "emsdk";
    expect(on("linux-x86_64", kWindows, none).candidates.empty());
}};

UTest a_priority_is_data_a_project_may_change{"TargetSelect/APriorityIsDataAProjectMayChange", [] {
    target::Context ctx;
    ctx.priority = [](const target::Row&, std::string_view p) -> std::optional<int> {
        if (p == "mingw") return 200;
        return std::nullopt;
    };
    auto s = on("linux-x86_64", kWindows, ctx);
    expect(eq(s.identity(), "x86_64-windows-gnu"));
    expect(eq(s.priority, 200));
}};

UTest android_is_reached_by_name_only{"TargetSelect/AndroidIsReachedByNameOnly", [] {
    for (auto const& c : on("linux-x86_64", kLinux).candidates)
        expect(eq(c.row->env == "android", false)) << c.row->triple;
    const Ask android{"x86_64-linux-android", "x86_64", "linux", "android", true};
    expect(eq(on("linux-x86_64", android).identity(), "x86_64-linux-android"));
}};

UTest a_planned_row_is_the_answer_only_when_alone{"TargetSelect/APlannedRowIsTheAnswerOnlyWhenAlone", [] {
    auto s = on("linux-x86_64", {"riscv64-linux", "riscv64", "linux", "gnu"});
    expect(fatal(s.row != nullptr));
    expect(eq(s.identity(), "riscv64-linux-musl"));
    expect(eq(s.row->tier, "planned"));
    // x86_64-macos is planned; the musl identity beside it is not.
    expect(eq(on("macos-x86_64", {"x86_64-macos", "x86_64", "macos", ""}).identity(), "x86_64-macos-musl"));
}};
