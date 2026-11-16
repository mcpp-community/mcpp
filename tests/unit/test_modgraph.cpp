
import std;
import mcpplibs.mtest;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.manifest;
import mcpp.source_kind;

using namespace mcpplibs::mtest;

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

UTest provides_and_requires{"Scanner/ProvidesAndRequires", [] {
    auto dir = make_tempdir("mcpp-scanner");
    // NOTE: avoid raw string literal for module source — clang-scan-deps
    // on Windows may false-positive on `import bar;` inside R"(...)".
    write(dir / "src" / "foo.cppm",
          "export module foo;\n"
          "import std;\n"
          "import bar;\n"
          "export int answer();\n");

    auto u = scan_file(dir / "src" / "foo.cppm", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(u->provides.has_value()));
    expect(eq(u->provides->logicalName, "foo"));
    expect(fatal(eq(u->requires_.size(), 2u)));
    expect(eq(u->requires_[0].logicalName, "std"));
    expect(eq(u->requires_[1].logicalName, "bar"));

    std::filesystem::remove_all(dir);
}};

// Regression: `import` lines that live INSIDE a multi-line raw-string literal
// (e.g. a `mcpp new --template gui` skeleton embedded as R"GUI( ... )GUI") must
// not be detected as real module imports. Before the fix this produced a
// spurious "module 'imgui.core' imported but not provided" warning.
// ─── implementation partitions ────────────────────────────────────────────
//
// `module M:part;` and `module M;` share a spelling and are different
// declarations. Conflating them left the graph without the edge from the unit
// that IMPORTS a partition to the unit that DEFINES it, so build order was
// unconstrained: GCC and macOS clang recovered through their own dependency
// scan, Windows clang failed with `failed to read compiled module`. The scanner
// also warned "imported but not provided in this build" on every platform,
// which read like a note instead of the cause.

UTest implementation_partition_provides_its_partition_name{"Scanner/ImplementationPartitionProvidesItsPartitionName", [] {
    auto dir = make_tempdir("scan-implpart");
    std::filesystem::create_directories(dir / "src");
    write(dir / "src" / "secret.cppm",
          "module mathkit:secret;\n"
          "namespace mk { int bias() { return 1; } }\n");

    auto u = scan_file(dir / "src" / "secret.cppm", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(u.has_value()));
    expect(fatal(u->provides.has_value()));
    expect(eq(u->provides->logicalName, "mathkit:secret"));
    // Not an interface: its source must not be published by `mcpp pack`.
    // Compared against the value, not tested for truthiness — the field is a
    // tri-state now (nullopt means "nobody determined this"), and on an
    // optional both EXPECT_FALSE and EXPECT_TRUE ask about the wrong thing.
    expect(eq(u->providesInterface, std::optional<bool>{false}));
    // And it must not require its own name, which is what it used to do.
    for (auto const& r : u->requires_)
        expect(ne(r.logicalName, "mathkit:secret"));
    std::filesystem::remove_all(dir);
}};

UTest interface_partition_is_marked_as_an_interface{"Scanner/InterfacePartitionIsMarkedAsAnInterface", [] {
    auto dir = make_tempdir("scan-ifacepart");
    std::filesystem::create_directories(dir / "src");
    write(dir / "src" / "api.cppm", "export module mathkit:api;\nexport int f();\n");

    auto u = scan_file(dir / "src" / "api.cppm", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(u.has_value()));
    expect(fatal(u->provides.has_value()));
    expect(eq(u->provides->logicalName, "mathkit:api"));
    expect(eq(u->providesInterface, std::optional<bool>{true}));
    std::filesystem::remove_all(dir);
}};

// ─── the declaration form (build database roles) ─────────────────────────
//
// `module M;` is stored as `requires M`, exactly as `import M;` is, so the form
// cannot be recovered from provides/requires. The scanner records what it read.
UTest declaration_form_is_recorded_as_read{"Scanner/DeclarationFormIsRecordedAsRead", [] {
    auto dir = make_tempdir("scan-declform");
    std::filesystem::create_directories(dir / "src");
    struct Case { const char* file; const char* text; ModuleDeclaration want; };
    const Case cases[] = {
        {"iface.cppm",  "export module mathkit;\nexport int f();\n",
                        ModuleDeclaration::Interface},
        {"ipart.cppm",  "export module mathkit:api;\nexport int g();\n",
                        ModuleDeclaration::InterfacePartition},
        {"impart.cppm", "module mathkit:secret;\nint h() { return 1; }\n",
                        ModuleDeclaration::ImplementationPartition},
        {"impl.cpp",    "module mathkit;\nint f() { return 2; }\n",
                        ModuleDeclaration::Implementation},
        {"user.cpp",    "import mathkit;\nint main() { return f(); }\n",
                        ModuleDeclaration::None},
    };
    for (auto const& c : cases) {
        write(dir / "src" / c.file, c.text);
        auto u = scan_file(dir / "src" / c.file, "pkg", mcpp::builtin_extension_table());
        expect(fatal(u.has_value())) << c.file;
        expect(eq(static_cast<int>(u->declaration), static_cast<int>(c.want))) << c.file;
    }
    std::filesystem::remove_all(dir);
}};

// ─── an entry source no `sources` glob matched ─────────────────────────────
//
// A discovered test, or a `main` outside the globs, is not in the package scan.
// Its imports were read from line-leading `import` alone, so an import inside a
// comment or a raw string was recorded as one: a scanner test in lsp-mcpp was
// planned as importing three modules no source provides.
UTest an_entry_source_is_read_by_the_scanner{"Scanner/AnEntrySourceIsReadByTheScanner", [] {
    auto dir = make_tempdir("scan-entry");
    write(dir / "t.cpp",
          "import std;\n"
          "/* a comment\n"
          "   import in.comment; */\n"
          "auto s = R\"x(\n"
          "import in.raw;\n"
          ")x\";\n"
          "import real;\n"
          "int main() {}\n");
    auto u = scan_entry_file(dir / "t.cpp", "pkg", mcpp::builtin_extension_table());
    std::vector<std::string> names;
    for (auto const& r : u.requires_) names.push_back(r.logicalName);
    expect(eq(names, (std::vector<std::string>{"std", "real"})));
    expect(eq(static_cast<int>(u.declaration), static_cast<int>(ModuleDeclaration::None)));
    std::filesystem::remove_all(dir);
}};

// The scanner's refusals were never applied to such a file, and are not now: a
// file it refuses keeps its line-leading imports, and its form is not decided.
UTest an_entry_source_the_scanner_refuses_keeps_its_imports{"Scanner/AnEntrySourceTheScannerRefusesKeepsItsImports", [] {
    auto dir = make_tempdir("scan-entry-refused");
    write(dir / "t.cpp",
          "#ifdef WITH_EXTRA\n"
          "import extra;\n"
          "#endif\n"
          "export import std;\n"
          "int main() {}\n");
    expect(fatal(!(scan_file(dir / "t.cpp", "pkg", mcpp::builtin_extension_table()).has_value())));
    auto u = scan_entry_file(dir / "t.cpp", "pkg", mcpp::builtin_extension_table());
    std::vector<std::string> names;
    for (auto const& r : u.requires_) names.push_back(r.logicalName);
    expect(eq(names, (std::vector<std::string>{"extra", "std"})));
    expect(eq(static_cast<int>(u.declaration), static_cast<int>(ModuleDeclaration::Unknown)));
    std::filesystem::remove_all(dir);
}};

