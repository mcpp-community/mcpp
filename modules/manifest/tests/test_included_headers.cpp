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

// mcpp#963: the functions read the code text, so a declaration spelled in a raw string, a comment
// or a literal is none.
UTest a_declaration_in_a_raw_string_declares_nothing{"ModuleNames/ADeclarationInARawStringDeclaresNothing", [] {
    const std::string_view text =
        "export module real;\n"
        "const char* usage = R\"(\n"
        "export module quoted;\n"
        "import quoted.dep;\n"
        "#include <quoted.h>\n"
        ")\";\n"
        "/*\n"
        "module commented;\n"
        "*/\n"
        "import real.dep;\n"
        "#include <real.h>\n";
    expect(eq(mcpp::pm::declared_module_roots(text), std::vector<std::string>{"real"}));
    expect(eq(mcpp::pm::imported_module_names(text), std::vector<std::string>{"real.dep"}));
    expect(eq(mcpp::pm::included_headers(text), std::vector<std::string>{"<real.h>"}));
}};

UTest a_rename_leaves_what_is_not_code{"ModuleNames/ARenameLeavesWhatIsNotCode", [] {
    const std::string_view text =
        "/* the module */ export module lib;\n"
        "const char* s = R\"(\n"
        "import lib;\n"
        ")\";\n"
        "// import lib;\n"
        "import lib;\n";
    const auto out = mcpp::pm::rewrite_module_decls(text, {{"lib", "lib__v1__mcpp"}});
    expect(eq(out, std::string(
        "/* the module */ export module lib__v1__mcpp;\n"
        "const char* s = R\"(\n"
        "import lib;\n"
        ")\";\n"
        "// import lib;\n"
        "import lib__v1__mcpp;\n")));
}};
