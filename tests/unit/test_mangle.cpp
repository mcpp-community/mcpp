#include <gtest/gtest.h>

import std;
import mcpp.pm.mangle;

using namespace mcpp::pm;

TEST(Mangle, NameFormat) {
    EXPECT_EQ(mangle_name("json", "1.2.3"),         "json__v1_2_3__mcpp");
    EXPECT_EQ(mangle_name("mcpplibs.cmdline","2.0"),"mcpplibs.cmdline__v2_0__mcpp");
    EXPECT_EQ(mangle_name("a", "0"),                "a__v0__mcpp");
}

TEST(Mangle, RewriteEmpty) {
    std::map<std::string, std::string> table;
    EXPECT_EQ(rewrite_module_decls("", table), "");
    EXPECT_EQ(rewrite_module_decls("export module foo;\n", table),
                                   "export module foo;\n")
        << "empty rename table → no change";
}

TEST(Mangle, RewriteModuleDecl) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    EXPECT_EQ(rewrite_module_decls("export module json;\n", table),
              "export module json__v2_0_0__mcpp;\n");
    EXPECT_EQ(rewrite_module_decls("module json;\n", table),
              "module json__v2_0_0__mcpp;\n");
}

TEST(Mangle, RewritePartitionDecl) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    EXPECT_EQ(rewrite_module_decls("export module json:utils;\n", table),
              "export module json__v2_0_0__mcpp:utils;\n");
    EXPECT_EQ(rewrite_module_decls("module json:impl;\n", table),
              "module json__v2_0_0__mcpp:impl;\n");
}

TEST(Mangle, RewriteImports) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    EXPECT_EQ(rewrite_module_decls("import json;\n", table),
              "import json__v2_0_0__mcpp;\n");
    EXPECT_EQ(rewrite_module_decls("export import json;\n", table),
              "export import json__v2_0_0__mcpp;\n");
    EXPECT_EQ(rewrite_module_decls("import json:utils;\n", table),
              "import json__v2_0_0__mcpp:utils;\n");
}

TEST(Mangle, KeepBarePartitionImport) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    // `import :P;` refers to the enclosing module's partition — not a
    // named module; must NOT be rewritten.
    EXPECT_EQ(rewrite_module_decls("import :utils;\n", table),
              "import :utils;\n");
    EXPECT_EQ(rewrite_module_decls("module ;\n", table),
              "module ;\n")
        << "global module fragment opener has no name";
}

TEST(Mangle, KeepNonMatching) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    // Other modules in the same file shouldn't be touched.
    EXPECT_EQ(rewrite_module_decls("import std;\n",  table), "import std;\n");
    EXPECT_EQ(rewrite_module_decls("import other;\n", table), "import other;\n");
    EXPECT_EQ(rewrite_module_decls("export module yaml;\n", table),
                                   "export module yaml;\n");
}

TEST(Mangle, MultipleLines) {
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
    EXPECT_EQ(rewrite_module_decls(in, table), expected);
}

TEST(Mangle, LeadingWhitespace) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    EXPECT_EQ(rewrite_module_decls("    export module json;\n", table),
              "    export module json__v2_0_0__mcpp;\n");
    EXPECT_EQ(rewrite_module_decls("\timport json;\n", table),
              "\timport json__v2_0_0__mcpp;\n");
}

TEST(Mangle, NoTrailingNewline) {
    std::map<std::string, std::string> table {
        {"json", "json__v2_0_0__mcpp"},
    };
    EXPECT_EQ(rewrite_module_decls("export module json;", table),
              "export module json__v2_0_0__mcpp;");
}

TEST(Mangle, DottedNames) {
    std::map<std::string, std::string> table {
        {"mcpplibs.cmdline", "mcpplibs.cmdline__v2_0_0__mcpp"},
    };
    EXPECT_EQ(rewrite_module_decls("export module mcpplibs.cmdline;\n", table),
              "export module mcpplibs.cmdline__v2_0_0__mcpp;\n");
    EXPECT_EQ(rewrite_module_decls("import mcpplibs.cmdline;\n", table),
              "import mcpplibs.cmdline__v2_0_0__mcpp;\n");
}
