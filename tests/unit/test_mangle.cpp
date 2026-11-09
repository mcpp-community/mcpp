
import std;
import mcpplibs.mtest;
import mcpp.pm.mangle;

using namespace mcpplibs::mtest;

using namespace mcpp::pm;

UTest name_format{"Mangle/NameFormat", [] {
    expect(eq(mangle_name("json", "1.2.3"), "json__v1_2_3__mcpp"));
    expect(eq(mangle_name("mcpplibs.cmdline","2.0"), "mcpplibs.cmdline__v2_0__mcpp"));
    expect(eq(mangle_name("a", "0"), "a__v0__mcpp"));
}};

UTest discovers_authored_module_roots_without_package_name_assumptions{"Mangle/DiscoversAuthoredModuleRootsWithoutPackageNameAssumptions", [] {
    auto roots = declared_module_roots(
        "module;\n"
        "export module mcpplibs.cmdline;\n"
        "export module mcpplibs.cmdline:options;\n"
        "module mcpplibs.cmdline:impl;\n"
        "import unrelated;\n"
        "export module vendor.extra;\n");
    expect(eq(roots, (std::vector<std::string>{
        "mcpplibs.cmdline", "vendor.extra"})));
}};

UTest rewrite_empty{"Mangle/RewriteEmpty", [] {
    std::map<std::string, std::string> table;
    expect(eq(rewrite_module_decls("", table), ""));
    expect(eq(rewrite_module_decls("export module foo;\n", table), "export module foo;\n"))
        << "empty rename table → no change";
}};

UTest rewrite_module_decl{"Mangle/RewriteModuleDecl", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    expect(eq(rewrite_module_decls("export module json;\n", table), "export module json__v2_0_0__mcpp;\n"));
    expect(eq(rewrite_module_decls("module json;\n", table), "module json__v2_0_0__mcpp;\n"));
}};

UTest rewrite_partition_decl{"Mangle/RewritePartitionDecl", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    expect(eq(rewrite_module_decls("export module json:utils;\n", table), "export module json__v2_0_0__mcpp:utils;\n"));
    expect(eq(rewrite_module_decls("module json:impl;\n", table), "module json__v2_0_0__mcpp:impl;\n"));
}};

UTest rewrite_imports{"Mangle/RewriteImports", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    expect(eq(rewrite_module_decls("import json;\n", table), "import json__v2_0_0__mcpp;\n"));
    expect(eq(rewrite_module_decls("export import json;\n", table), "export import json__v2_0_0__mcpp;\n"));
    expect(eq(rewrite_module_decls("import json:utils;\n", table), "import json__v2_0_0__mcpp:utils;\n"));
}};

UTest keep_bare_partition_import{"Mangle/KeepBarePartitionImport", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    // `import :P;` refers to the enclosing module's partition — not a
    // named module; must NOT be rewritten.
    expect(eq(rewrite_module_decls("import :utils;\n", table), "import :utils;\n"));
    expect(eq(rewrite_module_decls("module ;\n", table), "module ;\n"))
        << "global module fragment opener has no name";
}};

UTest keep_non_matching{"Mangle/KeepNonMatching", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    // Other modules in the same file shouldn't be touched.
    expect(eq(rewrite_module_decls("import std;\n",  table), "import std;\n"));
    expect(eq(rewrite_module_decls("import other;\n", table), "import other;\n"));
    expect(eq(rewrite_module_decls("export module yaml;\n", table), "export module yaml;\n"));
}};

UTest multiple_lines{"Mangle/MultipleLines", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    std::string in =
        "// header comment\n"
        "module;\n"
        "#include <iostream>\n"
        "export module json;\n"
        "import std;\n"
        "import json:utils;\n"
        "export int answer() { return 42; }\n";
    std::string expected =
        "// header comment\n"
        "module;\n"
        "#include <iostream>\n"
        "export module json__v2_0_0__mcpp;\n"
        "import std;\n"
        "import json__v2_0_0__mcpp:utils;\n"
        "export int answer() { return 42; }\n";
    expect(eq(rewrite_module_decls(in, table), expected));
}};

UTest leading_whitespace{"Mangle/LeadingWhitespace", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    expect(eq(rewrite_module_decls("    export module json;\n", table), "    export module json__v2_0_0__mcpp;\n"));
    expect(eq(rewrite_module_decls("\timport json;\n", table), "\timport json__v2_0_0__mcpp;\n"));
}};

UTest no_trailing_newline{"Mangle/NoTrailingNewline", [] {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    expect(eq(rewrite_module_decls("export module json;", table), "export module json__v2_0_0__mcpp;"));
}};

UTest dotted_names{"Mangle/DottedNames", [] {
    std::map<std::string, std::string> table {
        {"mcpplibs.cmdline", "mcpplibs.cmdline__v2_0_0__mcpp"},
    };
    expect(eq(rewrite_module_decls("export module mcpplibs.cmdline;\n", table), "export module mcpplibs.cmdline__v2_0_0__mcpp;\n"));
    expect(eq(rewrite_module_decls("import mcpplibs.cmdline;\n", table), "import mcpplibs.cmdline__v2_0_0__mcpp;\n"));
}};
