// mcpp.build.result: the diagnostics a failed step's output yields
// (design 2026-10-26 §5.1), in every language cl.exe speaks (mcpp#970,
// design 2026-11-19 §20 T4). The Chinese lines are the bytes cl.exe 19.44
// wrote with the zh-cn resources on windows-2022 (probe mcpp#971), decoded.

import std;
import mcpplibs.mtest;
import mcpp.build.result;

using namespace mcpplibs::mtest;

namespace r = mcpp::build::result;

UTest msvc_errors_and_notes_in_english{"BuildResult/MsvcErrorsAndNotesInEnglish", [] {
    const auto d = r::parse_compiler_diagnostics(
        "k.cpp\r\n"
        "k.cpp(2): error C2668: 'h': ambiguous call to overloaded function\r\n"
        "k.cpp(1): note: could be 'void h(long)'\r\n"
        "k.cpp(2,7): warning C4189: 'unused': local variable is initialized but not referenced\r\n");
    expect(fatal(eq(d.size(), std::size_t{3})));
    expect(eq(d[0].severity, std::string("error")) && eq(d[0].path, std::string("k.cpp")) && eq(d[0].line, 2));
    expect(eq(d[0].message, std::string("'h': ambiguous call to overloaded function")));
    // A note carries no code: its location is the file's, not `k.cpp(1)`.
    expect(eq(d[1].severity, std::string("note")) && eq(d[1].path, std::string("k.cpp")) && eq(d[1].line, 1));
    expect(eq(d[1].message, std::string("could be 'void h(long)'")));
    expect(eq(d[2].severity, std::string("warning")) && eq(d[2].line, 2) && eq(d[2].column, 7));
}};

UTest msvc_errors_and_notes_in_chinese{"BuildResult/MsvcErrorsAndNotesInChinese", [] {
    const auto d = r::parse_compiler_diagnostics(
        "b.cpp(2): error C2065: \xE2\x80\x9C\xE6\x9C\xAA\xE5\xAE\x9A\xE4\xB9\x89_\xC3\xA9\xE2\x80\x9D: "
        "\xE6\x9C\xAA\xE5\xA3\xB0\xE6\x98\x8E\xE7\x9A\x84\xE6\xA0\x87\xE8\xAF\x86\xE7\xAC\xA6\n"
        "k.cpp(1): note: \xE5\x8F\xAF\xE8\x83\xBD\xE6\x98\xAF\xE2\x80\x9Cvoid h(long)\xE2\x80\x9D\n"
        "c.cpp(1): fatal error C1083: \xE6\x97\xA0\xE6\xB3\x95\xE6\x89\x93\xE5\xBC\x80\xE5\x8C\x85\xE6\x8B\xAC"
        "\xE6\x96\x87\xE4\xBB\xB6: \xE2\x80\x9C\xE7\xBC\xBA\xE5\xA4\xB1.h\xE2\x80\x9D: No such file or directory\n");
    expect(fatal(eq(d.size(), std::size_t{3})));
    expect(eq(d[0].severity, std::string("error")) && eq(d[0].path, std::string("b.cpp")) && eq(d[0].line, 2));
    expect(d[0].message.starts_with("\xE2\x80\x9C\xE6\x9C\xAA")) << d[0].message;
    expect(eq(d[1].severity, std::string("note")) && eq(d[1].path, std::string("k.cpp")) && eq(d[1].line, 1));
    expect(eq(d[2].severity, std::string("error")) && eq(d[2].path, std::string("c.cpp")));
}};

UTest gnu_diagnostics_are_read_as_before{"BuildResult/GnuDiagnosticsAreReadAsBefore", [] {
    const auto d = r::parse_compiler_diagnostics(
        "src/a.cpp:3:5: error: 'y' was not declared in this scope\n"
        "src/a.cpp:1:1: note: declared here\n"
        "cc1plus: error: unrecognized command-line option '-fno'\n");
    expect(fatal(eq(d.size(), std::size_t{3})));
    expect(eq(d[0].path, std::string("src/a.cpp")) && eq(d[0].line, 3) && eq(d[0].column, 5));
    expect(eq(d[1].severity, std::string("note")));
    expect(eq(d[2].path, std::string("cc1plus")) && eq(d[2].line, 0));
}};