UTest plain_implementation_unit_still_requires_its_interface{"Scanner/PlainImplementationUnitStillRequiresItsInterface", [] {
    auto dir = make_tempdir("scan-implunit");
    std::filesystem::create_directories(dir / "src");
    write(dir / "src" / "impl.cpp", "module mathkit;\nnamespace mk { int g() { return 2; } }\n");

    auto u = scan_file(dir / "src" / "impl.cpp", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(u.has_value()));
    expect(!(u->provides.has_value()));
    expect(fatal(eq(u->requires_.size(), 1u)));
    expect(eq(u->requires_[0].logicalName, "mathkit"));
    std::filesystem::remove_all(dir);
}};

UTest partition_import_resolves_inside_an_implementation_unit{"Scanner/PartitionImportResolvesInsideAnImplementationUnit", [] {
    // The other half of the same bug: resolution keyed off `u.provides`, and an
    // implementation unit has none — so `import :secret;` stayed the literal
    // `:secret`, which nothing provides.
    auto dir = make_tempdir("scan-implimport");
    std::filesystem::create_directories(dir / "src");
    write(dir / "src" / "impl.cpp",
          "module mathkit;\nimport :secret;\nnamespace mk { int g(); }\n");

    auto u = scan_file(dir / "src" / "impl.cpp", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(u.has_value()));
    bool found = false;
    for (auto const& r : u->requires_) {
        expect(ne(r.logicalName, ":secret"));
        if (r.logicalName == "mathkit:secret") found = true;
    }
    expect(bool(found)) << "`import :secret;` did not resolve to mathkit:secret";
    std::filesystem::remove_all(dir);
}};

UTest partition_import_resolves_inside_a_partition{"Scanner/PartitionImportResolvesInsideAPartition", [] {
    // `export module foo:http;` + `import :tls;` must give `foo:tls`, not
    // `foo:http:tls` — the case the old code was written for, still true.
    auto dir = make_tempdir("scan-partpart");
    std::filesystem::create_directories(dir / "src");
    write(dir / "src" / "http.cppm", "export module foo:http;\nimport :tls;\nexport int h();\n");

    auto u = scan_file(dir / "src" / "http.cppm", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(u.has_value()));
    expect(fatal(eq(u->requires_.size(), 1u)));
    expect(eq(u->requires_[0].logicalName, "foo:tls"));
    std::filesystem::remove_all(dir);
}};

UTest ignores_imports_inside_raw_string_literal{"Scanner/IgnoresImportsInsideRawStringLiteral", [] {
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

    auto u = scan_file(dir / "src" / "gen.cppm", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(u->provides.has_value()));
    expect(eq(u->provides->logicalName, "gen"));
    // Only the two genuine top-level imports — NOT imgui.core / imgui.app.
    expect(fatal(eq(u->requires_.size(), 2u)));
    expect(eq(u->requires_[0].logicalName, "std"));
    expect(eq(u->requires_[1].logicalName, "bar"));
    for (auto& r : u->requires_) {
        expect(ne(r.logicalName, "imgui.core"));
        expect(ne(r.logicalName, "imgui.app"));
    }

    std::filesystem::remove_all(dir);
}};

// A single-line raw string with an embedded import-looking body stays code.
UTest ignores_import_inside_single_line_raw_string{"Scanner/IgnoresImportInsideSingleLineRawString", [] {
    auto dir = make_tempdir("mcpp-scanner-raw1");
    write(dir / "src" / "one.cppm",
          "export module one;\n"
          "const char* s = R\"(import nope;)\";\n"
          "import real;\n");

    auto u = scan_file(dir / "src" / "one.cppm", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(eq(u->requires_.size(), 1u)));
    expect(eq(u->requires_[0].logicalName, "real"));

    std::filesystem::remove_all(dir);
}};

// Assembly units skip the module text scan entirely (like .c): an .asm/.S
// file legally contains no `import`/`module` declarations, and asm comment
// syntax would misparse.
UTest assembly_sources_skip_module_scan{"Scanner/AssemblySourcesSkipModuleScan", [] {
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
        auto u = scan_file(dir / "src" / name, "pkg", mcpp::builtin_extension_table());
        if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
        expect(!(u->provides.has_value())) << name;
        expect(u->requires_.empty()) << name;
    }

    std::filesystem::remove_all(dir);
}};

// G8a: source globs follow directory symlinks (vendored trees are often
// symlink farms) without looping on cycles.
UTest expand_glob_follows_directory_symlinks{"Scanner/ExpandGlobFollowsDirectorySymlinks", [] {
    auto dir = make_tempdir("mcpp-scanner-symlink");
    auto real = dir / "vendor-real";
    write(real / "impl.cpp", "int f() { return 1; }\n");
    std::error_code ec;
    std::filesystem::create_directories(dir / "src");
    std::filesystem::create_directory_symlink(real, dir / "src" / "vendor", ec);
    if (ec) skip([&] { std::ostringstream text; text << "symlinks unsupported here: " << ec.message(); return text.str(); }());
    // A cycle: the linked tree points back at its parent.
    std::filesystem::create_directory_symlink(dir, real / "loop", ec);

    auto files = expand_glob(dir, "src/**/*.cpp");

    expect(fatal(eq(files.size(), 1u)));
    expect(eq(files[0].filename(), "impl.cpp"));

    std::filesystem::remove_all(dir);
}};

// mcpp#225 review finding: glob_literal_prefix's contract (the piece that
// actually decides WHERE expand_glob/expand_dir_glob start walking) is the
// thing that must be locked down deterministically. Calling the pure helper
// directly — rather than inferring its behavior from a filesystem walk whose
// entry order is unspecified by the standard — makes this test's pass/fail
// independent of directory-enumeration order on any filesystem.
UTest glob_literal_prefix_derivation{"Scanner/GlobLiteralPrefixDerivation", [] {
    expect(eq(glob_literal_prefix("src/**/*.cppm"), "src"));
    // Wildcard already in the first segment: no literal directory to bound to.
    expect(eq(glob_literal_prefix("**/*.c"), ""));
    // No wildcard at all: the full parent directory path is the prefix.
    expect(eq(glob_literal_prefix("a/b/c.cpp").generic_string(), "a/b"));
    // Truncate back to the last COMPLETE '/' before the first wildcard char —
    // "x*.cpp" is a partial segment, not a real directory named "x".
    expect(eq(glob_literal_prefix("src/x*.cpp"), "src"));
    // '{' is treated as a segment-boundary wildcard char (brace-expansion
    // globs are desugared before reaching this helper — see its comment in
    // scanner.cppm), so the prefix truncates at the last '/' before it.
    expect(eq(glob_literal_prefix("a/{x,y}/z"), "a"));
}};

// MSVC's std::filesystem::path preserves the separators of the string it was
// constructed from, so a raw `a/b` prefix stays generic and `root / p` turns
// into a MIXED `root\a/b` — which used to leak into compile_commands.json
// (`file` / `-c` for every source under a multi-segment glob) and break CLion.
// glob_literal_prefix must return NATIVE separators so the walk and everything
// downstream is native too. (The generic spelling is already locked down by
// Scanner.GlobLiteralPrefixDerivation above.)
UTest glob_literal_prefix_uses_native_separators{"Scanner/GlobLiteralPrefixUsesNativeSeparators", [] {
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        expect(eq(glob_literal_prefix("a/b/c.cpp").string(), "a\\b"));
        expect(eq(glob_literal_prefix("a/b/c.cpp").string().find('/'), std::string::npos));
    }
}};

// The exported converter itself — both spelling directions.
UTest glob_native_path_from_generic{"Glob/NativePathFromGeneric", [] {
    auto p = mcpp::modgraph::native_path_from_generic("a/b/c");
    expect(eq(p.generic_string(), "a/b/c"));
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        expect(eq(p.string(), "a\\b\\c"));
        // Already-native input is untouched.
        expect(eq(mcpp::modgraph::native_path_from_generic("C:\\x\\y").string(), "C:\\x\\y"));
    }
}};

