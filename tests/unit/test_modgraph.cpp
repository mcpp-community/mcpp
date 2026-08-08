#include <gtest/gtest.h>

import std;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.manifest;

using namespace mcpp::modgraph;

namespace {

std::filesystem::path make_tempdir(std::string_view prefix) {
    auto tmp = std::filesystem::temp_directory_path();
    auto dir = tmp / std::format("{}-{}", prefix, std::random_device{}() );
    std::filesystem::create_directories(dir);
    return dir;
}

void write(const std::filesystem::path& p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

} // namespace

TEST(Scanner, ProvidesAndRequires) {
    auto dir = make_tempdir("mcpp-scanner");
    // NOTE: avoid raw string literal for module source — clang-scan-deps
    // on Windows may false-positive on `import bar;` inside R"(...)".
    write(dir / "src" / "foo.cppm",
          "export module foo;\n"
          "import std;\n"
          "import bar;\n"
          "export int answer();\n");

    auto u = scan_file(dir / "src" / "foo.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_TRUE(u->provides.has_value());
    EXPECT_EQ(u->provides->logicalName, "foo");
    ASSERT_EQ(u->requires_.size(), 2u);
    EXPECT_EQ(u->requires_[0].logicalName, "std");
    EXPECT_EQ(u->requires_[1].logicalName, "bar");

    std::filesystem::remove_all(dir);
}

// Regression: `import` lines that live INSIDE a multi-line raw-string literal
// (e.g. a `mcpp new --template gui` skeleton embedded as R"GUI( ... )GUI") must
// not be detected as real module imports. Before the fix this produced a
// spurious "module 'imgui.core' imported but not provided" warning.
TEST(Scanner, IgnoresImportsInsideRawStringLiteral) {
    auto dir = make_tempdir("mcpp-scanner-raw");
    write(dir / "src" / "gen.cppm",
          "export module gen;\n"
          "import std;\n"
          "const char* tmpl = R\"GUI(\n"
          "import imgui.core;\n"
          "import imgui.app;\n"
          "int main() { return 0; }\n"
          ")GUI\";\n"
          "import bar;\n"               // a real import AFTER the raw string
          "export void f();\n");

    auto u = scan_file(dir / "src" / "gen.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_TRUE(u->provides.has_value());
    EXPECT_EQ(u->provides->logicalName, "gen");
    // Only the two genuine top-level imports — NOT imgui.core / imgui.app.
    ASSERT_EQ(u->requires_.size(), 2u);
    EXPECT_EQ(u->requires_[0].logicalName, "std");
    EXPECT_EQ(u->requires_[1].logicalName, "bar");
    for (auto& r : u->requires_) {
        EXPECT_NE(r.logicalName, "imgui.core");
        EXPECT_NE(r.logicalName, "imgui.app");
    }

    std::filesystem::remove_all(dir);
}

// A single-line raw string with an embedded import-looking body stays code.
TEST(Scanner, IgnoresImportInsideSingleLineRawString) {
    auto dir = make_tempdir("mcpp-scanner-raw1");
    write(dir / "src" / "one.cppm",
          "export module one;\n"
          "const char* s = R\"(import nope;)\";\n"
          "import real;\n");

    auto u = scan_file(dir / "src" / "one.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_EQ(u->requires_.size(), 1u);
    EXPECT_EQ(u->requires_[0].logicalName, "real");

    std::filesystem::remove_all(dir);
}

// Assembly units skip the module text scan entirely (like .c): an .asm/.S
// file legally contains no `import`/`module` declarations, and asm comment
// syntax would misparse.
TEST(Scanner, AssemblySourcesSkipModuleScan) {
    auto dir = make_tempdir("mcpp-scanner-asm");
    write(dir / "src" / "simd.asm",
          "; import std -- a comment, not a declaration\n"
          "section .text\n"
          "global sum2\n"
          "sum2:\n  lea rax, [rdi+rsi]\n  ret\n");
    write(dir / "src" / "copy.S",
          "#include \"defs.h\"\n"
          ".text\n.globl asm_copy\nasm_copy:\n  ret\n");

    for (auto name : { "simd.asm", "copy.S" }) {
        auto u = scan_file(dir / "src" / name, "pkg");
        ASSERT_TRUE(u.has_value()) << u.error().format();
        EXPECT_FALSE(u->provides.has_value()) << name;
        EXPECT_TRUE(u->requires_.empty()) << name;
    }

    std::filesystem::remove_all(dir);
}

// G8a: source globs follow directory symlinks (vendored trees are often
// symlink farms) without looping on cycles.
TEST(Scanner, ExpandGlobFollowsDirectorySymlinks) {
    auto dir = make_tempdir("mcpp-scanner-symlink");
    auto real = dir / "vendor-real";
    write(real / "impl.cpp", "int f() { return 1; }\n");
    std::error_code ec;
    std::filesystem::create_directories(dir / "src");
    std::filesystem::create_directory_symlink(real, dir / "src" / "vendor", ec);
    if (ec) GTEST_SKIP() << "symlinks unsupported here: " << ec.message();
    // A cycle: the linked tree points back at its parent.
    std::filesystem::create_directory_symlink(dir, real / "loop", ec);

    auto files = expand_glob(dir, "src/**/*.cpp");

    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].filename(), "impl.cpp");

    std::filesystem::remove_all(dir);
}

// mcpp#225 review finding: glob_literal_prefix's contract (the piece that
// actually decides WHERE expand_glob/expand_dir_glob start walking) is the
// thing that must be locked down deterministically. Calling the pure helper
// directly — rather than inferring its behavior from a filesystem walk whose
// entry order is unspecified by the standard — makes this test's pass/fail
// independent of directory-enumeration order on any filesystem.
TEST(Scanner, GlobLiteralPrefixDerivation) {
    EXPECT_EQ(glob_literal_prefix("src/**/*.cppm"), "src");
    // Wildcard already in the first segment: no literal directory to bound to.
    EXPECT_EQ(glob_literal_prefix("**/*.c"), "");
    // No wildcard at all: the full parent directory path is the prefix.
    EXPECT_EQ(glob_literal_prefix("a/b/c.cpp").generic_string(), "a/b");
    // Truncate back to the last COMPLETE '/' before the first wildcard char —
    // "x*.cpp" is a partial segment, not a real directory named "x".
    EXPECT_EQ(glob_literal_prefix("src/x*.cpp"), "src");
    // '{' is treated as a segment-boundary wildcard char (brace-expansion
    // globs are desugared before reaching this helper — see its comment in
    // scanner.cppm), so the prefix truncates at the last '/' before it.
    EXPECT_EQ(glob_literal_prefix("a/{x,y}/z"), "a");
}

// MSVC's std::filesystem::path preserves the separators of the string it was
// constructed from, so a raw `a/b` prefix stays generic and `root / p` turns
// into a MIXED `root\a/b` — which used to leak into compile_commands.json
// (`file` / `-c` for every source under a multi-segment glob) and break CLion.
// glob_literal_prefix must return NATIVE separators so the walk and everything
// downstream is native too. (The generic spelling is already locked down by
// Scanner.GlobLiteralPrefixDerivation above.)
TEST(Scanner, GlobLiteralPrefixUsesNativeSeparators) {
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        EXPECT_EQ(glob_literal_prefix("a/b/c.cpp").string(), "a\\b");
        EXPECT_EQ(glob_literal_prefix("a/b/c.cpp").string().find('/'),
                  std::string::npos);
    }
}

// The exported converter itself — both spelling directions.
TEST(Glob, NativePathFromGeneric) {
    auto p = mcpp::modgraph::native_path_from_generic("a/b/c");
    EXPECT_EQ(p.generic_string(), "a/b/c");
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        EXPECT_EQ(p.string(), "a\\b\\c");
        // Already-native input is untouched.
        EXPECT_EQ(mcpp::modgraph::native_path_from_generic("C:\\x\\y").string(),
                  "C:\\x\\y");
    }
}

