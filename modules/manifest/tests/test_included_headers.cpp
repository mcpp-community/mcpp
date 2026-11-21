import std;
import mcpplibs.mtest;
import mcpp.pm.mangle;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the headers a build program's text includes, read by the matcher that reads its
// imports (design 2026-11-21, build programs' inputs §3.3). That a project is told of them once is
// stated by the e2e suite.

UTest included_headers_keep_their_delimiters{"IncludedHeaders/KeepTheirDelimiters", [] {
    const auto h = mcpp::pm::included_headers(
        "#include <cstdio>\n"
        "  #  include \"support/emit.hpp\"\n"
        "#include <cstdio>\n"
        "import std;\n");
    expect(fatal(eq(h.size(), 2u)));
    expect(eq(h[0], std::string("<cstdio>")));
    expect(eq(h[1], std::string("\"support/emit.hpp\"")));
}};

UTest a_commented_or_computed_include_names_nothing{"IncludedHeaders/CommentedOrComputedNamesNothing", [] {
    expect(mcpp::pm::included_headers("// #include <cstdio>\n#include HEADER\n#include <>\n"
                                      "#includes <x>\n#define include\n").empty());
}};