// The end-to-end shape of the reported bug: a source under a multi-segment
// glob (`generated/modules/**/*.cppm`) must come out of expand_glob with
// NATIVE separators on Windows — the mixed `root\generated/modules\a.cppm`
// was what compile_commands.json's `file` field showed before the fix.
UTest expand_glob_multi_segment_prefix_uses_native_separators{"Scanner/ExpandGlobMultiSegmentPrefixUsesNativeSeparators", [] {
    auto dir = make_tempdir("mcpp-scanner-multi");
    write(dir / "generated" / "modules" / "a.cppm", "export module a;\n");

    auto files = expand_glob(dir, "generated/modules/**/*.cppm");

    expect(fatal(eq(files.size(), 1u)));
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        if (auto&& failed = expect(eq(files[0].string().find('/'), std::string::npos)); !failed) failed << files[0];
    }
    expect(eq(files[0].generic_string(), (dir / "generated" / "modules" / "a.cppm").generic_string()));

    std::filesystem::remove_all(dir);
}};

// Same contract for the INCLUDE-DIR channel (expand_dir_glob): a multi-segment
// `third_party/inc` entry must yield a native path or the CDB's -I carries the
// mixed form.
UTest expand_dir_glob_multi_segment_uses_native_separators{"Scanner/ExpandDirGlobMultiSegmentUsesNativeSeparators", [] {
    auto dir = make_tempdir("mcpp-scanner-dirglob");
    std::filesystem::create_directories(dir / "third_party" / "inc");

    auto dirs = expand_dir_glob(dir, "third_party/inc");

    expect(fatal(eq(dirs.size(), 1u)));
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        if (auto&& failed = expect(eq(dirs[0].string().find('/'), std::string::npos)); !failed) failed << dirs[0];
    }
    expect(eq(dirs[0].generic_string(), (dir / "third_party" / "inc").generic_string()));

    std::filesystem::remove_all(dir);
}};

// mcpp#225: expand_glob must bound its walk to the glob's literal directory
// prefix ("src" for "src/**/*.cppm") instead of always walking the whole
// root and lexically filtering afterward. This is the FUNCTIONAL half of the
// regression guard: given a normal (non-adversarial) tree with files outside
// "src", a bounded walk returns exactly the "src" matches. The core "walk
// starts AT the literal prefix, not root" behavior is now locked down
// deterministically by Scanner.GlobLiteralPrefixDerivation above, which
// tests glob_literal_prefix directly and isn't subject to filesystem
// enumeration order.
UTest expand_glob_starts_at_literal_prefix{"Scanner/ExpandGlobStartsAtLiteralPrefix", [] {
    auto dir = make_tempdir("mcpp-scanner-prefix");
    write(dir / "other" / "b.cppm", "export module b;\n");
    write(dir / "src" / "a.cppm", "export module a;\n");

    auto files = expand_glob(dir, "src/**/*.cppm");

    expect(fatal(eq(files.size(), 1u)));
    expect(eq(files[0], dir / "src" / "a.cppm"));

    std::filesystem::remove_all(dir);
}};

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
UTest expand_glob_starts_at_literal_prefix_landmine{"Scanner/ExpandGlobStartsAtLiteralPrefixLandmine", [] {
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
        skip("cannot restrict directory permissions in this environment");
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
    expect(fatal(eq(files.size(), 1u)));
    expect(eq(files[0], dir / "src" / "a.cppm"));

    std::filesystem::remove_all(dir);
}};

// mcpp#225: the walk must prune VCS metadata (.git) and mcpp's own build
// output (target) the same way it already prunes .mcpp (mcpp#230). Glob has
// no literal prefix ("**/*.cppm") so the walk starts at root and would
// otherwise reach all three excluded trees.
UTest excludes_git_and_target_and_mcpp{"Scanner/ExcludesGitAndTargetAndMcpp", [] {
    auto dir = make_tempdir("mcpp-scanner-exclude");
    write(dir / ".git" / "x.cppm", "export module x;\n");
    write(dir / "target" / "y.cppm", "export module y;\n");
    write(dir / ".mcpp" / "z.cppm", "export module z;\n");
    write(dir / "src" / "ok.cppm", "export module ok;\n");

    auto files = expand_glob(dir, "**/*.cppm");

    expect(fatal(eq(files.size(), 1u)));
    expect(eq(files[0], dir / "src" / "ok.cppm"));

    std::filesystem::remove_all(dir);
}};

UTest records_package_local_include_dirs{"Scanner/RecordsPackageLocalIncludeDirs", [] {
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
    expect(fatal(r.errors.empty()));
    expect(fatal(eq(r.graph.units.size(), 1u)));

    auto const& dirs = r.graph.units[0].localIncludeDirs;
    expect(fatal(eq(dirs.size(), 2u)));
    expect(eq(dirs[0], dir / "include"));
    expect(eq(dirs[1], dir / "private" / "nested"));

    std::filesystem::remove_all(dir);
}};

UTest uses_resolved_package_private_build_include_dirs{"Scanner/UsesResolvedPackagePrivateBuildIncludeDirs", [] {
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
    expect(fatal(r.errors.empty()));
    expect(fatal(eq(r.graph.units.size(), 1u)));

    auto const& dirs = r.graph.units[0].localIncludeDirs;
    expect(fatal(eq(dirs.size(), 1u)));
    expect(eq(dirs[0], dir / "resolved"));

    std::filesystem::remove_all(dir);
}};

// The realised [c-abi] environment (design 2026-09-18) broadcasts into
// `privateBuild.asmflags`, and this proves the wiring on the ONE point that
// went wrong first (coordinator report, openkal-musl spike): the broadcast
// must reach a GAS (.S) unit's `packageAsmflags`, exactly as it reaches a
// `.c`/`.cpp` unit's cflags/cxxflags -- and it must NOT reach a NASM (.asm)
// unit's, because NASM has no `--target=` concept and does not recognize a
// single clang-family flag. A `--target=x86_64-pc-cygwin` handed to `nasm`
// is not a degraded build, it is a build that does not assemble at all, so
// this is a refusal-shaped guarantee even though nothing here calls
// `refusal::record`: the wrong list for the wrong assembler.
UTest resolved_c_env_asmflags_reach_gas_units_only_not_nasm{"Scanner/ResolvedCEnvAsmflagsReachGasUnitsOnlyNotNasm", [] {
    auto dir = make_tempdir("mcpp-scanner-cenv-asm");
    write(dir / "src" / "gas.S",
          ".text\n.globl gas_marker\ngas_marker:\n  ret\n");
    write(dir / "src" / "nasm.asm",
          "section .text\nglobal nasm_marker\nnasm_marker:\n  ret\n");

    mcpp::manifest::Manifest m;
    m.package.name = "pkg";
    m.modules.sources = {"src/*.S", "src/*.asm"};

    PackageRoot p{dir, m};
    p.usageResolved = true;
    p.privateBuild.asmflags = {"--target=x86_64-pc-cygwin", "-fno-short-wchar"};

    auto r = scan_packages({p});
    expect(fatal(r.errors.empty()));
    expect(fatal(eq(r.graph.units.size(), 2u)));

    bool sawGas = false, sawNasm = false;
    for (auto const& u : r.graph.units) {
        if (u.kind == mcpp::SourceKind::GasAsm) {
            sawGas = true;
            expect(ne(std::find(u.packageAsmflags.begin(), u.packageAsmflags.end(),
                                "--target=x86_64-pc-cygwin"), u.packageAsmflags.end()))
                << "the realised environment must reach a GAS unit";
        } else if (u.kind == mcpp::SourceKind::NasmAsm) {
            sawNasm = true;
            expect(eq(std::find(u.packageAsmflags.begin(), u.packageAsmflags.end(),
                                "--target=x86_64-pc-cygwin"), u.packageAsmflags.end()))
                << "NASM does not understand a clang --target= flag and must "
                   "never receive it";
        }
    }
    expect(bool(sawGas));
    expect(bool(sawNasm));

    std::filesystem::remove_all(dir);
}};