// The end-to-end shape of the reported bug: a source under a multi-segment
// glob (`generated/modules/**/*.cppm`) must come out of expand_glob with
// NATIVE separators on Windows — the mixed `root\generated/modules\a.cppm`
// was what compile_commands.json's `file` field showed before the fix.
TEST(Scanner, ExpandGlobMultiSegmentPrefixUsesNativeSeparators) {
    auto dir = make_tempdir("mcpp-scanner-multi");
    write(dir / "generated" / "modules" / "a.cppm", "export module a;\n");

    auto files = expand_glob(dir, "generated/modules/**/*.cppm");

    ASSERT_EQ(files.size(), 1u);
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        EXPECT_EQ(files[0].string().find('/'), std::string::npos) << files[0];
    }
    EXPECT_EQ(files[0].generic_string(),
              (dir / "generated" / "modules" / "a.cppm").generic_string());

    std::filesystem::remove_all(dir);
}

// Same contract for the INCLUDE-DIR channel (expand_dir_glob): a multi-segment
// `third_party/inc` entry must yield a native path or the CDB's -I carries the
// mixed form.
TEST(Scanner, ExpandDirGlobMultiSegmentUsesNativeSeparators) {
    auto dir = make_tempdir("mcpp-scanner-dirglob");
    std::filesystem::create_directories(dir / "third_party" / "inc");

    auto dirs = expand_dir_glob(dir, "third_party/inc");

    ASSERT_EQ(dirs.size(), 1u);
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        EXPECT_EQ(dirs[0].string().find('/'), std::string::npos) << dirs[0];
    }
    EXPECT_EQ(dirs[0].generic_string(),
              (dir / "third_party" / "inc").generic_string());

    std::filesystem::remove_all(dir);
}

