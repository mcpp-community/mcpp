
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.toolchain.triple;
import mcpp.build.prepare_inputs;

using namespace mcpplibs::mtest;

// A `[target.<triple>]` row is one row whichever spelling names it. Every
// reader of a row asks `find_target_entry`; before 2026.10.5.3 the readers of
// `runner` and `min_api_level` compared the key exactly, so a section written
// `[target.x86-windows-msvc]` applied to `toolchain` and `sysroot` and not to
// `runner`.

namespace tr = mcpp::toolchain::triple;

UTest every_spelling_of_a_triple_finds_its_row{"TargetRowSpelling/EverySpellingOfATripleFindsItsRow", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"

[target.x86-windows-msvc]
runner = ["wine"]

[target.x86_64-w64-mingw32]
runner = ["wine64"]

[target.i386-windows-msvc]
runner = ["i386"]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto runner_of = [&](std::string_view triple) -> std::string {
        auto t = tr::parse(triple);
        if (!t) return "<unparsed>";
        auto* row = mcpp::build::find_target_entry(*m, *t);
        return row && !row->runner.empty() ? row->runner.front() : "<none>";
    };
    expect(eq(runner_of("i686-windows-msvc"), "wine"));
    expect(eq(runner_of("i686-pc-windows-msvc"), "wine"));
    expect(eq(runner_of("x86-windows-msvc"), "wine"));
    expect(eq(runner_of("x86_64-windows-gnu"), "wine64"));
    // A different baseline CPU is a different row.
    expect(eq(runner_of("i386-windows-msvc"), "i386"));
    expect(eq(runner_of("i586-windows-msvc"), "<none>"));
}};