UTest partition_import_from_primary_interface{"Scanner/PartitionImportFromPrimaryInterface", [] {
    // Primary module interface: `export module foo;` → logicalName = "foo".
    // `import :tls;` resolves to "foo:tls".
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "foo.cppm",
          "export module foo;\n"
          "import :tls;\n");
    auto u = scan_file(dir / "src" / "foo.cppm", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(eq(u->requires_.size(), 1u)));
    expect(eq(u->requires_[0].logicalName, "foo:tls"));
    std::filesystem::remove_all(dir);
}};

UTest partition_import_from_another_partition{"Scanner/PartitionImportFromAnotherPartition", [] {
    // Partition interface: `export module foo:http;` → logicalName = "foo:http".
    // `import :tls;` must resolve to "foo:tls" (the sibling partition),
    // NOT "foo:http:tls" (which is what a naive prepend produces).
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "http.cppm",
          "export module foo:http;\n"
          "import :tls;\n"
          "import :socket;\n");
    auto u = scan_file(dir / "src" / "http.cppm", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(u->provides.has_value()));
    expect(eq(u->provides->logicalName, "foo:http"));
    expect(fatal(eq(u->requires_.size(), 2u)));
    expect(eq(u->requires_[0].logicalName, "foo:tls"));
    expect(eq(u->requires_[1].logicalName, "foo:socket"));
    std::filesystem::remove_all(dir);
}};

UTest partition_import_with_dotted_module_name{"Scanner/PartitionImportWithDottedModuleName", [] {
    // Dotted module names (xpkg-style, e.g. `mcpplibs.tinyhttps:http`)
    // — only the colon-prefixed partition suffix is what we strip.
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "src" / "http.cppm",
          "export module mcpplibs.tinyhttps:http;\n"
          "import :tls;\n");
    auto u = scan_file(dir / "src" / "http.cppm", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(eq(u->requires_.size(), 1u)));
    expect(eq(u->requires_[0].logicalName, "mcpplibs.tinyhttps:tls"));
    std::filesystem::remove_all(dir);
}};

UTest rejects_conditional_import{"Scanner/RejectsConditionalImport", [] {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "main.cpp",
          "import std;\n"
          "#ifdef WANT_X\n"
          "import x;\n"
          "#endif\n"
          "int main(){}");
    auto r = scan_file(dir / "main.cpp", "pkg", mcpp::builtin_extension_table());
    expect(!r.has_value());
    expect(ne(r.error().message.find("conditional"), std::string::npos));
    std::filesystem::remove_all(dir);
}};

UTest rejects_header_unit{"Scanner/RejectsHeaderUnit", [] {
    auto dir = make_tempdir("mcpp-scanner");
    write(dir / "main.cpp",
          "import std;\n"
          "import \"x.h\";\n"
          "int main(){}");
    auto r = scan_file(dir / "main.cpp", "pkg", mcpp::builtin_extension_table());
    expect(!r.has_value());
    expect(ne(r.error().message.find("header units"), std::string::npos));
    std::filesystem::remove_all(dir);
}};

UTest objective_c_source_is_c_like{"Scanner/ObjectiveCSourceIsCLike", [] {
    auto dir = make_tempdir("mcpp-scanner-objc");
    write(dir / "src" / "window.m",
          "import Cocoa;\n"
          "int answer(void) { return 42; }\n");

    auto u = scan_file(dir / "src" / "window.m", "pkg", mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(!(u->provides.has_value()));
    expect(u->requires_.empty());

    std::filesystem::remove_all(dir);
}};

UTest module_name_not_required_to_match_package_name{"Validate/ModuleNameNotRequiredToMatchPackageName", [] {
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
    expect(rep.ok()) << "module name mismatch should not be an error";
}};

UTest forbidden_top_name{"Validate/ForbiddenTopName", [] {
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
    expect(!rep.ok());
}};

UTest lib_root_happy_path{"Validate/LibRootHappyPath", [] {
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
    expect(rep.ok()) << "errors:" << [&]{
        std::string s; for (auto& e : rep.errors) s += "\n  " + e.message; return s;
    }();
}};

UTest lib_root_exports_partition_is_error{"Validate/LibRootExportsPartitionIsError", [] {
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
    expect(!rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("partition") != std::string::npos
            && e.message.find("primary module") != std::string::npos) { found = true; break; }
    }
    expect(bool(found)) << "expected lib-root partition error";
}};

UTest lib_root_different_module_name_is_allowed{"Validate/LibRootDifferentModuleNameIsAllowed", [] {
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
    expect(rep.ok()) << "module name mismatch should not be an error";
}};

UTest lib_root_not_enforced_for_binary_project{"Validate/LibRootNotEnforcedForBinaryProject", [] {
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
    expect(rep.ok());
}};

UTest lib_root_missing_file_with_explicit_path_is_error{"Validate/LibRootMissingFileWithExplicitPathIsError", [] {
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
    expect(!rep.ok());
    bool found = false;
    for (auto& e : rep.errors) {
        if (e.message.find("does not exist") != std::string::npos) { found = true; break; }
    }
    expect(bool(found)) << "expected explicit-path-missing error";
}};

UTest detects_cycle{"TopoSort/DetectsCycle", [] {
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
    expect(!r.has_value());
}};

UTest dot_marks_public{"IsPublicPackage/DotMarksPublic", [] {
    expect(is_public_package_name("myorg.foo"));
    expect(!is_public_package_name("foo"));
}};

UTest known_names{"IsForbiddenTopModule/KnownNames", [] {
    expect(is_forbidden_top_module("core"));
    expect(is_forbidden_top_module("util.x"));
    expect(!is_forbidden_top_module("myorg.foo"));
}};

// G4: per-glob flags attach to exactly the matched units, in declaration
// order; a zero-hit glob warns (not errors — cfg-gated source sets may
// legitimately leave a glob empty on some targets).
UTest per_glob_flags_attach_to_matched_units{"Scanner/PerGlobFlagsAttachToMatchedUnits", [] {
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
    expect(fatal(res.errors.empty()));
    expect(fatal(eq(res.graph.units.size(), 2u)));
    for (auto& u : res.graph.units) {
        if (u.path.filename() == "noisy.cpp") {
            expect(eq(u.packageCxxflags, (std::vector<std::string>{"-w"})));
        } else {
            // defines land before the entry's own flag lists
            expect(eq(u.packageCxxflags, (std::vector<std::string>{"-DHOT", "-mavx2"})));
        }
    }
    // The unmatched glob warns.
    bool warned = false;
    for (auto& w : res.warnings)
        if (w.message.find("nothing/**") != std::string::npos) warned = true;
    expect(bool(warned));

    std::filesystem::remove_all(dir);
}};

// #228: expand_braces desugars `{a,b}` into a cartesian product of plain
// globs. No braces -> passthrough; multiple/nested groups combine.
UTest expand_braces_desugars_cartesian_product{"Scanner/ExpandBracesDesugarsCartesianProduct", [] {
    expect(eq(expand_braces("a/**"), (std::vector<std::string>{"a/**"})));
    expect(eq(expand_braces("a/{x,y}/**"), (std::vector<std::string>{"a/x/**", "a/y/**"})));
    expect(eq(expand_braces("a/{x,y}/{1,2}"), (std::vector<std::string>{"a/x/1", "a/x/2", "a/y/1", "a/y/2"})));
    // Nested group.
    expect(eq(expand_braces("a/{x,{y,z}}/**"), (std::vector<std::string>{"a/x/**", "a/y/**", "a/z/**"})));
}};