// mcpp#225: expand_glob must bound its walk to the glob's literal directory
// prefix ("src" for "src/**/*.cppm") instead of always walking the whole
// root and lexically filtering afterward. This is the FUNCTIONAL half of the
// regression guard: given a normal (non-adversarial) tree with files outside
// "src", a bounded walk returns exactly the "src" matches. The core "walk
// starts AT the literal prefix, not root" behavior is now locked down
// deterministically by Scanner.GlobLiteralPrefixDerivation above, which
// tests glob_literal_prefix directly and isn't subject to filesystem
// enumeration order.
TEST(Scanner, ExpandGlobStartsAtLiteralPrefix) {
    auto dir = make_tempdir("mcpp-scanner-prefix");
    write(dir / "other" / "b.cppm", "export module b;\n");
    write(dir / "src" / "a.cppm", "export module a;\n");

    auto files = expand_glob(dir, "src/**/*.cppm");

    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], dir / "src" / "a.cppm");

    std::filesystem::remove_all(dir);
}

// Supplementary (not the primary regression guard — see
// Scanner.GlobLiteralPrefixDerivation and Scanner.ExpandGlobStartsAtLiteralPrefix
// above): on filesystems where directory enumeration happens to visit "junk"
// before "src", this additionally proves the walk never even touches an
// unreadable sibling tree. The landmine's fire/no-fire outcome depends on
// unspecified recursive_directory_iterator enumeration order, so the guard
// condition is made explicit below rather than silently no-op'ing — if the
// landmine didn't fire (order-dependent), the test still asserts the
// positive result and reports (via trace) that the landmine was inert this
// run, instead of pretending it verified the old-code-fails claim.
TEST(Scanner, ExpandGlobStartsAtLiteralPrefixLandmine) {
    auto dir = make_tempdir("mcpp-scanner-prefix-landmine");
    auto blocked = dir / "junk" / "blocked";
    std::filesystem::create_directories(blocked);
    write(blocked / "sentinel.cppm", "export module sentinel;\n");
    std::error_code permEc;
    std::filesystem::permissions(blocked, std::filesystem::perms::none, permEc);

    write(dir / "src" / "a.cppm", "export module a;\n");

    // If permissions can't be locked down here (e.g. running as root), the
    // landmine can't fire — skip rather than risk a false pass/fail.
    std::error_code probeEc;
    std::filesystem::directory_iterator(blocked, probeEc);
    if (!probeEc) {
        std::filesystem::permissions(blocked, std::filesystem::perms::all, permEc);
        std::filesystem::remove_all(dir);
        GTEST_SKIP() << "cannot restrict directory permissions in this environment";
    }

    auto files = expand_glob(dir, "src/**/*.cppm");

    // Restore permissions before cleanup (remove_all needs to read `blocked`).
    std::filesystem::permissions(blocked, std::filesystem::perms::all, permEc);

    // This is the guard being made explicit: whether the landmine actually
    // fired for the OLD (unbounded) code depends on unspecified enumeration
    // order, so it cannot be asserted here either way. What IS asserted,
    // unconditionally, is the functional outcome — the bounded walk must
    // return exactly the "src" match regardless of "junk"'s enumeration
    // position. That functional assertion is real signal on every run; it
    // just isn't, by itself, an order-independent proof that old code would
    // have failed here (that proof now lives in
    // Scanner.GlobLiteralPrefixDerivation instead).
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], dir / "src" / "a.cppm");

    std::filesystem::remove_all(dir);
}

