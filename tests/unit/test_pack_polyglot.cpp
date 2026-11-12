
import std;
import mcpplibs.mtest;
import mcpp.pack.polyglot;

using namespace mcpplibs::mtest;

// A polyglot file's layout (SPEC-014 §5.3). The PE half is asserted end to
// end by e2e 992, against the carriers a pack makes; what is asserted here is
// the arithmetic a wrong answer to would still run on the machine that wrote
// it: every program lies at the offset its line names, byte for byte, and the
// script's head names where the script is.

namespace pg = mcpp::pack::polyglot;

namespace {

pg::Program program(std::string system, std::string arch, std::string bytes) {
    return { std::move(system), std::move(arch), std::move(bytes), std::string(64, 'a') };
}

std::uint64_t number_after(std::string_view text, std::string_view key) {
    const auto at = text.find(key);
    if (at == std::string_view::npos) return ~std::uint64_t{0};
    std::uint64_t v = 0;
    for (auto i = at + key.size(); i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
        v = v * 10 + static_cast<std::uint64_t>(text[i] - '0');
    return v;
}

}  // namespace

UTest without_windows_the_file_is_a_script_and_each_program_lies_where_its_line_says{"PackPolyglot/WithoutWindowsTheFileIsAScriptAndEachProgramLiesWhereItsLineSays", [] {
    const std::string linux64(1000, 'L'), mac(777, 'M');
    auto file = pg::assemble("app", { program("linux", "x86_64", linux64), program("macos", "aarch64", mac) });
    if (auto&& failed = expect(fatal(file.has_value())); !failed) failed << file.error();
    expect(file->starts_with("#!/bin/sh\no=$0;"));
    expect(pg::is_polyglot(*file));

    // The head names the script: `tail -c +<at+1>` and `head -c <length>`.
    const auto at = number_after(*file, "tail -c +") - 1;
    const auto length = number_after(*file, "|head -c ");
    expect(fatal(eq(at + length, file->size())));
    const std::string_view script = std::string_view(*file).substr(at);
    expect(script.starts_with("# mcpp polyglot"));
    expect(ne(script.find("n='app'"), std::string_view::npos));
    expect(eq(script.find("MINGW*"), std::string_view::npos)) << "no Windows program, no MSYS line";

    for (auto [key, bytes] : { std::pair{std::string_view("k=linux-x86_64 a="), std::string_view(linux64)},
                               std::pair{std::string_view("k=macos-aarch64 a="), std::string_view(mac)} }) {
        const auto line = script.find(key);
        expect(fatal(ne(line, std::string_view::npos))) << key;
        const auto off = number_after(script.substr(line), key);
        const auto len = number_after(script.substr(line), " b=");
        expect(eq(len, bytes.size())) << key;
        expect(eq(std::string_view(*file).substr(off, len), bytes)) << key;
    }
    // `uname` spells an architecture two ways on some systems.
    expect(ne(script.find("Linux-x86_64|Linux-amd64)"), std::string_view::npos));
    expect(ne(script.find("Darwin-aarch64|Darwin-arm64)"), std::string_view::npos));
}};

// The word a system is selected by is the system table's `uname` (SPEC-016
// §2.3): a system the table states joins the file with no word of the code.
UTest a_system_of_the_table_is_selected_by_its_uname{"PackPolyglot/ASystemOfTheTableIsSelectedByItsUname", [] {
    auto file = pg::assemble("app", { program("linux", "x86_64", "LINUX"),
                                      program("freebsd", "x86_64", "FREEBSD") });
    expect(fatal(file.has_value()));
    expect(ne(file->find("FreeBSD-x86_64|FreeBSD-amd64)"), std::string::npos));
    expect(ne(file->find("Linux-x86_64|Linux-amd64)"), std::string::npos));
}};

UTest refuses_what_it_cannot_lay_out{"PackPolyglot/RefusesWhatItCannotLayOut", [] {
    expect(!(pg::assemble("app", {}).has_value()));
    expect(!(pg::assemble("a'b", { program("linux", "x86_64", "x") }).has_value()))
        << "the name is quoted in the script";
    // A system no table states has no `uname` word to be selected by.
    auto unstated = pg::assemble("app", { program("plan9", "x86_64", "x") });
    expect(fatal(!unstated.has_value()));
    expect(ne(unstated.error().find("[[platform.system]]"), std::string::npos)) << unstated.error();
    auto notPe = pg::assemble("app", { program("windows", "x86_64", std::string(256, 'x')) });
    expect(fatal(!notPe.has_value()));
    if (auto&& failed = expect(ne(notPe.error().find("no PE"), std::string::npos)); !failed) failed << notPe.error();
    auto twoWindows = pg::assemble("app", { program("windows", "x86_64", "MZ"), program("windows", "aarch64", "MZ") });
    expect(fatal(!twoWindows.has_value()));
    if (auto&& failed = expect(ne(twoWindows.error().find("one Windows program"), std::string::npos)); !failed) failed << twoWindows.error();
}};

UTest recognises_its_own_heads_only{"PackPolyglot/RecognisesItsOwnHeadsOnly", [] {
    expect(pg::is_polyglot("MZqFpD='\n   "));
    expect(!pg::is_polyglot("MZ\x90\x00"));
    expect(!pg::is_polyglot("#!/bin/sh\necho hi\n"));
    expect(!pg::is_polyglot(""));
}};