// Review fix on #228: brace-nesting recursion depth is bounded (cap: 32
// levels), so a pathological manifest with deeply nested `{` cannot
// stack-overflow. Beyond the cap the remainder is passed through as a
// literal instead of throwing — this must simply return, not crash.
UTest expand_braces_bounds_deeply_nested_recursion{"Scanner/ExpandBracesBoundsDeeplyNestedRecursion", [] {
    std::string glob = "a/";
    constexpr int kDepth = 500;  // far beyond the 32-level cap
    for (int i = 0; i < kDepth; ++i) glob += "{";
    glob += "x";
    for (int i = 0; i < kDepth; ++i) glob += "}";

    std::vector<std::string> out;
    expect(fatal(nothrow([&] { out = expand_braces(glob); })));
    expect(!out.empty());
}};

// #228: expand_glob applies brace desugaring at its entry, so a glob like
// "p/{aac,bsf}/**" matches files under EITHER alternative directory and
// nothing else (a sibling directory outside the brace group, "opus", must
// not appear).
UTest expand_glob_brace_alternation{"Scanner/ExpandGlobBraceAlternation", [] {
    auto dir = make_tempdir("mcpp-scanner-brace");
    write(dir / "p" / "aac" / "x.c", "int x;\n");
    write(dir / "p" / "bsf" / "y.c", "int y;\n");
    write(dir / "p" / "opus" / "z.c", "int z;\n");

    auto files = expand_glob(dir, "p/{aac,bsf}/**");

    expect(fatal(eq(files.size(), 2u)));
    std::set<std::string> names;
    for (auto& f : files) names.insert(f.filename().string());
    expect(names.contains("x.c"));
    expect(names.contains("y.c"));
    expect(!names.contains("z.c"));

    std::filesystem::remove_all(dir);
}};

// #228: brace alternation in a [build].flags glob must also match — the
// per-glob-flags match point (scan_one_into's apply_glob_flags) uses
// path_matches_glob directly rather than expand_glob's walk, so it needs its
// own desugaring wire-up.
UTest per_glob_flags_match_brace_alternation{"Scanner/PerGlobFlagsMatchBraceAlternation", [] {
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
    expect(fatal(res.errors.empty()));
    expect(fatal(eq(res.graph.units.size(), 3u)));
    for (auto& u : res.graph.units) {
        bool wantsDefine = u.path.filename() != "z.cpp";
        bool hasDefine = std::find(u.packageCxxflags.begin(), u.packageCxxflags.end(),
                                    "-DCODEC") != u.packageCxxflags.end();
        expect(eq(hasDefine, wantsDefine)) << u.path.string();
    }

    std::filesystem::remove_all(dir);
}};

// G8b: relative -I flags are root-relative in the manifest but ninja runs
// with cwd = output dir — the scanner absolutizes them on every unit.
// The absolute spelling is NORMALIZED to native separators (#390): MSVC's
// path keeps the input `/` verbatim, and `-I/abs/path` written with forward
// slashes used to survive into the CDB's arguments as a mixed path.
UTest relative_include_flags_absolutized{"Scanner/RelativeIncludeFlagsAbsolutized", [] {
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
    expect(fatal(eq(res.graph.units.size(), 1u)));
    auto& fl = res.graph.units[0].packageCxxflags;
    expect(eq(fl[0], "-I" + (dir / "inc").string()));
    if constexpr (std::filesystem::path::preferred_separator == '\\')
        expect(eq(fl[1], "-I\\abs\\path"));   // absolute stays, but native
    else
        expect(eq(fl[1], "-I/abs/path"));     // absolute stays
    expect(eq(fl[2], "-DKEEP"));              // non-include untouched

    std::filesystem::remove_all(dir);
}};

// ─── mcpp#516 / #230: names the active code page cannot spell ──────────────
//
// On Windows `path::string()` converts the native (wide) name through the
// process ANSI code page and throws std::system_error when a character has no
// spelling there. OFF Windows the same call is a copy that cannot fail — so
// the defect is NOT FALSIFIABLE on Linux or macOS, and the Windows leg of CI
// is the only place the last test below means anything. It says so out loud
// (GTEST_SKIP with a reason) rather than passing vacuously.

namespace {

// Can the active code page spell this name? Asked through path's own
// conversion, i.e. the exact call the code under test makes. Doubles as the
// guard against a future runner image whose default ACP is UTF-8 (65001),
// which would otherwise quietly turn the regression test into decoration.
[[maybe_unused]] bool acp_can_spell(const std::wstring& w) {
    try { (void)std::filesystem::path(w).string(); return true; }
    catch (const std::exception&) { return false; }
}

}  // namespace

// The recorder: one unreadable subtree produces ONE record, keyed by the
// nearest ancestor that CAN be named — never by the offending name, which
// cannot be put into a message without throwing the very exception being
// reported. Platform-independent logic, so this runs everywhere.
UTest unnarrowable_paths_dedup_to_their_spellable_ancestor{"Glob/UnnarrowablePathsDedupToTheirSpellableAncestor", [] {
    (void)take_unnarrowable_paths();   // other tests in this binary walk too

    const std::filesystem::path base = "/pkg/test/www";
    note_unnarrowable_path(base / "bad" / "a.txt");
    note_unnarrowable_path(base / "bad" / "b.txt");
    note_unnarrowable_path(base / "other.txt");

    auto notes = take_unnarrowable_paths();
    expect(fatal(eq(notes.size(), 2u)));   // ".../www/bad" and ".../www" — not three files
    expect(ne(std::find(notes.begin(), notes.end(), (base / "bad").generic_string()), notes.end()));
    expect(ne(std::find(notes.begin(), notes.end(), base.generic_string()), notes.end()));

    // take() clears: the next command reports afresh rather than replaying.
    expect(take_unnarrowable_paths().empty());
}};