// mcpp#225: the walk must prune VCS metadata (.git) and mcpp's own build
// output (target) the same way it already prunes .mcpp (mcpp#230). Glob has
// no literal prefix ("**/*.cppm") so the walk starts at root and would
// otherwise reach all three excluded trees.
TEST(Scanner, ExcludesGitAndTargetAndMcpp) {
    auto dir = make_tempdir("mcpp-scanner-exclude");
    write(dir / ".git" / "x.cppm", "export module x;\n");
    write(dir / "target" / "y.cppm", "export module y;\n");
    write(dir / ".mcpp" / "z.cppm", "export module z;\n");
    write(dir / "src" / "ok.cppm", "export module ok;\n");

    auto files = expand_glob(dir, "**/*.cppm");

    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], dir / "src" / "ok.cppm");

    std::filesystem::remove_all(dir);
}

TEST(Scanner, RecordsPackageLocalIncludeDirs) {
    auto dir = make_tempdir("mcpp-scanner-includes");
    write(dir / "src" / "foo.cpp",
          "int answer() { return 42; }\n");
    std::filesystem::create_directories(dir / "include");
    std::filesystem::create_directories(dir / "private" / "nested");

    mcpp::manifest::Manifest m;
    m.package.name = "pkg";
    m.modules.sources = {"src/*.cpp"};
    m.buildConfig.includeDirs = {"include", "private/*"};

    auto r = scan_packages({PackageRoot{dir, m}});
    ASSERT_TRUE(r.errors.empty());
    ASSERT_EQ(r.graph.units.size(), 1u);

    auto const& dirs = r.graph.units[0].localIncludeDirs;
    ASSERT_EQ(dirs.size(), 2u);
    EXPECT_EQ(dirs[0], dir / "include");
    EXPECT_EQ(dirs[1], dir / "private" / "nested");

    std::filesystem::remove_all(dir);
}

TEST(Scanner, UsesResolvedPackagePrivateBuildIncludeDirs) {
    auto dir = make_tempdir("mcpp-scanner-resolved-includes");
    write(dir / "src" / "foo.cpp",
          "int answer() { return 42; }\n");
    std::filesystem::create_directories(dir / "legacy");
    std::filesystem::create_directories(dir / "resolved");

    mcpp::manifest::Manifest m;
    m.package.name = "pkg";
    m.modules.sources = {"src/*.cpp"};
    m.buildConfig.includeDirs = {"legacy"};

    PackageRoot p{dir, m};
    p.usageResolved = true;
    p.privateBuild.includeDirs = {dir / "resolved"};

    auto r = scan_packages({p});
    ASSERT_TRUE(r.errors.empty());
    ASSERT_EQ(r.graph.units.size(), 1u);

    auto const& dirs = r.graph.units[0].localIncludeDirs;
    ASSERT_EQ(dirs.size(), 1u);
    EXPECT_EQ(dirs[0], dir / "resolved");

    std::filesystem::remove_all(dir);
}

