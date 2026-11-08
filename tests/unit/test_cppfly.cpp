// tests/unit/test_cppfly.cpp — mcpp.toolchain.cppfly: the `standard = "c++fly"`
// capability tables (latest std level + experimental gates per compiler) and
// the c++latest/c++fly-aware std-flag spelling.
// Table facts pinned by on-machine probes (gcc 16.1.0, 2026-07-14) — see
// .agents/docs/2026-07-14-std-features-experimental-gate-design.md §2/§11-Q3.

import std;
import mcpplibs.mtest;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

namespace {
Toolchain tc_of(CompilerId id, std::string ver, std::string stdlib = "libstdc++") {
    Toolchain tc;
    tc.compiler = id;
    tc.version = std::move(ver);
    tc.stdlibId = std::move(stdlib);
    return tc;
}
bool has_flag(const std::vector<std::string>& v, std::string_view f) {
    return std::find(v.begin(), v.end(), f) != v.end();
}
const cppfly::FeatureState* feature(const cppfly::Resolution& r, std::string_view n) {
    for (auto& f : r.features)
        if (f.name == n) return &f;
    return nullptr;
}
} // namespace

UTest cpp_fly_compiler_major{"CppFly/CompilerMajor", [] {
    expect(eq(cppfly::compiler_major(tc_of(CompilerId::GCC, "16.1.0")), 16));
    expect(eq(cppfly::compiler_major(tc_of(CompilerId::Clang, "22.1.8")), 22));
    expect(eq(cppfly::compiler_major(tc_of(CompilerId::GCC, "")), 0));
}};

UTest cpp_fly_latest_std_canonical{"CppFly/LatestStdCanonical", [] {
    int lvl = 0;
    expect(eq(cppfly::latest_std_canonical(tc_of(CompilerId::GCC, "16.1.0"), &lvl), "c++26"));
    expect(eq(lvl, 26));
    expect(eq(cppfly::latest_std_canonical(tc_of(CompilerId::GCC, "13.2.0")), "c++23"));
    expect(eq(cppfly::latest_std_canonical(tc_of(CompilerId::Clang, "22.1.8")), "c++2c"));
    expect(eq(cppfly::latest_std_canonical(tc_of(CompilerId::MSVC, "19.44")), "c++26"));
}};

UTest resolve_gcc16{"CppFly/ResolveGcc16", [] {
    auto r = cppfly::resolve(tc_of(CompilerId::GCC, "16.1.0"));
    expect(eq(r.stdCanonical, "c++26"));
    expect(has_flag(r.flags, "-freflection"));
    auto* refl = feature(r, "reflection");
    expect(fatal(ne(refl, nullptr)));
    expect(bool(refl->enabled));
    expect(eq(refl->paper, "P2996"));
    // Contracts: enabled by -std=c++26 alone on GCC 16 (probe-pinned) — no
    // extra flag, but reported enabled for the summary.
    auto* con = feature(r, "contracts");
    expect(fatal(ne(con, nullptr)));
    expect(bool(con->enabled));
    expect(con->flags.empty());
    // libstdc++ ships experimental bits ungated.
    expect(!has_flag(r.flags, "-fexperimental-library"));
}};

UTest resolve_gcc15_skips_by_version{"CppFly/ResolveGcc15SkipsByVersion", [] {
    auto r = cppfly::resolve(tc_of(CompilerId::GCC, "15.1.0"));
    expect(r.flags.empty());
    auto* refl = feature(r, "reflection");
    expect(fatal(ne(refl, nullptr)));
    expect(!refl->enabled);
    expect(ne(refl->reason.find("16"), std::string::npos));  // names the version floor
}};

UTest resolve_clang_libcxx{"CppFly/ResolveClangLibcxx", [] {
    auto r = cppfly::resolve(tc_of(CompilerId::Clang, "22.1.8", "libc++"));
    expect(eq(r.stdCanonical, "c++2c"));
    expect(has_flag(r.flags, "-fexperimental-library"));
    auto* refl = feature(r, "reflection");
    expect(fatal(ne(refl, nullptr)));
    expect(!refl->enabled);
    auto* lib = feature(r, "experimental-library");
    expect(fatal(ne(lib, nullptr)));
    expect(bool(lib->enabled));
}};

UTest resolve_msvc_all_skipped{"CppFly/ResolveMsvcAllSkipped", [] {
    auto r = cppfly::resolve(tc_of(CompilerId::MSVC, "19.44", "msvc-stl"));
    expect(r.flags.empty());
    for (auto& f : r.features) expect(!f.enabled);
}};

UTest std_flag_resolves_latest_and_fly{"CppFly/StdFlagResolvesLatestAndFly", [] {
    auto gcc16 = tc_of(CompilerId::GCC, "16.1.0");
    expect(eq(cppfly::std_flag(gcc16, "c++fly", 1000), "-std=c++26"));
    // standard = "c++latest" used to reach the GNU driver as the invalid
    // spelling -std=c++latest (design §11-Q5) — now resolves to the family
    // latest.
    expect(eq(cppfly::std_flag(gcc16, "c++latest", 999), "-std=c++26"));
    expect(eq(cppfly::std_flag(gcc16, "c++23", 23), "-std=c++23"));
    expect(eq(cppfly::std_flag(gcc16, "gnu++23", 23), "-std=gnu++23"));
    expect(eq(cppfly::std_flag(tc_of(CompilerId::Clang, "22.1.8"), "c++fly", 1000), "-std=c++2c"));
    expect(eq(cppfly::std_flag(tc_of(CompilerId::MSVC, "19.44"), "c++fly", 1000), "/std:c++latest"));
    expect(eq(cppfly::std_flag(tc_of(CompilerId::MSVC, "19.44"), "c++latest", 999), "/std:c++latest"));
    // A plain level is passed through untouched — c++20 must NOT be lifted to
    // the family latest by the >= 999 branch.
    expect(eq(cppfly::std_flag(gcc16, "c++20", 20), "-std=c++20"));
    expect(eq(cppfly::std_flag(gcc16, "gnu++20", 20), "-std=gnu++20"));
    expect(eq(cppfly::std_flag(tc_of(CompilerId::Clang, "22.1.8"), "c++20", 20), "-std=c++20"));
    expect(eq(cppfly::std_flag(tc_of(CompilerId::MSVC, "19.44"), "c++20", 20), "/std:c++20"));
}};

UTest effective_dialect_flags_dedup{"CppFly/EffectiveDialectFlagsDedup", [] {
    auto gcc16 = tc_of(CompilerId::GCC, "16.1.0");
    // User hand-wrote -freflection too — the fly gate must not duplicate it.
    auto out = cppfly::effective_dialect_flags(gcc16, true, {"-freflection", "-fchar8_t"});
    expect(eq(std::count(out.begin(), out.end(), std::string("-freflection")), 1));
    expect(has_flag(out, "-fchar8_t"));
    // Not experimental → manifest flags pass through untouched.
    auto off = cppfly::effective_dialect_flags(gcc16, false, {"-fchar8_t"});
    expect(eq(off, (std::vector<std::string>{"-fchar8_t"})));
}};