// A non-ASCII directory name must not disturb the walk on ANY platform. This
// one is reproducible everywhere (on Windows the UTF-8 bytes land as whatever
// the ACP makes of them, which still exercises the walk).
UTest glob_walk_handles_non_ascii_names{"Scanner/GlobWalkHandlesNonAsciiNames", [] {
    auto dir = make_tempdir("mcpp-scanner-nonascii");
    // U+65E5 U+672C U+8A9E in UTF-8, spelled as bytes so the test does not
    // depend on the source file's encoding. Split before "Dir" because a C++
    // hex escape is greedy — "\x9ED" would be one (out-of-range) escape.
    std::filesystem::create_directories(
        dir / "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "Dir");
    write(dir / "zzz_ascii" / "x.h", "#pragma once\n");

    std::vector<std::filesystem::path> dirs;
    expect(fatal(nothrow([&] { { dirs = expand_dir_glob(dir, "*"); }; })));
    expect(ne(std::find(dirs.begin(), dirs.end(), dir / "zzz_ascii"), dirs.end()));

    std::vector<std::filesystem::path> files;
    expect(fatal(nothrow([&] { { files = expand_glob(dir, "**/*.h"); }; })));
    expect(ne(std::find(files.begin(), files.end(), dir / "zzz_ascii" / "x.h"), files.end()));

    std::filesystem::remove_all(dir);
}};

// mcpp#516 proper: a directory name the ACTIVE CODE PAGE cannot spell used to
// throw out of `is_excluded_walk_dir`'s `.filename().string()` — the first
// line of the walk loop, which is why #231 hardening `path_matches_glob` (one
// line later) could never cover it. The throw escaped to main() as
//   error: internal: unhandled exception: No mapping for the Unicode
//   character exists in the target multi-byte code page.
// and exit 70.
//
// Asserts (a) the walk does not throw, (b) it is not TRUNCATED at the bad
// entry, and (c) the skip was RECORDED — silently dropping the file is the
// half-fix this whole change exists to avoid.
UTest glob_walk_survives_names_the_code_page_cannot_spell{"Scanner/GlobWalkSurvivesNamesTheCodePageCannotSpell", [] {
#ifndef _WIN32
    skip([&] { std::ostringstream text; text << "path::string() performs no encoding conversion off Windows; "
                    "mcpp#516 is not reproducible here"; return text.str(); }());
#else
    // Devanagari is in NO Windows ANSI code page, so this case stays live on
    // every non-UTF-8 ACP. The Japanese name mirrors mcpp#516's real input
    // (cpp-httplib's test/www/<CJK>Dir), but CP932/936/950 CAN spell it, so on
    // Japanese and Chinese hosts it alone would silently prove nothing.
    //
    // Built from explicit code units rather than a literal: the test must not
    // depend on the source file's encoding, nor on /utf-8 reaching the
    // compiler, nor on a checkout preserving the bytes.
    // U+0915 U+0916 U+0917 = DEVANAGARI KA KHA GA
    const std::wstring unspellable{wchar_t(0x0915), wchar_t(0x0916), wchar_t(0x0917),
                                   L'D', L'i', L'r'};
    // U+65E5 U+672C U+8A9E = the three han characters in mcpp#516's directory
    const std::wstring japanese{wchar_t(0x65E5), wchar_t(0x672C), wchar_t(0x8A9E),
                                L'D', L'i', L'r'};

    if (acp_can_spell(unspellable)) {
        skip([&] { std::ostringstream text; text << "the active code page can spell any name (UTF-8 ACP); "
                        "mcpp#516 is unreachable on this host"; return text.str(); }());
    }

    auto dir = make_tempdir("mcpp-scanner-acp");
    std::filesystem::create_directories(dir / std::filesystem::path(unspellable));
    std::filesystem::create_directories(dir / std::filesystem::path(japanese));
    // A good neighbour, to prove the walk continued past the bad entry.
    write(dir / "zzz_ascii" / "x.h", "#pragma once\n");

    (void)take_unnarrowable_paths();

    // The include-dir channel — mcpp#516's actual path (`include_dirs = {"*"}`,
    // whose literal prefix is empty, so the walk starts at the package root).
    std::vector<std::filesystem::path> dirs;
    expect(fatal(nothrow([&] { { dirs = expand_dir_glob(dir, "*"); }; })));
    expect(ne(std::find(dirs.begin(), dirs.end(), dir / "zzz_ascii"), dirs.end()))
        << "the walk was truncated at the unspellable entry";

    // The file channel — installedLayoutMatchesIndex's
    // `expand_glob(verRoot, "mcpp.toml")` walks the whole tree the same way.
    std::vector<std::filesystem::path> files;
    expect(fatal(nothrow([&] { { files = expand_glob(dir, "**/*.h"); }; })));
    expect(ne(std::find(files.begin(), files.end(), dir / "zzz_ascii" / "x.h"), files.end()));

    expect(!(take_unnarrowable_paths().empty()))
        << "the skipped entries were not recorded, so nothing would be reported";

    std::filesystem::remove_all(dir);
#endif
}};

// ─── #693: the UTF-8 spelling a path must have ─────────────────────────────
//
// build.ninja and compile_commands.json are UTF-8 text, so a path enters them
// only through a UTF-8 spelling (`try_narrow`). These state the check itself,
// and the POSIX half of the walk: a name whose bytes are not UTF-8 is skipped
// and recorded, exactly as the Windows test above states for a name the code
// page cannot spell.

UTest utf8_validity_is_decided_byte_by_byte{"Glob/Utf8ValidityIsDecidedByteByByte", [] {
    expect(is_valid_utf8(""));
    expect(is_valid_utf8("plain/ascii"));
    expect(is_valid_utf8("caf\xC3\xA9"));                  // U+00E9
    expect(is_valid_utf8("\xE6\xB5\x8B\xE8\xAF\x95"));     // U+6D4B U+8BD5
    expect(is_valid_utf8("\xF0\x9F\x98\x80"));             // U+1F600
    expect(!is_valid_utf8("caf\xE9"));                     // Latin-1
    expect(!is_valid_utf8("\xB2\xE2\xCA\xD4"));            // GBK for U+6D4B U+8BD5
    expect(!is_valid_utf8("\xC0\xAF"));                    // an overlong '/'
    expect(!is_valid_utf8("\xED\xA0\x80"));                // a surrogate
    expect(!is_valid_utf8("\xF4\x90\x80\x80"));            // above U+10FFFF
    expect(!is_valid_utf8("\xE6\xB5"));                    // truncated
    expect(!is_valid_utf8("\x80"));                        // a stray continuation
}};

// A diagnostic names a path that has no UTF-8 spelling through an escaped one.
//
// Each platform is given a name in its own native form. A Windows path is
// UTF-16, and constructing one from the bytes "caf\xE9" converts them through
// the process's ANSI code page: code page 1252 accepts them, 936 and 65001
// throw. The assertion that once followed the branches did exactly that, so
// the test passed on an English runner and failed on a Chinese one (O6 of the
// 2026-10-05 design).
UTest escaped_spelling_is_utf8_whatever_the_name{"Glob/EscapedSpellingIsUtf8WhateverTheName", [] {
#ifdef _WIN32
    const std::wstring lone{L'a', wchar_t(0xD800), L'b'};
    expect(eq(escaped_spelling(std::filesystem::path(lone)), "a\\u{D800}b"));
    expect(is_valid_utf8(escaped_spelling(std::filesystem::path(lone))));
    const std::wstring cafe{L'c', L'a', L'f', wchar_t(0x00E9)};
    expect(eq(escaped_spelling(std::filesystem::path(cafe)), "caf\xC3\xA9"));
#else
    expect(eq(escaped_spelling("/x/caf\xE9"), "/x/caf\\xE9"));
    expect(eq(escaped_spelling("/x/caf\xC3\xA9"), "/x/caf\xC3\xA9"));
    expect(is_valid_utf8(escaped_spelling(std::filesystem::path("caf\xE9"))));
#endif
}};

UTest glob_walk_skips_names_that_are_not_utf8{"Scanner/GlobWalkSkipsNamesThatAreNotUtf8", [] {
#ifdef _WIN32
    skip("a Windows name is UTF-16; the test above states the Windows case");
#else
    auto dir = make_tempdir("mcpp-scanner-latin1");
    std::error_code ec;
    std::filesystem::create_directories(dir / "caf\xE9", ec);   // Latin-1
    if (ec) {
        std::filesystem::remove_all(dir);
        skip([&] { std::ostringstream text; text << "this file system refuses a name that is not UTF-8: " << ec.message(); return text.str(); }());
    }
    write(dir / "caf\xE9" / "x.h", "#pragma once\n");
    write(dir / "zzz_ascii" / "x.h", "#pragma once\n");
    (void)take_unnarrowable_paths();

    std::vector<std::filesystem::path> files;
    expect(fatal(nothrow([&] { { files = expand_glob(dir, "**/*.h"); }; })));
    expect(eq(files, (std::vector<std::filesystem::path>{dir / "zzz_ascii" / "x.h"})));

    auto notes = take_unnarrowable_paths();
    expect(fatal(eq(notes.size(), 1u)));
    expect(eq(notes[0], dir.generic_string()));

    std::filesystem::remove_all(dir);
#endif
}};

// `module : private;` is a THIRD production, not a spelling of the two the
// scanner already knew ([module.private.frag]). It declares nothing: the unit
// still provides what its `export module` line said, and requires nothing new.
//
// It was read as an implementation partition, because `is_module_name_char`
// admits `:` and the partition test is `name.find(':') != npos`. Both spellings
// are checked because the scanner tokenises them differently — `module :
// private;` yields the "name" `:` and `module :private;` yields `:private` —
// and one of the two passing proves nothing about the other.
UTest private_module_fragment_declares_nothing{"Scanner/PrivateModuleFragmentDeclaresNothing", [] {
    for (std::string_view spelling : {"module : private;", "module :private;"}) {
        auto dir = make_tempdir("mcpp-privfrag");
        write(dir / "src" / "pf.cppm",
              std::format("export module pf;\n"
                          "export int pf();\n"
                          "{}\n"
                          "int pf() {{ return 1; }}\n", spelling));

        auto u = scan_file(dir / "src" / "pf.cppm", "pkg",
                           mcpp::builtin_extension_table());
        if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << spelling << ": " << u.error().format();
        expect(fatal(u->provides.has_value())) << spelling;
        expect(eq(u->provides->logicalName, "pf")) << spelling;
        expect(bool(u->providesInterface)) << spelling;
        // The fragment contributes no edge in either direction.
        expect(u->requires_.empty()) << spelling;
    }
}};

// A UTF-8 byte-order mark is not part of the source text. MSVC writes one by
// default, so this is ordinary input; every compiler skips it, and this scanner
// did not — the mark stayed attached to the first token, the declaration was
// not seen, and the unit silently provided nothing.
UTest byte_order_mark_does_not_hide_the_module_declaration{"Scanner/ByteOrderMarkDoesNotHideTheModuleDeclaration", [] {
    auto dir = make_tempdir("mcpp-bom");
    write(dir / "src" / "bom.cppm",
          "\xEF\xBB\xBF"
          "export module bom;\n"
          "import std;\n"
          "export int bom();\n");

    auto u = scan_file(dir / "src" / "bom.cppm", "pkg",
                       mcpp::builtin_extension_table());
    if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << u.error().format();
    expect(fatal(u->provides.has_value()))
        << "the BOM hid the module declaration";
    expect(eq(u->provides->logicalName, "bom"));
    expect(fatal(eq(u->requires_.size(), 1u)));
    expect(eq(u->requires_[0].logicalName, "std"));
}};

// UTF-16/32 is refused by name rather than misread. Every line after the mark
// would be garbage, so a named refusal is the difference between a message and
// a mystery.
UTest utf16_byte_order_mark_is_refused_by_name{"Scanner/Utf16ByteOrderMarkIsRefusedByName", [] {
    auto dir = make_tempdir("mcpp-bom16");
    write(dir / "src" / "u16.cppm", std::string_view("\xFF\xFE" "e\0x\0p\0", 8));

    auto u = scan_file(dir / "src" / "u16.cppm", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(!u.has_value()));
    if (auto&& failed = expect(ne(u.error().message.find("UTF-16"), std::string::npos)); !failed) failed << u.error().message;
}};

// The scanner must never RECORD an identity no source could have declared.
//
// `is_module_name_char` admits `:` so `M:part` scans as one token, so a
// mis-parse of the `module` line can produce a "name" that is punctuation.
// Measured before this guard existed: a file combining a BOM with a private
// module fragment recorded a module called `:`, and the build graph grew an
// edge for a BMI named after a colon. Nothing reported it — every step after
// the mis-parse worked correctly on the answer it was given.
UTest a_module_identity_that_is_not_a_name_is_refused{"Scanner/AModuleIdentityThatIsNotANameIsRefused", [] {
    auto dir = make_tempdir("mcpp-badname");
    write(dir / "src" / "bad.cppm", "module :not_private;\n");

    auto u = scan_file(dir / "src" / "bad.cppm", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(!u.has_value()));
    if (auto&& failed = expect(ne(u.error().message.find("is not a module name"), std::string::npos)); !failed) failed << u.error().message;
}};

// The message quotes WHAT WAS READ, not what was recorded.
//
// The tokeniser stops at the first character it does not accept, so a
// declaration it cannot read at all yields an EMPTY name, and `'' is not a
// module name` names nothing the author can find. A non-ASCII name is the
// measured case: `is_module_name_char` tests bytes with std::isalnum, false for
// every byte of a UTF-8 sequence. GCC 16.1 refuses the same declaration with
// `unrecognized 'MODULE-EXPORT ...'`, so this is a clearer sentence for a
// refusal that already existed, not a new restriction.
UTest an_unreadable_module_name_is_quoted_as_written{"Scanner/AnUnreadableModuleNameIsQuotedAsWritten", [] {
    auto dir = make_tempdir("mcpp-nonascii");
    write(dir / "src" / "u.cppm", "export module \u6a21\u5757;\n");

    auto u = scan_file(dir / "src" / "u.cppm", "pkg",
                       mcpp::builtin_extension_table());
    expect(fatal(!u.has_value()));
    if (auto&& failed = expect(ne(u.error().message.find("is not a module name"), std::string::npos)); !failed) failed << u.error().message;
    if (auto&& failed = expect(eq(u.error().message.find("'' is not"), std::string::npos)); !failed) failed << "the message quoted an empty name: " << u.error().message;
    if (auto&& failed = expect(ne(u.error().message.find("\u6a21\u5757"), std::string::npos)); !failed) failed << u.error().message;
}};

// The guard above must not refuse what the language allows. A partition, a
// dotted name and a dotted partition are all names.
UTest well_formed_names_survive_the_identity_guard{"Scanner/WellFormedNamesSurviveTheIdentityGuard", [] {
    struct Case { std::string_view decl; std::string_view provides; };
    for (auto [decl, provides] : {
             Case{"export module a.b.c;",        "a.b.c"},
             Case{"export module a.b:part;",     "a.b:part"},
             Case{"export module m:p.q;",        "m:p.q"},
             Case{"export module _u9;",          "_u9"}}) {
        auto dir = make_tempdir("mcpp-goodname");
        write(dir / "src" / "g.cppm", std::format("{}\n", decl));
        auto u = scan_file(dir / "src" / "g.cppm", "pkg",
                           mcpp::builtin_extension_table());
        if (auto&& failed = expect(fatal(u.has_value())); !failed) failed << decl << ": " << u.error().format();
        expect(fatal(u->provides.has_value())) << decl;
        expect(eq(u->provides->logicalName, provides)) << decl;
    }
}};

// ─── A module name is resolved in the importer's closure (mcpp#732) ─────────
//
// GCC and clang mangle a module's entities with its name and give it one
// initializer named after it, so one program cannot link two modules of one
// name, and two programs may each have one. A build holds several programs, so
// an import is resolved in the importing package's closure, and a closure may
// hold one provider of a name.

UTest one_provider_is_taken_wherever_it_is{"ModuleResolution/OneProviderIsTakenWhereverItIs", [] {
    const std::vector<std::string> providers{"lib"};
    expect(eq(choose_provider(providers, "app", {}), std::optional<std::size_t>{0}));
}};

UTest two_providers_resolve_in_the_importers_closure{"ModuleResolution/TwoProvidersResolveInTheImportersClosure", [] {
    const std::vector<std::string> providers{"app", "updater"};
    const Closures closures{{"app", {"app"}}, {"updater", {"updater"}}};
    expect(eq(choose_provider(providers, "app", closures), std::optional<std::size_t>{0}));
    expect(eq(choose_provider(providers, "updater", closures), std::optional<std::size_t>{1}));
}};

UTest two_providers_in_one_closure_or_none_resolve_to_nothing{"ModuleResolution/TwoProvidersInOneClosureOrNoneResolveToNothing", [] {
    const std::vector<std::string> providers{"lib1", "lib2"};
    const Closures closures{{"prog", {"prog", "lib1", "lib2"}}, {"other", {"other"}}};
    expect(!choose_provider(providers, "prog", closures));
    expect(!choose_provider(providers, "other", closures));
    expect(!choose_provider(providers, "prog", {}));
}};

namespace {
// Two packages under `dir`, each providing module `boost` and importing it.
std::vector<PackageRoot> two_boost_packages(const std::filesystem::path& dir) {
    std::vector<PackageRoot> out;
    for (auto name : {"app", "updater"}) {
        write(dir / name / "src" / "boost.cppm", "export module boost;\nexport int value();\n");
        write(dir / name / "src" / "use.cpp", "import boost;\nint use() { return value(); }\n");
        mcpp::manifest::Manifest m;
        m.package.name = name;
        m.modules.sources = {"src/*.cppm", "src/*.cpp"};
        out.push_back(PackageRoot{dir / name, m});
    }
    return out;
}
std::string all_errors(const ScanResult& r) {
    std::string s;
    for (auto const& e : r.errors) s += e.message + "\n";
    return s;
}
}  // namespace

UTest two_programs_each_resolve_their_own_provider{"ModuleResolution/TwoProgramsEachResolveTheirOwnProvider", [] {
    auto dir = make_tempdir("mcpp-732-two-programs");
    const Closures closures{{"app", {"app"}}, {"updater", {"updater"}}};
    auto r = scan_packages(two_boost_packages(dir), closures);
    expect(fatal(r.errors.empty())) << all_errors(r);
    expect(fatal(eq(r.graph.providersOf.at("boost").size(), 2u)));
    expect(!r.graph.producerOf.contains("boost"));
    // Every edge joins a unit to the provider of its own package.
    expect(fatal(eq(r.graph.edges.size(), 2u)));
    for (auto [consumer, producer] : r.graph.edges)
        expect(eq(r.graph.units[consumer].packageName, r.graph.units[producer].packageName));
    std::filesystem::remove_all(dir);
}};

UTest two_providers_in_one_closure_are_refused{"ModuleResolution/TwoProvidersInOneClosureAreRefused", [] {
    auto dir = make_tempdir("mcpp-732-one-closure");
    const Closures closures{{"app", {"app", "updater"}}, {"updater", {"updater"}}};
    auto r = scan_packages(two_boost_packages(dir), closures);
    const auto errors = all_errors(r);
    expect(ne(errors.find("module 'boost' is provided by package"), std::string::npos)) << errors;
    expect(ne(errors.find("closure of package 'app'"), std::string::npos)) << errors;
    std::filesystem::remove_all(dir);
}};

UTest without_closures_a_name_is_unique_in_the_graph{"ModuleResolution/WithoutClosuresANameIsUniqueInTheGraph", [] {
    auto dir = make_tempdir("mcpp-732-no-closures");
    auto r = scan_packages(two_boost_packages(dir));
    const auto errors = all_errors(r);
    expect(ne(errors.find("module 'boost' is provided by package"), std::string::npos)) << errors;
    std::filesystem::remove_all(dir);
}};

UTest one_file_reached_twice_is_named_as_such_whatever_its_spelling{"ModuleResolution/OneFileReachedTwiceIsNamedAsSuchWhateverItsSpelling", [] {
    auto dir = make_tempdir("mcpp-732-one-file");
    write(dir / "shared" / "boost.cppm", "export module boost;\n");
    std::vector<PackageRoot> packages;
    for (auto name : {"lib3", "lib4"}) {
        std::filesystem::create_directories(dir / name);
        mcpp::manifest::Manifest m;
        m.package.name = name;
        m.modules.sources = {"../shared/boost.cppm"};
        packages.push_back(PackageRoot{dir / name, m});
    }
    const Closures closures{{"lib3", {"lib3", "lib4"}}, {"lib4", {"lib4"}}};
    auto r = scan_packages(packages, closures);
    const auto errors = all_errors(r);
    expect(ne(errors.find("one file is reached as two packages"), std::string::npos)) << errors;
    std::filesystem::remove_all(dir);
}};

// A kept walk matched by the literal tail of a glob: `**/` also matches no
// directory, so `**/main.cpp` matches a `main.cpp` at the root as well as one
// below it, as the per-pattern walk did. `domain.cpp` ends with the same tail
// and is still not a `main.cpp`.
UTest a_double_star_slash_tail_matches_at_the_root{"Scanner/ADoubleStarSlashTailMatchesAtTheRoot", [] {
    auto dir = make_tempdir("mcpp-glob-tail");
    write(dir / "main.cpp", "int main() {}\n");
    write(dir / "sub" / "main.cpp", "int main() {}\n");
    write(dir / "sub" / "other.cpp", "int x;\n");
    write(dir / "sub" / "domain.cpp", "int y;\n");
    auto files = expand_glob(dir, "**/main.cpp");
    expect(fatal(eq(files.size(), 2u)));
    expect(eq(files[0], dir / "main.cpp"));
    expect(eq(files[1], dir / "sub" / "main.cpp"));
    std::filesystem::remove_all(dir);
}};

// `**/` stands for zero or more whole directories: what follows it starts a
// path segment. It used to resume at any character, so `**/main.cpp` named
// `src/domain.cpp` and `tests/**/test_*.cpp` named `tests/mytest_a.cpp`.
// A `**` not followed by `/` still crosses segments and names.
UTest a_double_star_slash_stands_for_whole_directories{"Glob/ADoubleStarSlashStandsForWholeDirectories", [] {
    expect(bool(relative_path_matches_glob("main.cpp", "**/main.cpp")));
    expect(bool(relative_path_matches_glob("src/main.cpp", "**/main.cpp")));
    expect(bool(relative_path_matches_glob("src/a/b/main.cpp", "**/main.cpp")));
    expect(!relative_path_matches_glob("domain.cpp", "**/main.cpp"));
    expect(!relative_path_matches_glob("src/domain.cpp", "**/main.cpp"));
    expect(!relative_path_matches_glob("src/a/domain.cpp", "**/main.cpp"));

    expect(bool(relative_path_matches_glob("tests/test_a.cpp", "tests/**/test_*.cpp")));
    expect(bool(relative_path_matches_glob("tests/x/test_a.cpp", "tests/**/test_*.cpp")));
    expect(!relative_path_matches_glob("tests/mytest_a.cpp", "tests/**/test_*.cpp"));
    expect(!relative_path_matches_glob("tests/x/mytest_a.cpp", "tests/**/test_*.cpp"));

    const std::string_view dnn = "**/modules/dnn/**/*.avx2.cpp";
    expect(bool(relative_path_matches_glob("modules/dnn/l.avx2.cpp", dnn)));
    expect(bool(relative_path_matches_glob("a/modules/dnn/b/l.avx2.cpp", dnn)));
    expect(!relative_path_matches_glob("a/submodules/dnn/l.avx2.cpp", dnn));
    expect(!relative_path_matches_glob("modules/xdnn/l.avx2.cpp", dnn));

    expect(bool(relative_path_matches_glob("src/a/b.cpp", "src/**")));
    expect(bool(relative_path_matches_glob("src/a/b.cpp", "src/**.cpp")));
    expect(bool(relative_path_matches_glob("src/a/b.cpp", "**")));
    expect(!relative_path_matches_glob("srcx/a.cpp", "src/**/*.cpp"));
}};