TEST(Scanner, PartitionImportFromPrimaryInterface) {
    // Primary module interface: `export module foo;` → logicalName = "foo".
    // `import :tls;` resolves to "foo:tls".
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "foo.cppm",
          "export module foo;\n"
          "import :tls;\n");
    auto u = scan_file(dir / "src" / "foo.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_EQ(u->requires_.size(), 1u);
    EXPECT_EQ(u->requires_[0].logicalName, "foo:tls");
    std::filesystem::remove_all(dir);
}

TEST(Scanner, PartitionImportFromAnotherPartition) {
    // Partition interface: `export module foo:http;` → logicalName = "foo:http".
    // `import :tls;` must resolve to "foo:tls" (the sibling partition),
    // NOT "foo:http:tls" (which is what a naive prepend produces).
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "http.cppm",
          "export module foo:http;\n"
          "import :tls;\n"
          "import :socket;\n");
    auto u = scan_file(dir / "src" / "http.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_TRUE(u->provides.has_value());
    EXPECT_EQ(u->provides->logicalName, "foo:http");
    ASSERT_EQ(u->requires_.size(), 2u);
    EXPECT_EQ(u->requires_[0].logicalName, "foo:tls");
    EXPECT_EQ(u->requires_[1].logicalName, "foo:socket");
    std::filesystem::remove_all(dir);
}

TEST(Scanner, PartitionImportWithDottedModuleName) {
    // Dotted module names (xpkg-style, e.g. `mcpplibs.tinyhttps:http`)
    // — only the colon-prefixed partition suffix is what we strip.
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "http.cppm",
          "export module mcpplibs.tinyhttps:http;\n"
          "import :tls;\n");
    auto u = scan_file(dir / "src" / "http.cppm", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    ASSERT_EQ(u->requires_.size(), 1u);
    EXPECT_EQ(u->requires_[0].logicalName, "mcpplibs.tinyhttps:tls");
    std::filesystem::remove_all(dir);
}

TEST(Scanner, RejectsConditionalImport) {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "main.cpp",
          "import std;\n"
          "#ifdef WANT_X\n"
          "import x;\n"
          "#endif\n"
          "int main(){}");
    auto r = scan_file(dir / "main.cpp", "pkg");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("conditional"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(Scanner, RejectsHeaderUnit) {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "main.cpp",
          "import std;\n"
          "import \"x.h\";\n"
          "int main(){}");
    auto r = scan_file(dir / "main.cpp", "pkg");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("header units"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(Scanner, ObjectiveCSourceIsCLike) {
    auto dir = make_tempdir("mcpp-scanner-objc");
    write(dir / "src" / "window.m",
          "import Cocoa;\n"
          "int answer(void) { return 42; }\n");

    auto u = scan_file(dir / "src" / "window.m", "pkg");
    ASSERT_TRUE(u.has_value()) << u.error().format();
    EXPECT_FALSE(u->provides.has_value());
    EXPECT_TRUE(u->requires_.empty());

    std::filesystem::remove_all(dir);
}

TEST(Validate, ModuleNameNotRequiredToMatchPackageName) {
    // 0.0.10+: module name does NOT need to be prefixed by package name.
    // The library author decides the module naming convention.
    Graph g;
    SourceUnit u;
    u.path = "/x/foo.cppm";
    u.packageName = "myorg.foo";
    u.provides = ModuleId{"completely.different.name"};
    g.units.push_back(u);
    g.producerOf["completely.different.name"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "myorg.foo";

    auto rep = validate(g, m);
    EXPECT_TRUE(rep.ok()) << "module name mismatch should not be an error";
}

TEST(Validate, ForbiddenTopName) {
    Graph g;
    SourceUnit u;
    u.path = "/x/foo.cppm";
    u.packageName = "myorg.foo";
    u.provides = ModuleId{"util"};
    g.units.push_back(u);
    g.producerOf["util"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "myorg.foo";

    auto rep = validate(g, m);
    EXPECT_FALSE(rep.ok());
}

TEST(Validate, LibRootHappyPath) {
    // Project: lib target "tinyhttps", convention puts the lib root at
    // src/tinyhttps.cppm exporting `mcpplibs.tinyhttps`. Two partition
    // siblings sit alongside.
    Graph g;
    SourceUnit root;
    root.path = "src/tinyhttps.cppm";
    root.packageName = "mcpplibs.tinyhttps";
    root.provides = ModuleId{"mcpplibs.tinyhttps"};
    g.units.push_back(root);
    g.producerOf["mcpplibs.tinyhttps"] = 0;
    SourceUnit p1;
    p1.path = "src/tls.cppm";
    p1.packageName = "mcpplibs.tinyhttps";
    p1.provides = ModuleId{"mcpplibs.tinyhttps:tls"};
    g.units.push_back(p1);
    g.producerOf["mcpplibs.tinyhttps:tls"] = 1;

    mcpp::manifest::Manifest m;
    m.package.name = "mcpplibs.tinyhttps";
    mcpp::manifest::Target t;
    t.name = "tinyhttps";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    auto rep = validate(g, m);     // empty projectRoot → on-disk check skipped
    EXPECT_TRUE(rep.ok()) << "errors:" << [&]{
        std::string s; for (auto& e : rep.errors) s += "\n  " + e.message; return s;
    }();
}

TEST(Validate, LibRootExportsPartitionIsError) {
    // Lib root file at the conventional path exports `:foo` (a partition
    // suffix) — must be rejected: lib root must be the primary module.
    Graph g;
    SourceUnit u;
    u.path = "src/tinyhttps.cppm";
    u.packageName = "mcpplibs.tinyhttps";
    u.provides = ModuleId{"mcpplibs.tinyhttps:something"};
    g.units.push_back(u);
    g.producerOf["mcpplibs.tinyhttps:something"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "mcpplibs.tinyhttps";
    mcpp::manifest::Target t;
    t.name = "tinyhttps";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    auto rep = validate(g, m);
    EXPECT_FALSE(rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("partition") != std::string::npos
            && e.message.find("primary module") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "expected lib-root partition error";
}

TEST(Validate, LibRootDifferentModuleNameIsAllowed) {
    // 0.0.10+: lib root module name does NOT need to match [package].name.
    // The library author decides the module name; the build tool auto-detects.
    Graph g;
    SourceUnit u;
    u.path = "src/tinyhttps.cppm";
    u.packageName = "mcpplibs.tinyhttps";
    u.provides = ModuleId{"some.other.module"};
    g.units.push_back(u);
    g.producerOf["some.other.module"] = 0;

    mcpp::manifest::Manifest m;
    m.package.name = "mcpplibs.tinyhttps";
    mcpp::manifest::Target t;
    t.name = "tinyhttps";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    auto rep = validate(g, m);
    EXPECT_TRUE(rep.ok()) << "module name mismatch should not be an error";
}

TEST(Validate, LibRootNotEnforcedForBinaryProject) {
    // Pure-binary project: no lib target → no lib-root checks. Even if a
    // file at src/<tail>.cppm exists exporting an unrelated module, no
    // error should fire.
    Graph g;
    mcpp::manifest::Manifest m;
    m.package.name = "myapp";
    mcpp::manifest::Target t;
    t.name = "myapp";
    t.kind = mcpp::manifest::Target::Binary;
    t.main = "src/main.cpp";
    m.targets.push_back(t);

    auto rep = validate(g, m);
    EXPECT_TRUE(rep.ok());
}

TEST(Validate, LibRootMissingFileWithExplicitPathIsError) {
    Graph g;
    mcpp::manifest::Manifest m;
    m.package.name = "myorg.foo";
    m.lib.path = "src/does-not-exist.cppm";
    mcpp::manifest::Target t;
    t.name = "foo";
    t.kind = mcpp::manifest::Target::Library;
    m.targets.push_back(t);

    // Pass a non-empty projectRoot so the on-disk check is enabled.
    auto rep = validate(g, m, std::filesystem::current_path());
    EXPECT_FALSE(rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("does not exist") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "expected explicit-path-missing error";
}

TEST(TopoSort, DetectsCycle) {
    Graph g;
    g.units.resize(2);
    g.units[0].provides = ModuleId{"a"};
    g.units[1].provides = ModuleId{"b"};
    g.units[0].requires_.push_back({"b"});
    g.units[1].requires_.push_back({"a"});
    g.producerOf["a"] = 0;
    g.producerOf["b"] = 1;
    g.edges.push_back({0, 1});  // a->b
    g.edges.push_back({1, 0});  // b->a

    auto r = topo_sort(g);
    EXPECT_FALSE(r.has_value());
}

TEST(IsPublicPackage, DotMarksPublic) {
    EXPECT_TRUE(is_public_package_name("myorg.foo"));
    EXPECT_FALSE(is_public_package_name("foo"));
}

TEST(IsForbiddenTopModule, KnownNames) {
    EXPECT_TRUE(is_forbidden_top_module("core"));
    EXPECT_TRUE(is_forbidden_top_module("util.x"));
    EXPECT_FALSE(is_forbidden_top_module("myorg.foo"));
}

// G4: per-glob flags attach to exactly the matched units, in declaration
// order; a zero-hit glob warns (not errors — cfg-gated source sets may
// legitimately leave a glob empty on some targets).
TEST(Scanner, PerGlobFlagsAttachToMatchedUnits) {
    auto dir = make_tempdir("mcpp-scanner-globflags");
    write(dir / "src" / "hot.cpp", "int hot() { return 1; }\n");
    write(dir / "third_party" / "noisy.cpp", "int noisy() { return 2; }\n");

    mcpp::manifest::Manifest m;
    m.package.name = "globpkg";
    m.modules.sources = { "src/**/*.cpp", "third_party/**/*.cpp" };
    m.buildConfig.globFlags.push_back(
        { .glob = "third_party/**", .cxxflags = {"-w"} });
    m.buildConfig.globFlags.push_back(
        { .glob = "src/**", .cxxflags = {"-mavx2"}, .defines = {"HOT"} });
    m.buildConfig.globFlags.push_back(
        { .glob = "nothing/**", .cxxflags = {"-Wnever"} });

    auto res = scan_package(dir, m);
    ASSERT_TRUE(res.errors.empty());
    ASSERT_EQ(res.graph.units.size(), 2u);
    for (auto& u : res.graph.units) {
        if (u.path.filename() == "noisy.cpp") {
            EXPECT_EQ(u.packageCxxflags, (std::vector<std::string>{"-w"}));
        } else {
            // defines land before the entry's own flag lists
            EXPECT_EQ(u.packageCxxflags,
                      (std::vector<std::string>{"-DHOT", "-mavx2"}));
        }
    }
    // The unmatched glob warns.
    bool warned = false;
    for (auto& w : res.warnings)
        if (w.message.find("nothing/**") != std::string::npos) warned = true;
    EXPECT_TRUE(warned);

    std::filesystem::remove_all(dir);
}

// #228: expand_braces desugars `{a,b}` into a cartesian product of plain
// globs. No braces -> passthrough; multiple/nested groups combine.
TEST(Scanner, ExpandBracesDesugarsCartesianProduct) {
    EXPECT_EQ(expand_braces("a/**"), (std::vector<std::string>{"a/**"}));
    EXPECT_EQ(expand_braces("a/{x,y}/**"),
              (std::vector<std::string>{"a/x/**", "a/y/**"}));
    EXPECT_EQ(expand_braces("a/{x,y}/{1,2}"),
              (std::vector<std::string>{"a/x/1", "a/x/2", "a/y/1", "a/y/2"}));
    // Nested group.
    EXPECT_EQ(expand_braces("a/{x,{y,z}}/**"),
              (std::vector<std::string>{"a/x/**", "a/y/**", "a/z/**"}));
}

// Review fix on #228: brace-nesting recursion depth is bounded (cap: 32
// levels), so a pathological manifest with deeply nested `{` cannot
// stack-overflow. Beyond the cap the remainder is passed through as a
// literal instead of throwing — this must simply return, not crash.
TEST(Scanner, ExpandBracesBoundsDeeplyNestedRecursion) {
    std::string glob = "a/";
    constexpr int kDepth = 500;  // far beyond the 32-level cap
    for (int i = 0; i < kDepth; ++i) glob += "{";
    glob += "x";
    for (int i = 0; i < kDepth; ++i) glob += "}";

    std::vector<std::string> out;
    ASSERT_NO_THROW(out = expand_braces(glob));
    EXPECT_FALSE(out.empty());
}

// #228: expand_glob applies brace desugaring at its entry, so a glob like
// "p/{aac,bsf}/**" matches files under EITHER alternative directory and
// nothing else (a sibling directory outside the brace group, "opus", must
// not appear).
TEST(Scanner, ExpandGlobBraceAlternation) {
    auto dir = make_tempdir("mcpp-scanner-brace");
    write(dir / "p" / "aac" / "x.c", "int x;\n");
    write(dir / "p" / "bsf" / "y.c", "int y;\n");
    write(dir / "p" / "opus" / "z.c", "int z;\n");

    auto files = expand_glob(dir, "p/{aac,bsf}/**");

    ASSERT_EQ(files.size(), 2u);
    std::set<std::string> names;
    for (auto& f : files) names.insert(f.filename().string());
    EXPECT_TRUE(names.contains("x.c"));
    EXPECT_TRUE(names.contains("y.c"));
    EXPECT_FALSE(names.contains("z.c"));

    std::filesystem::remove_all(dir);
}

// #228: brace alternation in a [build].flags glob must also match — the
// per-glob-flags match point (scan_one_into's apply_glob_flags) uses
// path_matches_glob directly rather than expand_glob's walk, so it needs its
// own desugaring wire-up.
TEST(Scanner, PerGlobFlagsMatchBraceAlternation) {
    auto dir = make_tempdir("mcpp-scanner-globflags-brace");
    write(dir / "p" / "aac" / "x.cpp", "int x() { return 1; }\n");
    write(dir / "p" / "bsf" / "y.cpp", "int y() { return 2; }\n");
    write(dir / "p" / "opus" / "z.cpp", "int z() { return 3; }\n");

    mcpp::manifest::Manifest m;
    m.package.name = "bracepkg";
    m.modules.sources = { "p/**/*.cpp" };
    m.buildConfig.globFlags.push_back(
        { .glob = "p/{aac,bsf}/**", .defines = {"CODEC"} });

    auto res = scan_package(dir, m);
    ASSERT_TRUE(res.errors.empty());
    ASSERT_EQ(res.graph.units.size(), 3u);
    for (auto& u : res.graph.units) {
        bool wantsDefine = u.path.filename() != "z.cpp";
        bool hasDefine = std::find(u.packageCxxflags.begin(), u.packageCxxflags.end(),
                                    "-DCODEC") != u.packageCxxflags.end();
        EXPECT_EQ(hasDefine, wantsDefine) << u.path.string();
    }

    std::filesystem::remove_all(dir);
}

// G8b: relative -I flags are root-relative in the manifest but ninja runs
// with cwd = output dir — the scanner absolutizes them on every unit.
// The absolute spelling is NORMALIZED to native separators (#390): MSVC's
// path keeps the input `/` verbatim, and `-I/abs/path` written with forward
// slashes used to survive into the CDB's arguments as a mixed path.
TEST(Scanner, RelativeIncludeFlagsAbsolutized) {
    auto dir = make_tempdir("mcpp-scanner-relinc");
    write(dir / "src" / "a.cpp", "int a();\n");

    mcpp::manifest::Manifest m;
    m.package.name = "relinc";
    m.modules.sources = {"src/*.cpp"};

    // via package flags
    auto res = [&] {
        mcpp::manifest::Manifest mm = m;
        mm.buildConfig.cxxflags = {"-Iinc", "-I/abs/path", "-DKEEP"};
        return scan_package(dir, mm);
    }();
    ASSERT_EQ(res.graph.units.size(), 1u);
    auto& fl = res.graph.units[0].packageCxxflags;
    EXPECT_EQ(fl[0], "-I" + (dir / "inc").string());
    if constexpr (std::filesystem::path::preferred_separator == '\\')
        EXPECT_EQ(fl[1], "-I\\abs\\path");   // absolute stays, but native
    else
        EXPECT_EQ(fl[1], "-I/abs/path");     // absolute stays
    EXPECT_EQ(fl[2], "-DKEEP");              // non-include untouched

    std::filesystem::remove_all(dir);
}
