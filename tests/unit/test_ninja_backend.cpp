
import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.source_kind;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.ninja;
import mcpp.build.plan;
import nlohmann.json;
import mcpp.manifest;
import mcpp.modgraph.scanner;   // PackageRoot, for trim_map
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpplibs.os.platform;
import mcpp.runtime.search;
import mcpp.targetside;

using namespace mcpplibs::mtest;

using namespace mcpp::build;

namespace {

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// The words of a flag string equal to `word`: a flag glued to its neighbour is
// not one of them.
std::size_t count_words(std::string_view flags, std::string_view word) {
    std::size_t count = 0;
    while (!flags.empty()) {
        const auto start = flags.find_first_not_of(' ');
        if (start == std::string_view::npos) break;
        flags.remove_prefix(start);
        const auto end = flags.find(' ');
        if (flags.substr(0, end) == word) ++count;
        flags.remove_prefix(end == std::string_view::npos ? flags.size() : end);
    }
    return count;
}

std::string escaped_include_flag(const std::filesystem::path& path) {
    auto s = path.string();
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '$' || c == ':')
            escaped.push_back('$');
        escaped.push_back(c);
    }
    return "-I" + escaped;
}

BuildPlan minimal_plan() {
    BuildPlan plan;
    plan.projectRoot = std::filesystem::temp_directory_path() / "mcpp-ninja-test";
    plan.outputDir = plan.projectRoot / "target" / "test";
    plan.manifest.package.name = "objc_rule_test";
    plan.manifest.buildConfig.cStandard = "c11";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::GCC;
    plan.toolchain.version = "test";
    plan.toolchain.binaryPath = "/usr/bin/g++";
    plan.toolchain.targetTriple = "x86_64-linux-gnu";

    // AND THE TARGET SIDE, WHICH THIS FIXTURE USED TO LEAVE DEFAULT.
    //
    // A default-constructed `TargetSide` has every layer at `Origin::None`,
    // which is not a native build — it is "nothing has been resolved". No
    // production path reaches the flag builder with one: `resolve` gives a
    // plain native build `cAbi = { Payload, … }`, from its final branch.
    //
    // The fixture got away with it while the link line was gated on
    // `kernelAbi.fromGraph() || cAbi.fromGraph()`, which is false for all-None
    // and false for a payload build alike. Asking the question the link line
    // actually has — did the C library come from a directory that existed
    // before resolution — separates them, and the fixture then described a
    // target with no C library while every assertion in this file is about one
    // that has the payload's.
    plan.targetSide.compiler = { mcpp::targetside::Origin::Payload, "gcc", "", false };
    plan.targetSide.kernelAbi = { mcpp::targetside::Origin::Payload, "linux", "", false };
    plan.targetSide.cAbi = { mcpp::targetside::Origin::Payload, "glibc", "", false };
    plan.targetSide.cxx = { mcpp::targetside::Origin::Payload, "libstdc++", "", false };
    return plan;
}

}  // namespace

UTest windows_driver_links_retain_the_selected_target_for_c_and_cxx{"NinjaBackend/WindowsDriverLinksRetainTheSelectedTargetForCAndCxx", [] {
    // This regression covers the Windows-host PE driver link branch.
    if constexpr (!mcpplibs::os::is_windows)
        skip("the PE driver link branch is selected on Windows hosts");
    for (const auto arch : {"i686", "x86_64", "aarch64"}) {
        auto plan = minimal_plan();
        plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
        plan.toolchain.binaryPath = "/selected/bin/clang++";
        plan.toolchain.targetTriple = std::string(arch) + "-pc-windows-msvc";
        plan.toolchain.crossTargetFlag = "--target=" + plan.toolchain.targetTriple;
        plan.manifest.buildConfig.cxxRuntime = "host-coupled";
        plan.targetSide.compiler.interfaceName = "llvm";
        plan.targetSide.kernelAbi.interfaceName = "windows";
        plan.targetSide.cAbi.interfaceName = "msvc";
        plan.targetSide.cxx.interfaceName = "msvc";
        auto flags = compute_flags(plan);
        expect(ne(flags.ld.find(plan.toolchain.crossTargetFlag), std::string::npos));
        expect(ne(flags.ldC.find(plan.toolchain.crossTargetFlag), std::string::npos));
    }
}};

UTest bitcode_export_tools_come_from_the_selected_llvm_and_are_quoted_as_words{"NinjaBackend/BitcodeExportToolsComeFromTheSelectedLlvmAndAreQuotedAsWords", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.toolchain.targetTriple = "x86_64-pc-windows-msvc";
    plan.toolchain.binaryPath = "/selected llvm/$installation/bin/clang++";
    LinkUnit dll;
    dll.kind = LinkUnit::SharedLibrary;
    dll.targetName = "probe";
    dll.output = "bin/probe.dll";
    dll.importLibrary = "bin/probe.lib";
    dll.defFile = "bin/probe.def";
    dll.objects = {"obj/probe.o"};
    plan.linkUnits.push_back(dll);
    auto text = emit_ninja_string(plan);
    const auto nm = plan.toolchain.binaryPath.parent_path()
        / (mcpplibs::os::is_windows ? "llvm-nm.exe" : "llvm-nm");
    expect(ne(text.find("--llvm-cxx " + ninja_command_word(plan.toolchain.binaryPath.generic_string())), std::string::npos));
    expect(ne(text.find("--llvm-nm " + ninja_command_word(nm.generic_string())), std::string::npos));
    expect(ne(text.find("--llvm-target x86_64-pc-windows-msvc"), std::string::npos));
    expect(ne(text.find("build bin/probe.def : coff_def obj/probe.o"), std::string::npos));
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath = "/native/bin/cl.exe";
    expect(eq(emit_ninja_string(plan).find("coff_args ="), std::string::npos));
}};

UTest ordinary_compilers_share_the_detached_schedule_bound{"NinjaBackend/OrdinaryCompilersShareTheDetachedScheduleBound", [] {
    auto plan = minimal_plan();
    plan.toolchain.version = "16.1.0";
    plan.scheduleTag = "detach-codegen";
    plan.scheduleCompilerCap = 2;
    plan.compileUnits.push_back({.source = "src/value.c", .kind = mcpp::SourceKind::C,
                                 .object = "obj/value.o"});
    plan.compileUnits.push_back({.source = "src/value.S", .kind = mcpp::SourceKind::GasAsm,
                                 .object = "obj/value-asm.o"});
    plan.compileUnits.push_back({.source = "src/value.asm", .kind = mcpp::SourceKind::NasmAsm,
                                 .object = "obj/value-nasm.o"});
    const auto text = emit_ninja_string(plan);
    auto rule = [&](std::string_view name) {
        const auto start = text.find("rule " + std::string(name) + "\n");
        expect(ne(start, std::string::npos)) << name;
        return start == std::string::npos ? std::string{} : text.substr(start, text.find("\n\n", start) - start);
    };
    for (const auto name : {"cxx_module", "cxx_object", "c_object", "asm_object", "asm_object_raw", "nasm_object", "cxx_scan"})
        expect(ne(rule(name).find("__compiler-cap --sem .mcpp-sched --output $out --depfile $out.d -- "), std::string::npos)) << name;
    // The generation carries the cap: the graph names no number of compilers,
    // so it is the same whatever the job count (e2e 861).
    expect(eq(text.find("--cap"), std::string::npos));
    expect(eq(text.find("sched_cap"), std::string::npos));
    expect(eq(rule("cxx_module_bmi").find("__compiler-cap"), std::string::npos))
        << "the BMI supervisor already holds a permit";
    expect(eq(rule("cxx_module_obj").find("__compiler-cap"), std::string::npos))
        << "a join must not wait for a second permit";
}};

UTest objective_c_source_uses_c_object_rule_and_c_flags{"NinjaBackend/ObjectiveCSourceUsesCObjectRuleAndCFlags", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/cocoa.m",
        .kind = mcpp::SourceKind::C,
        .object = "obj/cocoa.o",
        .packageName = "objc_rule_test",
        .packageCflags = {"-DOBJ_C_BUILD=1"},
        .packageCxxflags = {"-DWRONG_CXX_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("build obj/cocoa.o : c_object src/cocoa.m"), std::string::npos))
        << ninja;
    expect(eq(ninja.find("build obj/cocoa.o : cxx_object src/cocoa.m"), std::string::npos))
        << ninja;
    expect(ne(ninja.find("unit_cflags = -DOBJ_C_BUILD=1"), std::string::npos))
        << ninja;
    expect(eq(ninja.find("unit_cxxflags = -DWRONG_CXX_FLAG=1"), std::string::npos))
        << ninja;
}};

// mcpp#235: cxx_module/cxx_object must track header/purview/GMF includes via
// a GNU-style depfile on non-MSVC toolchains (this test's plan uses GCC on a
// non-Windows host, so posixDepfile is true). Before the fix, neither rule
// had ANY depfile outside the msvcDeps branch, so editing a file #include'd
// inside a module's purview (or a header pulled in by a .cpp) never
// invalidated the compile edge. The depfile is routed through a scratch
// `$out.d.raw` + `awk` filter (not written directly to `$out.d`) because
// GCC's `-fmodules` bolts non-standard "reversed" module rules onto the raw
// -MMD output that ninja's depfile loader rejects — see the long comment at
// the definition site for the empirically-confirmed failure mode.
UTest cxx_module_and_cxx_object_rules_track_header_deps_via_gcc_depfile{"NinjaBackend/CxxModuleAndCxxObjectRulesTrackHeaderDepsViaGccDepfile", [] {
    // Every host (the 2026-09-28 design, WS2): whether a unit emits a depfile
    // is the compiler's property. What differs by host is how GCC's filtered
    // form arrives: the awk program in the POSIX rule's shell command, and
    // `mcpp depfile-filter` wrapping the compile on Windows, which has no
    // shell to chain it in. Until 2026.9.28.2 Windows got neither.
    auto plan = minimal_plan();

    auto ninja = emit_ninja_string(plan);

    auto module_rule_start = ninja.find("rule cxx_module");
    auto object_rule_start = ninja.find("rule cxx_object");
    expect(fatal(ne(module_rule_start, std::string::npos))) << ninja;
    expect(fatal(ne(object_rule_start, std::string::npos))) << ninja;
    expect(fatal(lt(module_rule_start, object_rule_start))) << ninja;

    auto module_rule = ninja.substr(module_rule_start, object_rule_start - module_rule_start);
    auto object_rule = ninja.substr(object_rule_start);

    for (auto const& rule : {module_rule, object_rule}) {
        expect(ne(rule.find("-MMD -MF $out.d.raw"), std::string::npos)) << ninja;
        expect(ne(rule.find("deps = gcc"), std::string::npos)) << ninja;
        expect(ne(rule.find("depfile = $out.d\n"), std::string::npos)) << ninja;
        // The raw compiler depfile (with GCC's module-specific reversed
        // rules) must never be bound directly as ninja's depfile.
        expect(eq(rule.find("depfile = $out.d.raw"), std::string::npos)) << ninja;
        if constexpr (mcpplibs::os::is_windows) {
            expect(ne(rule.find("$mcpp depfile-filter --raw $out.d.raw --out $out.d -- "), std::string::npos)) << ninja;
            expect(eq(rule.find("awk"), std::string::npos)) << ninja;
        } else {
            expect(ne(rule.find("awk"), std::string::npos)) << ninja;
            expect(eq(rule.find("depfile-filter"), std::string::npos)) << ninja;
        }
    }
}};

UTest uses_package_cpp_standard_for_cxx_flags{"NinjaBackend/UsesPackageCppStandardForCxxFlags", [] {
    auto plan = minimal_plan();
    plan.manifest.package.standard = "c++26";
    plan.manifest.language.standard = "c++26";
    plan.cppStandard = "c++26";
    plan.cppStandardFlag = "-std=c++26";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "cpp26_test",
    });

    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("cxxflags  = -std=c++26"), std::string::npos))
        << ninja;
    expect(eq(ninja.find("-std=c++23"), std::string::npos))
        << ninja;
}};

UTest compile_commands_uses_same_cpp_standard{"NinjaBackend/CompileCommandsUsesSameCppStandard", [] {
    auto plan = minimal_plan();
    plan.manifest.package.standard = "c++26";
    plan.manifest.language.standard = "c++26";
    plan.cppStandard = "c++26";
    plan.cppStandardFlag = "-std=c++26";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "cpp26_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    expect(ne(cdb.find("\"-std=c++26\""), std::string::npos))
        << cdb;
    expect(eq(cdb.find("\"-std=c++23\""), std::string::npos))
        << cdb;
}};

// #690 (design record F7): the root's `[build] include_dirs` and
// `include_dirs_after` are its PRIVATE build requirement. They reach the root's
// units through their per-unit `$local_includes` and must not appear in the
// FILE-LEVEL flag strings, which every unit in the graph reads (every
// dependency's units and the std module included). Before the fix both keys
// were appended to all four strings, so a root header named like a system or
// dependency header shadowed it inside the dependency, and a dependency cache
// entry could be compiled against another project's root headers.
UTest file_level_flags_carry_no_root_include_directory{"NinjaBackend/FileLevelFlagsCarryNoRootIncludeDirectory", [] {
    auto plan = minimal_plan();
    plan.nasmPath = "/usr/bin/nasm";
    plan.manifest.buildConfig.includeDirs      = {"include", "third_party/imgui"};
    plan.manifest.buildConfig.includeDirsAfter = {"generated/inc"};

    auto flags = compute_flags(plan);

    for (auto const* channel : {&flags.cxx, &flags.cc, &flags.as, &flags.nasm}) {
        for (auto const* dir : {"include", "imgui", "generated"}) {
            if (auto&& failed = expect(eq(channel->find(dir), std::string::npos)); !failed) failed << "root include directory '" << dir
                << "' reached a file-level flag string: " << *channel;
        }
    }
}};

// What the file-level channel used to guarantee now holds on the per-unit
// one: a relative `[build] include_dirs` entry is absolutised against the
// project root and natively spelt before it becomes a unit's include
// directory (#390; the Cluster A review fix of #226/#234, whose failure shape
// was a relative `/Iinclude` under the MSVC dialect that stopped resolving
// once ninja ran from the output directory).
UTest root_include_entries_are_absolutised_for_the_unit_channel{"NinjaBackend/RootIncludeEntriesAreAbsolutisedForTheUnitChannel", [] {
    auto plan = minimal_plan();
    auto dirs = mcpp::build::expand_manifest_include_entry(
        plan.projectRoot, "third_party/imgui");
    expect(fatal(eq(dirs.size(), 1u)));
    auto expected = plan.projectRoot / "third_party" / "imgui";
    expected.make_preferred();
    expect(eq(dirs.front(), expected));
    expect(dirs.front().has_root_path()) << dirs.front();
}};

// #249: a compile unit's localIncludeDirsAfter emit as -idirafter into the
// same $local_includes variable, APPENDED after the -I entries. -idirafter
// dirs are searched after the toolchain's system dirs (gcc+clang), so a dep
// source root containing a file named like a standard header (ffmpeg's
// VERSION vs libc++'s <version> on case-insensitive macOS) can't shadow it
// while the dep's real headers stay findable. The flag's semantics — not
// its position — carry the priority; the -I-then-idirafter ordering is for
// readability.
UTest local_include_dirs_after_emit_idirafter_appended_after_dash_i{"NinjaBackend/LocalIncludeDirsAfterEmitIdirafterAppendedAfterDashI", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "after_test",
        .localIncludeDirs = {"/dep/include"},
        .localIncludeDirsAfter = {"/dep/tarball-root"},
    });

    auto ninja = emit_ninja_string(plan);

    auto line_start = ninja.find("local_includes =");
    expect(fatal(ne(line_start, std::string::npos))) << ninja;
    auto line_end = ninja.find('\n', line_start);
    auto line = ninja.substr(line_start, line_end - line_start);

    auto i_pos = line.find("-I/dep/include");
    auto after_pos = line.find("-idirafter/dep/tarball-root");
    expect(fatal(ne(i_pos, std::string::npos))) << line;
    expect(fatal(ne(after_pos, std::string::npos))) << line;
    // After-dirs come after the -I entries within $local_includes.
    expect(lt(i_pos, after_pos)) << line;
    // Never upgraded to -I.
    expect(eq(line.find("-I/dep/tarball-root"), std::string::npos)) << line;
}};

// #249 MSVC degradation: cl.exe has no -idirafter, so under the msvc
// dialect after-dirs are emitted as regular /I at the END of the include
// list (clang targeting MSVC uses the gnu dialect and gets -idirafter).
UTest msvc_dialect_emits_include_dirs_after_as_trailing_slash_i{"NinjaBackend/MsvcDialectEmitsIncludeDirsAfterAsTrailingSlashI", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath = "cl.exe";
    plan.toolchain.targetTriple = "x86_64-pc-windows-msvc";
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "after_test",
        .localIncludeDirs = {"/dep/include"},
        .localIncludeDirsAfter = {"/dep/tarball-root"},
    });

    auto ninja = emit_ninja_string(plan);

    auto line_start = ninja.find("local_includes =");
    expect(fatal(ne(line_start, std::string::npos))) << ninja;
    auto line_end = ninja.find('\n', line_start);
    auto line = ninja.substr(line_start, line_end - line_start);

    // Both halves take the dialect's prefix. The plain half used to hardcode
    // `-I` even here — cl.exe accepts it, so it never broke anything, but it
    // meant local_include_flags derived the prefix twice and only agreed with
    // the dialect on one of them. Converging both on include_token fixed it.
    auto i_pos = line.find("/I/dep/include");
    auto after_pos = line.find("/I/dep/tarball-root");
    expect(fatal(ne(i_pos, std::string::npos))) << line;
    expect(fatal(ne(after_pos, std::string::npos))) << line;
    expect(lt(i_pos, after_pos)) << line;
    expect(eq(line.find("-idirafter"), std::string::npos)) << line;
    expect(eq(line.find("-I/dep"), std::string::npos)) << line;
}};

// #331: the per-TU include channel applied only ninja's `$` escaping, while
// the global channel in flags.cppm shell-quoted. Same manifest include_dirs,
// two derivations — so a directory with a space in it survived one path and
// split into separate shell words on the other, which is what every Windows
// user hits the moment a dependency lands under `C:\Program Files`.
UTest local_include_dirs_with_spaces_are_shell_quoted{"NinjaBackend/LocalIncludeDirsWithSpacesAreShellQuoted", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "spaced",
        .localIncludeDirs = {"/opt/my dep/include"},
        .localIncludeDirsAfter = {"/opt/my dep/after"},
    });

    auto ninja = emit_ninja_string(plan);
    auto line_start = ninja.find("local_includes =");
    expect(fatal(ne(line_start, std::string::npos))) << ninja;
    auto line = ninja.substr(line_start, ninja.find('\n', line_start) - line_start);

    // Two escaping layers, in order: ninja's (`$ ` for a literal space, so
    // ninja does not treat it as a field separator) and then the shell's
    // (quotes, so what ninja hands to sh stays one word). The old code had
    // only the first, which is why the path survived ninja and then split in
    // the shell. The prefix must be INSIDE the quotes — quoting the path
    // alone would leave `-I` as its own word and reintroduce the split.
    //
    // The quote character is the host shell's, not a fixed one: POSIX sh
    // wants single quotes, cmd.exe double. Hardcoding `'` passed on Linux
    // and failed on the Windows runner for a difference that is correct.
    const std::string q = mcpplibs::os::is_windows ? "\"" : "'";
    expect(ne(line.find(q + "-I/opt/my$ dep/include" + q), std::string::npos)) << line;
    expect(ne(line.find(q + "-idirafter/opt/my$ dep/after" + q), std::string::npos)) << line;
}};

// #261: on Windows $local_includes is copied into a RESPONSE FILE, which the
// drivers tokenize GNU-style — a backslash is an escape character there, and
// quoting does not exempt it. A native-separator token like C:\src\inc loses
// its separators and every dependency header goes missing, with nothing in
// the error pointing at the include flag.
//
// The separator distinction is real only on Windows: POSIX treats '\' as an
// ordinary filename character, so generic_string() leaves it alone and this
// assertion cannot be made from a Linux host. Guarded rather than weakened —
// a test that passes for the wrong reason everywhere is worse than one that
// says where it applies. Windows CI is the enforcement point.
UTest local_include_tokens_use_generic_separators{"NinjaBackend/LocalIncludeTokensUseGenericSeparators", [] {
    const auto& gnu = mcpp::toolchain::gnu_dialect();
    auto sub = std::filesystem::path("src") / "inc";
    auto tok = mcpp::build::include_token(gnu, sub, {},
                                          mcpp::build::PathForm::Generic);
    // True on every host: the generic form never uses the native separator.
    expect(ne(tok.find("src/inc"), std::string::npos)) << tok;

    if constexpr (mcpplibs::os::is_windows) {
        auto abs = mcpp::build::include_token(
            gnu, std::filesystem::path("C:\\src\\inc"), {},
            mcpp::build::PathForm::Generic);
        expect(eq(abs.find('\\'), std::string::npos)) << abs;

        // The command-line channel keeps native separators — a backslash is
        // just a character there, and rewriting those paths would be a change
        // nobody asked for.
        auto native = mcpp::build::include_token(
            gnu, std::filesystem::path("C:\\src\\inc"), {},
            mcpp::build::PathForm::Native);
        expect(ne(native.find('\\'), std::string::npos)) << native;
    }
}};

// #249 NASM degradation: nasm_object edges share $local_includes, but NASM
// would parse `-idirafter<p>` as its `-i` option with value `dirafter<p>` —
// a silently wrong search dir. Only the C/C++ frontends have a system-header
// chain to protect, so a nasm unit's after-dirs degrade to plain -I.
UTest nasm_unit_emits_include_dirs_after_as_plain_dash_i{"NinjaBackend/NasmUnitEmitsIncludeDirsAfterAsPlainDashI", [] {
    auto plan = minimal_plan();
    plan.nasmPath = "/usr/bin/nasm";
    plan.compileUnits.push_back({
        .source = "src/scale.asm",
        .kind = mcpp::SourceKind::NasmAsm,
        .object = "obj/scale.asm.o",
        .packageName = "after_test",
        .localIncludeDirs = {"/dep/x86"},
        .localIncludeDirsAfter = {"/dep/tarball-root"},
    });

    auto ninja = emit_ninja_string(plan);

    auto edge_start = ninja.find("build obj/scale.asm.o : nasm_object");
    expect(fatal(ne(edge_start, std::string::npos))) << ninja;
    auto line_start = ninja.find("local_includes =", edge_start);
    expect(fatal(ne(line_start, std::string::npos))) << ninja;
    auto line = ninja.substr(line_start, ninja.find('\n', line_start) - line_start);

    expect(ne(line.find("-I/dep/x86"), std::string::npos)) << line;
    expect(ne(line.find("-I/dep/tarball-root"), std::string::npos)) << line;
    expect(eq(line.find("-idirafter"), std::string::npos)) << line;
}};

// ── assembly sources (.S/.s → asm_object via $cc, .asm → nasm_object) ────────

UTest gas_source_uses_asm_object_rule{"NinjaBackend/GasSourceUsesAsmObjectRule", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/copy.S",
        .kind = mcpp::SourceKind::GasAsm,
        .object = "obj/copy.S.o",
        .packageName = "asm_rule_test",
        // Only the -D/-U/-I subset may reach the assembler: -std/-O/-w on an
        // asm command line are driver noise (or errors).
        .packageCflags = {"-DHAVE_ASM=1", "-std=c99", "-O2", "-w"},
        .packageCxxflags = {"-DWRONG_CXX_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("rule asm_object"), std::string::npos)) << ninja;
    expect(ne(ninja.find("build obj/copy.S.o : asm_object src/copy.S"), std::string::npos)) << ninja;
    expect(ne(ninja.find("cc        = "), std::string::npos)) << ninja;
    expect(ne(ninja.find("unit_asmflags = -DHAVE_ASM=1\n"), std::string::npos))
        << ninja;
    expect(eq(ninja.find("unit_asmflags = -DHAVE_ASM=1 -std=c99"), std::string::npos))
        << ninja;
    // The global asm flag string must not carry a C standard or opt level.
    auto asmline_pos = ninja.find("asmflags  =");
    expect(fatal(ne(asmline_pos, std::string::npos))) << ninja;
    auto asmline = ninja.substr(asmline_pos, ninja.find('\n', asmline_pos) - asmline_pos);
    expect(eq(asmline.find("-std="), std::string::npos)) << asmline;
    expect(eq(asmline.find("-O"), std::string::npos)) << asmline;
}};

UTest nasm_source_uses_nasm_rule_with_derived_format{"NinjaBackend/NasmSourceUsesNasmRuleWithDerivedFormat", [] {
    auto plan = minimal_plan();
    plan.nasmPath = "/opt/bin/nasm";
    plan.nasmFormat = "elf64";
    plan.compileUnits.push_back({
        .source = "src/simd.asm",
        .kind = mcpp::SourceKind::NasmAsm,
        .object = "obj/simd.asm.o",
        .packageName = "nasm_rule_test",
        .packageCflags = {"-DHAVE_AVX2=1", "-O2"},
    });

    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("rule nasm_object"), std::string::npos)) << ninja;
    expect(ne(ninja.find("nasm      = /opt/bin/nasm"), std::string::npos)) << ninja;
    expect(ne(ninja.find("nasmfmt   = elf64"), std::string::npos)) << ninja;
    expect(ne(ninja.find("build obj/simd.asm.o : nasm_object src/simd.asm"), std::string::npos)) << ninja;
    expect(ne(ninja.find("unit_asmflags = -DHAVE_AVX2=1\n"), std::string::npos))
        << ninja;
}};

UTest no_asm_rules_without_asm_sources{"NinjaBackend/NoAsmRulesWithoutAsmSources", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "plain_test",
    });

    auto ninja = emit_ninja_string(plan);

    expect(eq(ninja.find("rule asm_object"), std::string::npos)) << ninja;
    expect(eq(ninja.find("rule nasm_object"), std::string::npos)) << ninja;
}};

UTest compile_commands_skip_nasm_and_cover_gas{"NinjaBackend/CompileCommandsSkipNasmAndCoverGas", [] {
    auto plan = minimal_plan();
    plan.nasmPath = "/opt/bin/nasm";
    plan.nasmFormat = "elf64";
    plan.compileUnits.push_back({
        .source = "src/simd.asm",
        .kind = mcpp::SourceKind::NasmAsm,
        .object = "obj/simd.asm.o",
        .packageName = "cdb_test",
    });
    plan.compileUnits.push_back({
        .source = "src/copy.S",
        .kind = mcpp::SourceKind::GasAsm,
        .object = "obj/copy.S.o",
        .packageName = "cdb_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    // NASM command lines are meaningless to CDB consumers (clangd) — excluded.
    expect(eq(cdb.find("simd.asm"), std::string::npos)) << cdb;
    // GAS units ride the C driver and stay in the CDB.
    expect(ne(cdb.find("copy.S"), std::string::npos)) << cdb;
    expect(eq(cdb.find("\"-std=c11\""), std::string::npos)) << cdb;   // asm-safe flags, no C std
}};

// mcpp#234: each packageCflags/packageCxxflags element is already one argv
// token — apply_glob_flags pushes a define like `T=long long` as the single
// element `-DT=long long`. join_flags previously joined tokens with a bare
// space and zero quoting, so once ninja resolved the command line and handed
// it to the shell, the embedded space split `-DT=long long` into TWO words
// (`-DT=long` and a bare `long`). The emitted unit_cflags line must carry the
// define as a single shell-quoted token.
UTest quotes_flag_value_with_space{"NinjaBackend/QuotesFlagValueWithSpace", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/main.o",
        .packageName = "quote_test",
        .packageCflags = {"-DT=long long"},
    });

    auto ninja = emit_ninja_string(plan);

    // shell_quote_arg wraps in single quotes on POSIX, double quotes on
    // Windows — assert the platform-appropriate spelling.
    const std::string quoted = mcpplibs::os::is_windows
        ? "unit_cflags = \"-DT=long long\""
        : "unit_cflags = '-DT=long long'";
    expect(ne(ninja.find(quoted), std::string::npos)) << ninja;
    // Must NOT appear as two bare, unquoted words split on the space.
    expect(eq(ninja.find("unit_cflags = -DT=long long"), std::string::npos))
        << ninja;
}};

// Plain framework-shaped flags with nothing shell-significant must pass
// through byte-for-byte unquoted (no over-quoting regression).
UTest plain_flags_pass_through_unquoted{"NinjaBackend/PlainFlagsPassThroughUnquoted", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "plain_flag_test",
        .packageCxxflags = {"-DFOO=1", "-O2"},
    });

    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("unit_cxxflags = -DFOO=1 -O2"), std::string::npos))
        << ninja;
}};

// The SubOS farm must reach DT_RPATH AFTER the artifact's own directory.
//
// It did not, and the composition is why: the farm was appended to the GLOBAL
// `$ldflags` while `$ORIGIN` rides the PER-UNIT `$unit_ldflags`, and every link
// rule renders `$ldflags $unit_ldflags`. Each site read correctly on its own —
// flags.cppm even commented "so it is LAST" — and the artifact loaded a
// different build of libX11 than it had just been linked against.
//
// The assertion is on the EFFECTIVE line (global then unit, exactly as the
// rule expands it), not on either variable alone: checking one of them is what
// made the original mistake invisible.
UTest subos_farm_rpath_follows_the_artifacts_own_directory{"NinjaBackend/SubosFarmRpathFollowsTheArtifactsOwnDirectory", [] {
    if constexpr (!mcpplibs::os::is_linux)
        skip("the SubOS farm rpath is emitted for ELF targets only");

    const std::string farm = "/tmp/mcpp-ninja-test-farm/subos/default/lib";
    auto plan = minimal_plan();
    plan.runtimeSearch.push_back(
        {farm, mcpp::runtime::search::Origin::SubosFarm});
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "farm_order_test",
    });
    plan.linkUnits.push_back({
        .targetName = "app",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/main.o"},
        // What `shared_library_link_flags` produces for a shared-lib consumer.
        .linkFlags = {"-Lbin", "-Wl,-rpath,'$$ORIGIN'", "-lgreet"},
        .output = "bin/app",
        .entryMain = "src/main.cpp",
    });

    auto ninja = emit_ninja_string(plan);

    auto line_after = [&](std::string_view prefix) -> std::string {
        auto at = ninja.find(prefix);
        if (at == std::string::npos) return {};
        at += prefix.size();
        return ninja.substr(at, ninja.find('\n', at) - at);
    };
    // `$cxx $in -o $out $ldflags $unit_ldflags` — reproduce that expansion.
    const std::string effective =
        line_after("\nldflags   =") + " " + line_after("\n  unit_ldflags =");

    auto origin = effective.find("$$ORIGIN");
    auto fallback = effective.find(farm);
    expect(fatal(ne(origin, std::string::npos)))   << effective;
    expect(fatal(ne(fallback, std::string::npos))) << effective;
    expect(lt(origin, fallback))
        << "the SubOS farm outranks $ORIGIN, so the artifact can load a "
           "different build of a library than it linked against:\n" << effective;

    // And it must have left the global channel entirely — leaving a copy there
    // would restore the old order no matter what the unit tail says.
    expect(eq(line_after("\nldflags   =").find(farm), std::string::npos)) << ninja;
}};

// An archive is produced by `ar`, whose rule never expands `$unit_ldflags`.
// Emitting run-time search flags there would be dead bytes in every graph.
UTest static_library_carries_no_runtime_fallback{"NinjaBackend/StaticLibraryCarriesNoRuntimeFallback", [] {
    if constexpr (!mcpplibs::os::is_linux)
        skip("the SubOS farm rpath is emitted for ELF targets only");

    const std::string farm = "/tmp/mcpp-ninja-test-farm/subos/default/lib";
    auto plan = minimal_plan();
    plan.runtimeSearch.push_back(
        {farm, mcpp::runtime::search::Origin::SubosFarm});
    plan.compileUnits.push_back({
        .source = "src/lib.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/lib.o",
        .packageName = "farm_order_test",
    });
    plan.linkUnits.push_back({
        .targetName = "greet",
        .kind = mcpp::build::LinkUnit::StaticLibrary,
        .objects = {"obj/lib.o"},
        .output = "lib/libgreet.a",
    });

    auto ninja = emit_ninja_string(plan);
    auto at = ninja.find("build lib/libgreet.a");
    expect(fatal(ne(at, std::string::npos))) << ninja;
    auto stanza = ninja.substr(at, ninja.find("\nbuild ", at + 1) - at);
    expect(eq(stanza.find(farm), std::string::npos)) << stanza;
}};

// Regression: mcpp-GENERATED per-unit LINK flags are already correctly
// shell-quoted + ninja-escaped at construction — e.g. the shared-dep rpath
// token `-Wl,-rpath,'$$ORIGIN'` (single quotes stop shell $-expansion, `$$`
// is ninja's literal `$`). join_flags for link flags must NOT re-run
// shell_quote_arg over them: doing so double-quotes the token, baking a
// literal `'$ORIGIN'` (quotes included) into the binary's RUNPATH so the
// dynamic linker can't find dependency .so's next to the exe (e2e 55-57/64).
UTest link_flags_are_not_re_quoted{"NinjaBackend/LinkFlagsAreNotReQuoted", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "rpath_test",
    });
    plan.linkUnits.push_back({
        .targetName = "app",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/main.o"},
        .linkFlags = {"-Wl,-rpath,'$$ORIGIN'"},
        .output = "bin/app",
        .entryMain = "src/main.cpp",
    });

    auto ninja = emit_ninja_string(plan);

    // The rpath token passes through verbatim (ninja `$$` = literal `$`).
    expect(ne(ninja.find("-Wl,-rpath,'$$ORIGIN'"), std::string::npos)) << ninja;
    // Must NOT be double-quoted: shell_quote_arg escaping an embedded `'`
    // produces the `'\''` sequence, which only appears if it re-quoted.
    expect(eq(ninja.find("'\\''"), std::string::npos)) << ninja;
}};

// Regression (#234 follow-up): a raw descriptor flag that packs two argv
// tokens into one string — e.g. compat.lua's `-include <header>` — must pass
// through VERBATIM so the shell splits it back into `-include` + the header.
// Blanket shell-quoting wrapped it into one malformed arg, so gcc looked for a
// file literally named "<space>header" → "No such file" → aarch64/macos/windows
// cross-builds failed. Only `-D`/`/D` define tokens with an intra-value space
// get quoted; `-include foo.h` does not.
UTest raw_multi_token_flag_is_not_quoted{"NinjaBackend/RawMultiTokenFlagIsNotQuoted", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/main.o",
        .packageName = "include_flag_test",
        .packageCflags = {"-include mcpp_lua_platform_config.h"},
    });

    auto ninja = emit_ninja_string(plan);

    // Verbatim, so the shell re-splits into two args.
    expect(ne(ninja.find("unit_cflags = -include mcpp_lua_platform_config.h"), std::string::npos)) << ninja;
    // Must NOT be wrapped in quotes (which would make it one malformed arg).
    expect(eq(ninja.find("'-include mcpp_lua_platform_config.h'"), std::string::npos)) << ninja;
}};

// mcpp#247 + #344: link/archive/shared route the object list through a response
// file on EVERY platform. The number of objects on one link edge is unbounded —
// an ffmpeg/opencv-class source package contributes thousands — and every way of
// spawning a command has a ceiling:
//
//   Windows  CreateProcess, 32 KiB command line
//   POSIX    ninja runs `sh -c "<whole command>"`, so the command is a SINGLE
//            argv entry and hits MAX_ARG_STRLEN (32 pages, 128 KiB) long before
//            the 2 MiB ARG_MAX anyone would think to check
//
// This test used to assert the OPPOSITE for POSIX ("ARG_MAX is ample"), pinning
// an assumption that was both about the wrong limit and false: mcpp-index's
// opencv-module link line was already 56 840 bytes inline, and #344's per-package
// object directories took it to 161 687 — at which point ninja dies with
// `posix_spawn: Argument list too long`, naming no edge and no cause. A ceiling
// nothing watches is not a ceiling anyone can stay under, so there isn't one now.
UTest driver_style_link_rules_always_use_rspfile{"NinjaBackend/DriverStyleLinkRulesAlwaysUseRspfile", [] {
    auto plan = minimal_plan();  // GCC → gnu dialect → driver-style branch

    auto ninja = emit_ninja_string(plan);

    for (std::string_view rule : {"rule cxx_link\n", "rule cxx_archive\n",
                                  "rule cxx_shared\n"}) {
        auto start = ninja.find(rule);
        expect(fatal(ne(start, std::string::npos))) << ninja;
        auto end = ninja.find("\n\n", start);
        expect(fatal(ne(end, std::string::npos))) << ninja;
        auto body = ninja.substr(start, end - start);
        expect(ne(body.find("@$out.rsp"), std::string::npos)) << body;
        expect(ne(body.find("rspfile = $out.rsp"), std::string::npos)) << body;
        expect(ne(body.find("rspfile_content = $in"), std::string::npos)) << body;
        // And the object list must no longer be inlined into the command: that
        // is the whole point, so `$in` may appear only as rspfile_content.
        expect(eq(count_occurrences(body, "$in"), 1u)) << body;
    }
}};

UTest root_package_cxxflags_are_emitted_once_per_unit{"NinjaBackend/RootPackageCxxflagsAreEmittedOncePerUnit", [] {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.cxxflags = {"-DROOT_FLAG=1"};
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "root_flag_test",
        .packageCxxflags = {"-DROOT_FLAG=1"},
    });

    auto ninja = emit_ninja_string(plan);

    expect(eq(count_occurrences(ninja, "unit_cxxflags = -DROOT_FLAG=1"), 2u))
        << ninja;
    expect(eq(ninja.find("cxxflags  = -std=c++23 -O2 -DROOT_FLAG=1"), std::string::npos))
        << ninja;
}};

// mcpp#261: the clang scan rule used shell redirection (`> $out`), which on
// Windows forced a `cmd /c` wrapper and with it cmd.exe's 8191-char command
// line ceiling — a quarter of what ninja's CreateProcess path allows. Any
// package with a large include-dir list overran it. clang-scan-deps has -o
// (LLVM 17+), so the redirect and the wrapper are both unnecessary.
UTest clang_scan_rule_writes_via_dash_o_without_shell_redirection{"NinjaBackend/ClangScanRuleWritesViaDashOWithoutShellRedirection", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.toolchain.binaryPath = "/usr/bin/clang++";
    plan.scanDepsPath = "/usr/bin/clang-scan-deps";
    plan.compileUnits.push_back({
        .source = "src/m.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/m.o",
        .packageName = "objc_rule_test",
        .providesModule = "m",
    });

    auto ninja = emit_ninja_string(plan);

    auto scanRule = ninja.find("rule cxx_scan");
    expect(fatal(ne(scanRule, std::string::npos))) << ninja;
    auto scanEnd = ninja.find("description = SCAN", scanRule);
    expect(fatal(ne(scanEnd, std::string::npos))) << ninja;
    auto rule = ninja.substr(scanRule, scanEnd - scanRule);

    expect(ne(rule.find("-format=p1689 -o $out --"), std::string::npos)) << rule;
    expect(eq(rule.find("> $out"), std::string::npos))
        << "scan rule must not use shell redirection: " << rule;
}};

// The `cmd /c` wrapper was the only place mcpp put a shell between ninja and
// a compiler invocation. Keep it gone: it is what re-imposes the 8191 ceiling.
UTest no_rule_wraps_its_command_in_cmd_slash_c{"NinjaBackend/NoRuleWrapsItsCommandInCmdSlashC", [] {
    for (auto compiler : {mcpp::toolchain::CompilerId::GCC,
                          mcpp::toolchain::CompilerId::Clang}) {
        auto plan = minimal_plan();
        plan.toolchain.compiler = compiler;
        if (compiler == mcpp::toolchain::CompilerId::Clang) {
            plan.toolchain.binaryPath = "/usr/bin/clang++";
            plan.scanDepsPath = "/usr/bin/clang-scan-deps";
        }
        plan.compileUnits.push_back({
            .source = "src/m.cppm",
            .kind = mcpp::SourceKind::ModuleInterface,
            .object = "obj/m.o",
            .packageName = "objc_rule_test",
            .providesModule = "m",
        });

        auto ninja = emit_ninja_string(plan);
        expect(eq(ninja.find("cmd /c"), std::string::npos))
            << "compiler=" << static_cast<int>(compiler) << "\n" << ninja;
    }
}};

// mcpp#261: the compile and scan rules carry an UNBOUNDED flag payload —
// one -I per dependency include dir — and on Windows ninja spawns through
// CreateProcess (32767-char ceiling). They now route that payload through a
// response file, the same mitigation #247 gave the link rules for the same
// reason. Reachable from a POSIX test host via the msvc dialect, which only
// ever runs on Windows. NASM is excluded on purpose (it spells response
// files `-@ file`), and POSIX keeps the inline form byte-identical.
UTest compile_and_scan_rules_route_flags_through_rspfile_under_msvc_dialect{"NinjaBackend/CompileAndScanRulesRouteFlagsThroughRspfileUnderMsvcDialect", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath = "cl.exe";
    plan.compileUnits.push_back({
        .source = "src/m.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/m.o",
        .packageName = "objc_rule_test",
        .providesModule = "m",
    });
    plan.compileUnits.push_back({
        .source = "src/a.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/a.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);

    // cxx_module and cxx_object pick their Windows shape through
    // `if constexpr (is_windows)`, so their response-file form is only
    // reachable on a Windows host; c_object and cxx_scan have no such
    // compile-time branch and are assertable everywhere.
    std::vector<std::string_view> rules{"rule c_object\n", "rule cxx_scan\n"};
    if constexpr (mcpplibs::os::is_windows) {
        rules.push_back("rule cxx_module\n");
        rules.push_back("rule cxx_object\n");
    }
    for (std::string_view rule : rules) {
        auto start = ninja.find(rule);
        expect(fatal(ne(start, std::string::npos))) << rule << "\n" << ninja;
        auto end = ninja.find("\n\n", start);
        expect(fatal(ne(end, std::string::npos))) << ninja;
        auto body = ninja.substr(start, end - start);

        expect(ne(body.find("@$out.rsp"), std::string::npos)) << body;
        expect(ne(body.find("rspfile = $out.rsp"), std::string::npos)) << body;
        // ONLY $local_includes may live in the response file: its content
        // is tokenized GNU-style (backslash = escape), and those are the
        // only paths this file forward-slashes itself. $cxxflags carries
        // native-separated paths from flags.cppm and must stay inline —
        // routing it through the rsp ate the separators of the std.pcm path
        // and broke every `import std;` on Windows. The byte order mark in
        // front is how cl.exe learns the file is UTF-8 (#693).
        expect(ne(body.find("rspfile_content = \xEF\xBB\xBF $local_includes\n"), std::string::npos)) << body;
        // The payload must not ALSO remain inline, or the ceiling stands.
        auto cmdStart = body.find("command = ");
        auto cmdEnd = body.find('\n', cmdStart);
        auto cmd = body.substr(cmdStart, cmdEnd - cmdStart);
        expect(eq(cmd.find("$local_includes"), std::string::npos)) << cmd;
        // ...and the flags that must NOT move off the command line stay there.
        if (rule != "rule c_object\n")
            expect(ne(cmd.find("$cxxflags"), std::string::npos)) << cmd;
        else
            expect(ne(cmd.find("$cflags"), std::string::npos)) << cmd;
    }
}};

// POSIX must keep the inline form: ARG_MAX is ample and an inline command is
// far easier to re-run by hand. This is the byte-identity guard for the
// #261 change on the platform where it must be a no-op.
UTest compile_rules_stay_inline_on_posix_drivers{"NinjaBackend/CompileRulesStayInlineOnPosixDrivers", [] {
    if constexpr (mcpplibs::os::is_windows) {
        skip("inline form is Windows-exempt by design");
    } else {
        auto plan = minimal_plan();  // GCC → gnu dialect, non-msvc deps
        plan.compileUnits.push_back({
            .source = "src/a.c",
            .kind = mcpp::SourceKind::C,
            .object = "obj/a.o",
            .packageName = "objc_rule_test",
        });

        auto ninja = emit_ninja_string(plan);

        for (std::string_view rule : {"rule cxx_module\n", "rule cxx_object\n",
                                      "rule c_object\n"}) {
            auto start = ninja.find(rule);
            expect(fatal(ne(start, std::string::npos))) << rule << "\n" << ninja;
            auto end = ninja.find("\n\n", start);
            expect(fatal(ne(end, std::string::npos))) << ninja;
            auto body = ninja.substr(start, end - start);
            expect(eq(body.find("rspfile"), std::string::npos)) << body;
            expect(ne(body.find("$local_includes"), std::string::npos)) << body;
        }
    }
}};

// mcpp#257: "emit a depfile" and "strip GCC's reversed module rules" are two
// decisions; 0.0.97 conflated them and left Clang with no include tracking
// at all. Clang emits a single plain make rule (measured on 20.1.7/22.1.8),
// so it takes the depfile WITHOUT the awk filter, writing -MF straight to
// $out.d.
UTest clang_gets_depfile_without_the_gcc_module_rule_filter{"NinjaBackend/ClangGetsDepfileWithoutTheGccModuleRuleFilter", [] {
    // Every host: the clang++ row on Windows (the default Windows row since
    // #718) had no depfile until 2026.9.28.2 (WS2).
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.toolchain.binaryPath = "/usr/bin/clang++";

    auto ninja = emit_ninja_string(plan);

    auto module_rule_start = ninja.find("rule cxx_module");
    auto object_rule_start = ninja.find("rule cxx_object");
    expect(fatal(ne(module_rule_start, std::string::npos))) << ninja;
    expect(fatal(ne(object_rule_start, std::string::npos))) << ninja;
    auto module_rule = ninja.substr(module_rule_start,
                                    object_rule_start - module_rule_start);

    expect(ne(module_rule.find("-MMD -MF $out.d"), std::string::npos)) << ninja;
    expect(ne(module_rule.find("deps = gcc"), std::string::npos)) << ninja;
    expect(ne(module_rule.find("depfile = $out.d\n"), std::string::npos)) << ninja;
    // No scratch file and no filter: there is nothing to strip.
    expect(eq(module_rule.find("$out.d.raw"), std::string::npos)) << ninja;
    expect(eq(module_rule.find("awk"), std::string::npos)) << ninja;
    expect(eq(module_rule.find("depfile-filter"), std::string::npos)) << ninja;
}};

// The other half of the same asymmetry: C and GAS edges include headers too
// and had no depfile on ANY toolchain.
UTest c_and_asm_rules_also_track_header_deps{"NinjaBackend/CAndAsmRulesAlsoTrackHeaderDeps", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/a.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/a.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.push_back({
        .source = "src/b.S",
        .kind = mcpp::SourceKind::GasAsm,
        .object = "obj/b.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);

    for (std::string_view rule : {"rule c_object\n", "rule asm_object\n"}) {
        // NOTE: asm_object is the `.S` rule. `.s` uses asm_object_raw and
        // deliberately has no depfile — see below.
        auto start = ninja.find(rule);
        expect(fatal(ne(start, std::string::npos))) << rule << "\n" << ninja;
        auto end = ninja.find("\n\n", start);
        expect(fatal(ne(end, std::string::npos))) << ninja;
        auto body = ninja.substr(start, end - start);
        expect(ne(body.find("-MMD -MF $out.d"), std::string::npos)) << body;
        expect(ne(body.find("deps = gcc"), std::string::npos)) << body;
        expect(ne(body.find("depfile = $out.d"), std::string::npos)) << body;
        // C/GAS units never carry module reversed-rules — no filter, and so
        // no scratch file to bind by mistake.
        expect(eq(body.find("$out.d.raw"), std::string::npos)) << body;
        expect(eq(body.find("awk"), std::string::npos)) << body;
    }
}};

// The C driver preprocesses `.S` but not `.s`, so only `.S` can produce a
// depfile. Asking anyway makes clang emit "argument unused during
// compilation: '-MMD'" for every such file and write nothing, and ninja's
// `deps = gcc` treats an absent depfile as an error — so the two cases need
// separate rules.
UTest lowercase_asm_has_no_depfile_and_its_own_rule{"NinjaBackend/LowercaseAsmHasNoDepfileAndItsOwnRule", [] {
    if constexpr (mcpplibs::os::is_windows)
        skip("POSIX depfile shape only");

    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/upper.S",
        .kind = mcpp::SourceKind::GasAsm,
        .object = "obj/upper.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.push_back({
        .source = "src/lower.s",
        .kind = mcpp::SourceKind::GasAsm,
        .object = "obj/lower.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);

    auto body_of = [&](std::string_view rule) {
        auto start = ninja.find(rule);
        expect(ne(start, std::string::npos)) << rule << "\n" << ninja;
        auto end = ninja.find("\n\n", start);
        return ninja.substr(start, end - start);
    };

    auto upper = body_of("rule asm_object\n");
    expect(ne(upper.find("-MMD -MF $out.d"), std::string::npos)) << upper;
    expect(ne(upper.find("depfile = $out.d"), std::string::npos)) << upper;

    auto lower = body_of("rule asm_object_raw\n");
    expect(eq(lower.find("-MMD"), std::string::npos)) << lower;
    expect(eq(lower.find("depfile"), std::string::npos)) << lower;

    // And the edges must be routed to the matching rule.
    expect(ne(ninja.find("build obj/upper.o : asm_object src/upper.S"), std::string::npos)) << ninja;
    expect(ne(ninja.find("build obj/lower.o : asm_object_raw src/lower.s"), std::string::npos)) << ninja;
}};

// ── #311: staging goes through `mcpp stage`, never an in-place shell copy ──

UTest stage_rule_runs_through_mcpp_with_restat{"NinjaBackend/StageRuleRunsThroughMcppWithRestat", [] {
    auto plan = minimal_plan();
    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("rule stage_file\n"), std::string::npos)) << ninja;
    expect(ne(ninja.find("  command = $mcpp stage $verify --output $out $in\n"), std::string::npos)) << ninja;
    // restat is what keeps a skipped (already-equivalent) stage from dirtying
    // every importer of the staged BMI.
    auto rulePos = ninja.find("rule stage_file\n");
    auto restatPos = ninja.find("  restat = 1\n", rulePos);
    auto nextRulePos = ninja.find("\nrule ", rulePos + 1);
    expect(fatal(ne(restatPos, std::string::npos))) << ninja;
    expect(lt(restatPos, nextRulePos)) << ninja;

    // The old per-platform in-place copies are gone for good — including the
    // second one, in runtime_alias, which the Windows job caught still
    // shelling out to PowerShell.
    expect(eq(ninja.find("Copy-Item"), std::string::npos)) << ninja;
    expect(eq(ninja.find("cp -f $in $out"), std::string::npos)) << ninja;
    expect(eq(ninja.find("cp_bmi"), std::string::npos)) << ninja;
}};

UTest runtime_alias_copies_through_stage_on_windows_and_symlinks_elsewhere{"NinjaBackend/RuntimeAliasCopiesThroughStageOnWindowsAndSymlinksElsewhere", [] {
    auto plan = minimal_plan();
    auto ninja = emit_ninja_string(plan);
    auto rulePos = ninja.find("rule runtime_alias\n");
    expect(fatal(ne(rulePos, std::string::npos))) << ninja;
    auto body = ninja.substr(rulePos, ninja.find("\nrule ", rulePos + 1) - rulePos);

    if constexpr (mcpplibs::os::is_windows) {
        // PE has no soname symlink: the alias is a copy of a freshly built DLL,
        // the same hazard class as BMI staging.
        expect(ne(body.find("$mcpp stage --output $out $in"), std::string::npos)) << body;
        expect(ne(body.find("restat = 1"), std::string::npos)) << body;
    } else {
        // POSIX must keep the SYMLINK — turning it into a copy would change
        // what gets packaged, not just how it is written.
        expect(ne(body.find("ln -s"), std::string::npos)) << body;
        expect(eq(body.find("stage --output"), std::string::npos)) << body;
    }
}};

UTest mcpp_binary_is_bound_even_without_dyndep{"NinjaBackend/McppBinaryIsBoundEvenWithoutDyndep", [] {
    // dyndep off ⇒ no scan/collect rules, but stage_file still needs $mcpp.
    std::string ninja;
    {
        mcpp::invocation::ScopedEnv dyndep("MCPP_NINJA_DYNDEP", "0");
        auto plan = minimal_plan();
        ninja = emit_ninja_string(plan);
    }

    expect(eq(ninja.find("rule cxx_scan"), std::string::npos)) << ninja;
    expect(ne(ninja.find("\nmcpp      = "), std::string::npos)) << ninja;
}};

// SPEC-007 R4.2: one destination, one writer. On a PE target two spellings
// that differ only in case are one file where the program runs, so they are
// one destination; elsewhere they are two.
UTest folds_case_only_on_a_pe_target{"DeployDestination/FoldsCaseOnlyOnAPeTarget", [] {
    const mcpp::build::BuildPlan::DeployFile d{{"/a/Foo.DLL"}, "bin/Foo.DLL"};
    expect(d.is_destination("bin/Foo.DLL", /*peTarget=*/false));
    expect(!d.is_destination("bin/foo.dll", /*peTarget=*/false));
    expect(d.is_destination("bin/foo.dll", /*peTarget=*/true));
    expect(!d.is_destination("bin/sub/foo.dll", /*peTarget=*/true));
}};

UTest std_artifacts_and_runtime_dlls_use_the_stage_rule{"NinjaBackend/StdArtifactsAndRuntimeDllsUseTheStageRule", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.toolchain.binaryPath = "/usr/bin/clang++";
    plan.stdBmiPath = "/cache/bmi/fp/pcm.cache/std.pcm";
    plan.stdObjectPath = "/cache/bmi/fp/std.o";
    plan.stdCompatBmiPath = "/cache/bmi/fp/pcm.cache/std.compat.pcm";
    plan.stdCompatObjectPath = "/cache/bmi/fp/std.compat.o";
    plan.runtimeDeployFiles.push_back({{"/pkg/lib/libfoo.dll"}, "bin/libfoo.dll"});

    auto ninja = emit_ninja_string(plan);

    // The std artifacts are fingerprint-scoped, so they carry the size-only
    // check: it needs no OPEN of the destination, which is what lets staging
    // survive a holder that denies reads (Windows FileShare.None).
    expect(ne(ninja.find("build pcm.cache/std.pcm : stage_file "
                         "/cache/bmi/fp/pcm.cache/std.pcm\n"
                         "  verify = --verify size\n"), std::string::npos)) << ninja;
    expect(ne(ninja.find("build obj/std.o : stage_file /cache/bmi/fp/std.o\n"
                         "  verify = --verify size\n"), std::string::npos)) << ninja;
    // std.compat keeps its order-only dependency on the staged std BMI.
    expect(ne(ninja.find("build pcm.cache/std.compat.pcm : stage_file "
                         "/cache/bmi/fp/pcm.cache/std.compat.pcm | pcm.cache/std.pcm"), std::string::npos)) << ninja;
    // Windows DLL deployment shares the rule — and therefore the
    // skip-if-equivalent behaviour that keeps a loaded DLL from failing a build.
    // But NOT the size-only shortcut: a rebuilt DLL keeps its size (PE sections
    // are page-padded), so this edge must compare content.
    auto dllEdge = ninja.find("build bin/libfoo.dll : stage_file /pkg/lib/libfoo.dll\n");
    expect(fatal(ne(dllEdge, std::string::npos))) << ninja;
    auto afterDll = ninja.substr(dllEdge, ninja.find('\n', dllEdge + 1) - dllEdge + 1);
    expect(eq(afterDll.find("verify"), std::string::npos)) << afterDll;
}};

// ── SPEC-007 R4.3: a Windows program's runtime DLLs are placed after its link ──

namespace {

BuildPlan program_plan(std::string_view triple, bool withRuntimeDirs) {
    auto plan = minimal_plan();
    plan.toolchain.targetTriple = std::string(triple);
    plan.linkUnits.push_back({
        .targetName = "app",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/main.o"},
        .output = "bin/app.exe",
        .entryMain = "src/main.cpp",
    });
    if (withRuntimeDirs)
        plan.linkIntent.runtimeSearchDirs.push_back("/sdk/my bin");
    return plan;
}

}  // namespace

UTest pe_program_with_runtime_search_dirs_gets_a_placement_edge{"NinjaBackend/PeProgramWithRuntimeSearchDirsGetsAPlacementEdge", [] {
    auto ninja = emit_ninja_string(program_plan("x86_64-w64-windows-gnu", true));
    // One rule, carrying the directories in search order, each one word.
    expect(ne(ninja.find("rule place_dlls\n"), std::string::npos)) << ninja;
    // The C++ runtime rule the resolver applied (mcpp.build.runtime_placement):
    // a GNU PE program has no MSVC runtime set, so the edge is told so and
    // places by search order.
    expect(ne(ninja.find("place-dlls --output $out --depfile $out.d --crt not-applicable $in "), std::string::npos)) << ninja;
    expect(ne(ninja.find("my bin"), std::string::npos)) << ninja;
    expect(ne(ninja.find("  deps = gcc\n"), std::string::npos)) << ninja;
    // One edge per program, after the link it reads, and built by default.
    expect(ne(ninja.find("build bin/app.exe.dlls: place_dlls bin/app.exe\n"), std::string::npos)) << ninja;
    expect(ne(ninja.find("default bin/app.exe.dlls\n"), std::string::npos)) << ninja;
}};

UTest no_placement_edge_without_runtime_search_dirs_or_off_pe{"NinjaBackend/NoPlacementEdgeWithoutRuntimeSearchDirsOrOffPe", [] {
    // A PE program with no runtime search directory has nothing to place.
    auto pe = emit_ninja_string(program_plan("x86_64-w64-windows-gnu", false));
    expect(eq(pe.find("place_dlls"), std::string::npos)) << pe;
    // ELF keeps its run path: the directories are RUNPATH entries, not copies.
    auto elf = emit_ninja_string(program_plan("x86_64-linux-gnu", true));
    expect(eq(elf.find("place_dlls"), std::string::npos)) << elf;
}};

UTest deployed_dlls_are_order_only_inputs_of_the_link{"NinjaBackend/DeployedDllsAreOrderOnlyInputsOfTheLink", [] {
    // The linker reads the import library, never the deployed DLL, so a DLL
    // that changes (or a deploy entry a later plan adds) must not relink.
    auto plan = program_plan("x86_64-w64-windows-gnu", false);
    plan.runtimeDeployFiles.push_back({{"/pkg/lib/libfoo.dll"}, "bin/libfoo.dll"});
    auto ninja = emit_ninja_string(plan);
    auto link = ninja.find("build bin/app.exe");
    expect(fatal(ne(link, std::string::npos))) << ninja;
    auto line = ninja.substr(link, ninja.find('\n', link) - link);
    auto oo = line.find(" || ");
    expect(fatal(ne(oo, std::string::npos))) << line;
    expect(ne(line.find("bin/libfoo.dll", oo), std::string::npos)) << line;
    auto implicitBar = line.find(" | ");
    expect(implicitBar == std::string::npos || implicitBar >= oo
                || line.substr(implicitBar, oo - implicitBar).find("libfoo.dll")
                       == std::string::npos) << line;
}};

// ── #311: staging failures must stay readable through the output filter ──

UTest filter_keeps_staging_diagnostics_and_failed_target{"NinjaBackend/FilterKeepsStagingDiagnosticsAndFailedTarget", [] {
    std::vector<std::string> prefixes = {"/usr/bin/g++", "/opt/mcpp/bin/mcpp"};
    std::string raw =
        "ninja: Entering directory `target/x86_64-linux-gnu/fp'\n"
        "[1/3] STAGE pcm.cache/std.pcm\n"
        "FAILED: [code=1] pcm.cache/std.pcm \n"
        "/opt/mcpp/bin/mcpp stage --output pcm.cache/std.pcm /cache/std.pcm\n"
        "error: cannot stage file into the build directory\n"
        "help: another process has this file memory-mapped, loaded or open.\n"
        "ninja: build stopped: subcommand failed.\n";

    auto filtered = filter_ninja_output(raw, prefixes);

    // Which output failed is now preserved (it used to be dropped entirely).
    // ninja pads the target list with a trailing space; the normalized line
    // must not inherit it.
    expect(ne(filtered.find("failed: pcm.cache/std.pcm\n"), std::string::npos)) << filtered;
    expect(eq(filtered.find("[code=1]"), std::string::npos)) << filtered;
    // The diagnostic survives; the echoed command line does not.
    expect(ne(filtered.find("error: cannot stage file"), std::string::npos)) << filtered;
    expect(ne(filtered.find("help:"), std::string::npos)) << filtered;
    expect(eq(filtered.find("stage --output"), std::string::npos)) << filtered;
    expect(eq(filtered.find("ninja: Entering directory"), std::string::npos)) << filtered;
}};

// ── Cache-served units: stage edges, not compile edges ──────────────────────
//
// The global dependency cache used to copy artifacts into the build dir from
// inside prepare_build, while those same paths stayed declared as the outputs of
// compile edges. Ninja treats an output it has no command line for in
// .ninja_log as dirty ("command line not found in log"), so on a fresh build dir
// every "cached" unit was recompiled — the cache saved nothing and the CLI still
// printed "Cached". Staging has to be an edge for ninja to have a record.

UTest cached_unit_emits_stage_edge_and_no_compile_edge{"NinjaBackend/CachedUnitEmitsStageEdgeAndNoCompileEdge", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "/store/dep/src/dep.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/dep.m.o",
        .packageName = "dep",
        .providesModule = "dep",
        .servedFromCache = true,
        .cachedObject = "/bc/v1/pkg/idx/dep@1.0.0/key/obj/dep.m.o",
        .cachedBmi = "/bc/v1/pkg/idx/dep@1.0.0/key/bmi/dep.gcm",
    });

    auto ninja = emit_ninja_string(plan);

    expect(ne(ninja.find("build obj/dep.m.o : stage_file "
                         "/bc/v1/pkg/idx/dep@1.0.0/key/obj/dep.m.o"), std::string::npos)) << ninja;
    // The BMI is staged too, at the path a compile edge would have produced.
    expect(ne(ninja.find("stage_file /bc/v1/pkg/idx/dep@1.0.0/key/bmi/dep.gcm"), std::string::npos)) << ninja;
    // No compile edge, and no scan edge, for this unit.
    expect(eq(ninja.find("cxx_module /store/dep/src/dep.cppm"), std::string::npos))
        << ninja;
    expect(eq(ninja.find("cxx_object /store/dep/src/dep.cppm"), std::string::npos))
        << ninja;
    expect(eq(ninja.find("dep.cppm.ddi"), std::string::npos)) << ninja;
}};

// `--verify size` for the same reason the std artifacts use it: the entry
// directory is named by a key covering the toolchain, dialect, profile, the
// package's own config and its dependencies' keys, so under one key equal size
// IS equivalence — and size comes from directory metadata, which stays readable
// even when a holder denies opening the file.
UTest cached_stage_edges_use_size_verification{"NinjaBackend/CachedStageEdgesUseSizeVerification", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "/store/dep/src/a.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/a.o",
        .packageName = "dep",
        .servedFromCache = true,
        .cachedObject = "/cache/obj/a.o",
    });

    auto ninja = emit_ninja_string(plan);
    auto at = ninja.find("build obj/a.o : stage_file /cache/obj/a.o");
    expect(fatal(ne(at, std::string::npos))) << ninja;
    expect(ne(ninja.find("verify = --verify size", at), std::string::npos)) << ninja;
}};

// A cached unit sits next to uncached ones; only the cached one changes shape.
UTest uncached_units_still_compile_alongside_cached_ones{"NinjaBackend/UncachedUnitsStillCompileAlongsideCachedOnes", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "/store/dep/src/dep.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/dep.o",
        .packageName = "dep",
        .servedFromCache = true,
        .cachedObject = "/cache/obj/dep.o",
    });
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);
    expect(ne(ninja.find("build obj/dep.o : stage_file /cache/obj/dep.o"), std::string::npos)) << ninja;
    expect(ne(ninja.find("build obj/main.o : cxx_object src/main.cpp"), std::string::npos)) << ninja;
}};

// The default path must be byte-identical to what it was: `servedFromCache` is
// false everywhere unless a cache hit set it.
UTest no_stage_edges_without_cached_units{"NinjaBackend/NoStageEdgesWithoutCachedUnits", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);
    expect(eq(ninja.find("build obj/main.o : stage_file"), std::string::npos)) << ninja;
    expect(ne(ninja.find("build obj/main.o : cxx_object src/main.cpp"), std::string::npos)) << ninja;
}};

UTest object_only_package_builds_own_objects_alongside_cache_fills{"NinjaBackend/ObjectOnlyPackageBuildsOwnObjectsAlongsideCacheFills", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/provider.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/provider.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.push_back({
        .source = "/store/dep/src/dep.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/dep.o",
        .packageName = "dep",
        .fillsCache = true,
    });

    auto ninja = emit_ninja_string(plan);
    auto at = ninja.rfind("\ndefault ");
    expect(fatal(ne(at, std::string::npos))) << ninja;
    auto defaults = ninja.substr(at, ninja.find('\n', at + 1) - at);
    expect(ne(defaults.find(" obj/provider.o"), std::string::npos)) << defaults;
    expect(eq(count_occurrences(defaults, " obj/dep.o"), 1u)) << defaults;
}};

// A cached unit whose cachedObject was never filled in is a bug upstream, not a
// reason to emit a stage edge with an empty input — it must fall back to being
// compiled rather than producing an unbuildable graph.
UTest cached_unit_without_cached_object_path_is_still_compiled{"NinjaBackend/CachedUnitWithoutCachedObjectPathIsStillCompiled", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
        .servedFromCache = true,
    });

    auto ninja = emit_ninja_string(plan);
    expect(eq(ninja.find("stage_file \n"), std::string::npos)) << ninja;
    expect(eq(ninja.find("build obj/main.o : stage_file"), std::string::npos)) << ninja;
}};

// A cache-served unit keeps its compile_commands.json entry. The units stay in
// the plan (only their EDGE changes shape) precisely so clangd does not lose the
// dependency's sources — a cache that silently degraded IDE navigation would be
// a bad trade for build time.
UTest cached_units_still_appear_in_compile_commands{"NinjaBackend/CachedUnitsStillAppearInCompileCommands", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "/store/dep/src/dep.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/dep.o",
        .packageName = "dep",
        .servedFromCache = true,
        .cachedObject = "/bc/v1/pkg/idx/dep@1.0.0/key/obj/dep.o",
    });
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
    });

    auto flags = compute_flags(plan);
    auto cdb = emit_compile_commands(plan, flags);

    // #390: the emitter spells every path natively, so the expected source
    // spelling is make_preferred'ed (a no-op on POSIX). Compared on the
    // DECODED file field — the raw JSON text doubles every backslash.
    auto dep = std::filesystem::path("/store/dep/src/dep.c");
    dep.make_preferred();
    auto main = std::filesystem::path("src/main.cpp");
    main.make_preferred();
    auto j = nlohmann::json::parse(cdb);
    bool sawDep = false, sawMain = false;
    for (auto const& e : j) {
        auto f = e["file"].get<std::string>();
        sawDep  = sawDep  || f == dep.string();
        sawMain = sawMain || f == main.string();
    }
    expect(bool(sawDep)) << cdb;
    expect(bool(sawMain)) << cdb;
}};

// Replacing a package's compile edges with stage edges also removes the ordering
// those compile edges carried. A module PARTITION is the case that breaks: a
// consumer imports `pkg`, so its dyndep declares `pkg`'s BMI and nothing else —
// the partition BMI `pkg:part` was reached only because `pkg`'s own compile edge
// depended on it. With independent stage edges ninja may start the consumer while
// the partition is still unstaged, and Clang fails with `failed to find module
// file for module 'pkg:part'`. Observed on macOS CI while Linux won the race,
// which is why it is pinned here rather than left to scheduling.
UTest non_cached_edges_order_after_every_staged_artifact{"NinjaBackend/NonCachedEdgesOrderAfterEveryStagedArtifact", [] {
    auto plan = minimal_plan();
    // A cached package with a primary interface and a partition.
    plan.compileUnits.push_back({
        .source = "/store/dep/src/dep.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/dep.m.o",
        .packageName = "dep",
        .providesModule = "dep",
        .servedFromCache = true,
        .cachedObject = "/bc/obj/dep.m.o",
        .cachedBmi = "/bc/bmi/dep.gcm",
    });
    plan.compileUnits.push_back({
        .source = "/store/dep/src/part.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/part.m.o",
        .packageName = "dep",
        .providesModule = "dep:part",
        .servedFromCache = true,
        .cachedObject = "/bc/obj/part.m.o",
        .cachedBmi = "/bc/bmi/dep-part.gcm",
    });
    // The consumer, which imports only the primary module.
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
        .imports = {"dep"},
    });

    auto ninja = emit_ninja_string(plan);

    // One phony aggregating every staged artifact — a per-edge list would repeat
    // it and grow ninja lines without bound (mcpp#274).
    auto phony = ninja.find("build _mcpp_staged_cache : phony");
    expect(fatal(ne(phony, std::string::npos))) << ninja;
    auto phonyLine = ninja.substr(phony, ninja.find('\n', phony) - phony);
    for (auto* art : {"obj/dep.m.o", "obj/part.m.o",
                      "gcm.cache/dep.gcm", "gcm.cache/dep-part.gcm"}) {
        expect(ne(phonyLine.find(art), std::string::npos))
            << art << " missing from: " << phonyLine;
    }

    // The consumer's edge orders after it...
    auto consumer = ninja.find("build obj/main.o");
    expect(fatal(ne(consumer, std::string::npos))) << ninja;
    auto consumerLine = ninja.substr(consumer, ninja.find('\n', consumer) - consumer);
    expect(ne(consumerLine.find("|| _mcpp_staged_cache"), std::string::npos))
        << consumerLine;

    // ...as an ORDER-ONLY dependency, not an implicit one: the real content
    // dependency is still declared where it always was, so this must add
    // sequencing without making a staged BMI dirty its consumers.
    auto bar = consumerLine.find("|| _mcpp_staged_cache");
    expect(eq(consumerLine.find("| _mcpp_staged_cache"), bar + 1))
        << "staged phony must appear only after ||, never as an implicit input: "
        << consumerLine;
}};

// The other direction of the same ordering. A staged BMI can import a module
// compiled HERE: a unit of the package that the cache entry does not hold,
// such as a source its build program writes below the consumer's target
// directory (xpkg's `lua_stdlib`). The consumer's dyndep names the staged BMI
// only, and the stage edge had no input but the cache entry, so a consumer
// could compile while that module's BMI did not yet exist (`failed to read
// compiled module: No such file or directory`, observed in CI once the scan
// pass changed the schedule). The stage edge now waits for those BMIs, and it
// leaves the aggregate, because every compile edge waits for the aggregate and
// the compile it waits for is one of them.
UTest a_staged_bmi_waits_for_the_modules_it_imports_that_compile_here{"NinjaBackend/AStagedBmiWaitsForTheModulesItImportsThatCompileHere", [] {
    auto plan = minimal_plan();
    // The generated unit of the cached package, compiled here.
    plan.compileUnits.push_back({
        .source = "target/.build-mcpp/deps/dep/out/gen.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/gen.m.o",
        .packageName = "dep",
        .providesModule = "dep.gen",
    });
    // The package's primary interface, served from the cache, imports it.
    plan.compileUnits.push_back({
        .source = "/store/dep/src/dep.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/dep.m.o",
        .packageName = "dep",
        .providesModule = "dep",
        .imports = {"dep.gen"},
        .servedFromCache = true,
        .cachedObject = "/bc/obj/dep.m.o",
        .cachedBmi = "/bc/bmi/dep.gcm",
    });
    // A second staged interface that imports the first: it waits as well.
    plan.compileUnits.push_back({
        .source = "/store/dep/src/api.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/api.m.o",
        .packageName = "dep",
        .providesModule = "dep.api",
        .imports = {"dep"},
        .servedFromCache = true,
        .cachedObject = "/bc/obj/api.m.o",
        .cachedBmi = "/bc/bmi/dep.api.gcm",
    });
    // A staged interface whose imports are all staged: unchanged.
    plan.compileUnits.push_back({
        .source = "/store/dep/src/util.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/util.m.o",
        .packageName = "dep",
        .providesModule = "dep.util",
        .servedFromCache = true,
        .cachedObject = "/bc/obj/util.m.o",
        .cachedBmi = "/bc/bmi/dep.util.gcm",
    });
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
        .imports = {"dep.api", "dep.util"},
    });

    auto ninja = emit_ninja_string(plan);
    auto line_of = [&](std::string_view head) {
        auto at = ninja.find(head);
        if (at == std::string::npos) return std::string{};
        return ninja.substr(at, ninja.find('\n', at) - at);
    };

    auto dep = line_of("build gcm.cache/dep.gcm : stage_file");
    expect(fatal(!dep.empty())) << ninja;
    expect(ne(dep.find("|| gcm.cache/dep.gen.gcm"), std::string::npos)) << dep;
    auto api = line_of("build gcm.cache/dep.api.gcm : stage_file");
    expect(fatal(!api.empty())) << ninja;
    expect(ne(api.find("|| gcm.cache/dep.gcm"), std::string::npos)) << api;
    auto util = line_of("build gcm.cache/dep.util.gcm : stage_file");
    expect(fatal(!util.empty())) << ninja;
    expect(eq(util.find("||"), std::string::npos)) << util;

    // The aggregate holds the BMIs that wait for nothing, and every object.
    auto phony = line_of("build _mcpp_staged_cache : phony");
    expect(fatal(!phony.empty())) << ninja;
    auto words = [](const std::string& s) {
        std::set<std::string> w;
        std::istringstream is(s);
        for (std::string t; is >> t;) w.insert(t);
        return w;
    };
    auto held = words(phony);
    expect(!held.contains("gcm.cache/dep.gcm")) << phony;
    expect(!held.contains("gcm.cache/dep.api.gcm")) << phony;
    expect(held.contains("gcm.cache/dep.util.gcm")) << phony;
    for (auto* o : {"obj/dep.m.o", "obj/api.m.o", "obj/util.m.o"})
        expect(held.contains(o)) << o << " missing from: " << phony;

    // The generated unit still compiles after the aggregate: no cycle.
    auto gen = line_of("build obj/gen.m.o");
    expect(fatal(!gen.empty())) << ninja;
    expect(ne(gen.find("|| _mcpp_staged_cache"), std::string::npos)) << gen;
}};

UTest no_staged_phony_when_nothing_is_cached{"NinjaBackend/NoStagedPhonyWhenNothingIsCached", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/main.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/main.o",
        .packageName = "objc_rule_test",
    });
    auto ninja = emit_ninja_string(plan);
    expect(eq(ninja.find("_mcpp_staged_cache"), std::string::npos)) << ninja;
}};

// #390: the main-manifest include channel's fallback join — `root / inc` for
// a directory a build step will create LATER (the `generated/inc` shape) —
// must spell its result NATIVELY on Windows. MSVC keeps the `/` of a TOML
// `generated/inc` verbatim, so the bare join yields `C:\proj\generated/inc`,
// which reaches the CDB's -I via local_include_args and breaks CLion. Same
// rule for an absolute entry written with forward slashes (`C:/SDL2/include`).
// (The existing-directory path runs through expand_dir_glob, covered by
// Scanner.ExpandDirGlobMultiSegmentUsesNativeSeparators.)
UTest expand_manifest_include_entry_native_spelling{"Plan/ExpandManifestIncludeEntryNativeSpelling", [] {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "mcpp-plan-inc-entry";
    fs::remove_all(root);
    fs::create_directories(root);   // generated/inc deliberately does NOT exist

    auto fb = expand_manifest_include_entry(root, fs::path("generated/inc"));
    expect(fatal(eq(fb.size(), 1u)));
    if constexpr (fs::path::preferred_separator == '\\')
        expect(eq(fb[0].string().find('/'), std::string::npos)) << fb[0];
    expect(eq(fb[0], root / "generated" / "inc"));

    auto abs = expand_manifest_include_entry(root, fs::path("C:/SDL2/include"));
    expect(fatal(eq(abs.size(), 1u)));
    if constexpr (fs::path::preferred_separator == '\\')
        expect(eq(abs[0].string(), "C:\\SDL2\\include"));

    fs::remove_all(root);
}};

// ── PE `toolchain-coupled`: the toolset's CRT travels with the artifact ─────
//
// On ELF this contract needs no files copied — the artifact carries an rpath
// into the toolchain's lib directory. PE has no rpath: a DLL is resolved from
// the directory of the executable, so on this format the mechanism IS the
// copy, and a graph that resolves the contract but stages nothing delivers
// exactly what the old flat refusal did.
//
// Runs on every host. The format now comes from the TARGET TRIPLE rather than
// from `mcpplibs::os::is_windows`, which is what makes a Windows contract
// assertable on the Linux runner that reviews most of these changes.
namespace {

struct FakeRedistDir {
    std::filesystem::path path;
    FakeRedistDir() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp-deploy-{}", std::chrono::steady_clock::now()
                                                 .time_since_epoch().count());
        std::filesystem::create_directories(path);
        for (auto name : {"vcruntime140.dll", "msvcp140.dll",
                          "vcruntime140_1.dll"})
            std::ofstream{path / name} << "MZ";
        // Not a DLL: the redist directory also carries a manifest, and copying
        // it beside the artifact would be cargo.
        std::ofstream{path / "Microsoft.VC143.CRT.manifest"} << "<assembly/>";
    }
    ~FakeRedistDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    FakeRedistDir(const FakeRedistDir&) = delete;
    FakeRedistDir& operator=(const FakeRedistDir&) = delete;
};

BuildPlan msvc_plan_with_redist(const FakeRedistDir& redist,
                                std::string_view cxxRuntime) {
    auto plan = minimal_plan();
    plan.toolchain.compiler        = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath      = "cl.exe";
    plan.toolchain.targetTriple    = "x86_64-pc-windows-msvc";
    plan.toolchain.linkRuntimeDirs = {redist.path};
    plan.toolchain.base.msvc.redistDir   = redist.path;
    plan.manifest.buildConfig.cxxRuntime = std::string(cxxRuntime);
    plan.linkUnits.push_back({
        .targetName = "app",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/main.o"},
        .output = "bin/app.exe",
        .entryMain = "src/main.cpp",
    });
    return plan;
}

// The entries of the runtime placement resolver's answer that come from the
// toolset's C++ runtime.
std::vector<BuildPlan::DeployFile> toolset_entries(const mcpp::build::CompileFlags& f) {
    std::vector<BuildPlan::DeployFile> out;
    for (auto const& d : f.runtimeDeploy)
        if (d.origin == BuildPlan::DeployFile::Origin::Toolchain) out.push_back(d);
    return out;
}

}  // namespace

UTest toolchain_coupled_stages_the_toolset_crt_beside_the_exe{"NinjaBackendPeRuntime/ToolchainCoupledStagesTheToolsetCrtBesideTheExe", [] {
    FakeRedistDir redist;
    auto plan = msvc_plan_with_redist(redist, "toolchain-coupled");

    auto flags = compute_flags(plan);
    auto toolset = toolset_entries(flags);
    expect(fatal(eq(toolset.size(), 3u)))
        << "expected the three .dll and not the .manifest beside them";
    for (auto const& d : toolset) {
        expect(eq(d.dest.parent_path(), std::filesystem::path("bin")))
            << "a DLL must land in the same directory as the .exe: "
            << d.dest.string();
        expect(fatal(eq(d.sources.size(), 1u)));
        expect(eq(d.sources.front().extension(), ".dll")) << d.sources.front().string();
    }

    std::string placements;
    auto ninja = emit_ninja_string(plan, &placements);
    // #734 E4: two or more placements are one `stage_list` edge whose outputs
    // are every destination, and the list the edge reads names each pair.
    const auto listAt = ninja.find(": stage_list ");
    expect(fatal(ne(listAt, std::string::npos))) << ninja;
    const auto listLine = ninja.substr(ninja.rfind("\nbuild ", listAt) + 1,
                                       ninja.find('\n', listAt) - ninja.rfind("\nbuild ", listAt) - 1);
    for (auto name : {"vcruntime140.dll", "msvcp140.dll", "vcruntime140_1.dll"}) {
        expect(ne(listLine.find(std::format("bin/{}", name)), std::string::npos))
            << name << " is not an output of the placement edge\n" << ninja;
        expect(ne(placements.find(std::format("\tbin/{}", name)), std::string::npos))
            << name << " is not in the placement list\n" << placements;
    }
    expect(eq(ninja.find("Microsoft.VC143.CRT.manifest"), std::string::npos))
        << "copied something that is not a runtime DLL\n" << ninja;

    // Reachability, twice over. A copy edge nothing asks for is never run
    // under explicit ninja goals, and an .exe that does not depend on its DLLs
    // can be reported "up to date" while they are missing.
    auto exeAt = ninja.find("build bin/app.exe :");
    expect(fatal(ne(exeAt, std::string::npos))) << ninja;
    auto exeStanza = ninja.substr(exeAt, ninja.find("\nbuild ", exeAt + 1) - exeAt);
    expect(ne(exeStanza.find("bin/vcruntime140.dll"), std::string::npos))
        << "the executable does not depend on the CRT staged beside it\n"
        << exeStanza;
    auto defaultAt = ninja.rfind("\ndefault ");
    expect(fatal(ne(defaultAt, std::string::npos))) << ninja;
    expect(ne(ninja.find("bin/vcruntime140.dll", defaultAt), std::string::npos))
        << "the staged CRT is not a default goal\n" << ninja.substr(defaultAt);
}};

UTest host_coupled_stages_nothing{"NinjaBackendPeRuntime/HostCoupledStagesNothing", [] {
    // An EXPLICIT `host-coupled`: the machine is a promise that it provides
    // the DLLs, and keeping a copy beside the artifact would contradict it.
    FakeRedistDir redist;
    auto plan = msvc_plan_with_redist(redist, "host-coupled");
    expect(compute_flags(plan).runtimeDeploy.empty());

    // The BARE default (#718): `toolchain-coupled` is now the MSVC-ABI
    // default for every role, so a project that never mentioned
    // `cxx_runtime` stages the toolset's redistributable exactly as an
    // explicit `toolchain-coupled` would — see
    // `ToolchainCoupledStagesTheToolsetCrtBesideTheExe`.
    auto bare = msvc_plan_with_redist(redist, "");
    expect(eq(toolset_entries(compute_flags(bare)).size(), 3u))
        << "the undeclared MSVC-ABI default no longer stages the redistributable";
}};

UTest a_projects_own_deploy_file_outranks_the_toolsets{"NinjaBackendPeRuntime/AProjectsOwnDeployFileOutranksTheToolsets", [] {
    // A vendored redist named in `[runtime] deploy_files` is a human's
    // statement about which build of msvcp140.dll this program ships. Silently
    // replacing it with the toolset's copy produces a different program than
    // the manifest describes, so the explicit one wins -- and is compared with
    // the toolset's version (D2). The fake files carry no VERSIONINFO, so the
    // comparison cannot be made, and that is what is said.
    FakeRedistDir redist;
    auto plan = msvc_plan_with_redist(redist, "toolchain-coupled");
    plan.runtimeDeployFiles.push_back(
        {{"/vendor/msvcp140.dll"}, std::filesystem::path("bin") / "msvcp140.dll"});

    auto flags = compute_flags(plan);
    for (auto const& d : toolset_entries(flags))
        expect(ne(d.dest.filename(), std::filesystem::path("msvcp140.dll")))
            << "overwrote the project's own deploy file";
    expect(eq(toolset_entries(flags).size(), 2u));
    expect(std::ranges::any_of(flags.runtimeDeploy, [](auto const& d) {
        return d.origin == BuildPlan::DeployFile::Origin::Declared
            && d.sources.front() == std::filesystem::path("/vendor/msvcp140.dll");
    })) << "the declared file is not placed";
    expect(std::ranges::any_of(flags.runtimeNotes, [](auto const& d) {
        return d.find("msvcp140.dll") != std::string::npos;
    })) << "the declared runtime file was placed without a word about its version";
}};

// The 2026-09-28 design, §2.9: a build for a Windows target has one C++
// runtime, the toolset's, for the program and for the tools that build it.
// Every action runs with the toolset's runtime directory first on its PATH,
// through the named wrapper, whatever its role. A target without the MSVC ABI
// keeps its commands as they were.
UTest every_action_of_a_windows_target_has_the_toolset_runtime_on_path{"NinjaBackendPeRuntime/EveryActionOfAWindowsTargetHasTheToolsetRuntimeOnPath", [] {
    FakeRedistDir redist;
    auto plan = msvc_plan_with_redist(redist, "");
    mcpp::manifest::BuildAction a;
    a.id = "gen:moc";
    a.packageName = "app";
    a.role = mcpp::manifest::BuildAction::Role::Source;
    a.command = {"moc.exe", "widget.h", "-o", "gen/moc_widget.cpp"};
    a.outputs = {"gen/moc_widget.cpp"};
    plan.actions.push_back(a);

    auto ninja = emit_ninja_string(plan);
    auto ruleAt = ninja.find("rule mcpp_action_0");
    expect(fatal(ne(ruleAt, std::string::npos))) << ninja;
    auto rule = ninja.substr(ruleAt, ninja.find("\n\n", ruleAt) - ruleAt);
    expect(ne(rule.find(" __action"), std::string::npos)) << rule;
    expect(ne(rule.find("--path-prepend"), std::string::npos)) << rule;
    expect(ne(rule.find(redist.path.filename().string()), std::string::npos))
        << "the prepended directory is not the toolset's runtime directory\n" << rule;

    auto elf = minimal_plan();
    elf.actions.push_back(a);
    auto elfNinja = emit_ninja_string(elf);
    expect(eq(elfNinja.find("--path-prepend"), std::string::npos))
        << "a target without the MSVC ABI received the Windows runtime on PATH\n" << elfNinja;
}};

UTest an_elf_toolchain_never_stages_its_runtime_dirs{"NinjaBackendPeRuntime/AnElfToolchainNeverStagesItsRuntimeDirs", [] {
    // `linkRuntimeDirs` means "the toolchain's private runtime" for every
    // provider — on gcc it is libstdc++'s directory. That is reached through
    // an rpath, and copying its contents into an output tree would be a
    // different contract wearing the same name.
    FakeRedistDir redist;
    auto plan = msvc_plan_with_redist(redist, "toolchain-coupled");
    plan.toolchain.compiler     = mcpp::toolchain::CompilerId::GCC;
    plan.toolchain.binaryPath   = "/usr/bin/g++";
    plan.toolchain.targetTriple = "x86_64-linux-gnu";
    expect(toolset_entries(compute_flags(plan)).empty());
}};

// #718: every MSVC-ABI row x {undeclared, self-contained, toolchain-coupled,
// host-coupled, linkage=static} yields exactly one CRT word, spelled for its
// own driver, and that word is the SAME on the compile line (`f.cxx`,
// `f.cc`) and — for the LLVM row — the link line (`f.ld`, `f.ldC`). This is
// the exact property #649 E10 broke: a compile-only flag the clang driver's
// link step never saw, so it chose `-defaultlib:libcmt` on its own. cl's
// link line is `link.exe` directly (`LinkShape::MsvcLinkExe`) and carries no
// compile flag at all — the CRT is baked into the objects it is given — so
// only the LLVM row's link line is asked to repeat the word.
UTest crt_word_is_one_word_equal_on_compile_and_link{"NinjaBackendPeRuntime/CrtWordIsOneWordEqualOnCompileAndLink", [] {
    FakeRedistDir redist;
    struct Row {
        std::string_view label;
        mcpp::toolchain::CompilerId compiler;
        std::string_view staticWord, dynamicWord;
    };
    Row rows[] = {
        {"cl",   mcpp::toolchain::CompilerId::MSVC,  "/MT", "/MD"},
        {"llvm", mcpp::toolchain::CompilerId::Clang, "-fms-runtime-lib=static",
                                                     "-fms-runtime-lib=dll"},
    };
    struct Case { std::string_view linkage, cxxRuntime; bool wantsStatic; };
    Case cases[] = {
        {"", "",                  false},   // undeclared -> toolchain-coupled
        {"", "self-contained",    true},
        {"", "toolchain-coupled", false},
        {"", "host-coupled",      false},
        {"static", "",            true},
    };
    for (auto& row : rows) {
        for (auto& c : cases) {
            auto plan = msvc_plan_with_redist(redist, c.cxxRuntime);
            plan.toolchain.compiler = row.compiler;
            plan.manifest.buildConfig.linkage = std::string(c.linkage);
            auto flags = compute_flags(plan);
            auto want = c.wantsStatic ? row.staticWord : row.dynamicWord;
            auto other = c.wantsStatic ? row.dynamicWord : row.staticWord;
            /* trace: std::format("{} linkage='{}' cxx_runtime='{}'",
                                     row.label, c.linkage, c.cxxRuntime) */;

            expect(eq(count_occurrences(flags.cxx, want), 1u)) << flags.cxx;
            expect(eq(count_occurrences(flags.cc, want), 1u)) << flags.cc;
            // A word of its own: `-fms-runtime-lib=dll-std=c11` holds the text
            // once and states neither the model nor the standard.
            expect(eq(count_words(flags.cxx, want), 1u)) << flags.cxx;
            expect(eq(count_words(flags.cc, want), 1u)) << flags.cc;
            if (row.compiler == mcpp::toolchain::CompilerId::Clang)
                expect(eq(flags.cc.find("=dll-std"), std::string::npos)) << flags.cc;
            expect(eq(flags.cxx.find(other), std::string::npos));
            expect(eq(flags.cc.find(other), std::string::npos));

            // `link_shape` (flags.cppm) picks `LinkShape::PeLld` — the
            // branch that carries this word on the link line — only when
            // `current_link_host()` is Windows, a HOST fact fixed at mcpp's
            // own compile time. A Linux-built test binary always resolves
            // this plan's link line through the ELF/Generic branch instead,
            // whatever the plan's TARGET triple says, so the link-line half
            // of this property is checkable only on a Windows-built mcpp —
            // see the Windows e2e leg (814) for that half.
            if constexpr (mcpplibs::os::is_windows) {
                if (row.compiler == mcpp::toolchain::CompilerId::Clang) {
                    expect(eq(count_occurrences(flags.ld, want), 1u)) << flags.ld;
                    expect(eq(flags.ld.find(other), std::string::npos));
                    expect(eq(flags.ld, flags.ldC));
                }
            }
        }
    }
}};

// ── link_failure_advice ──────────────────────────────────────────────────────
//
// A freestanding link that reaches for `operator new` fails naming a mangled
// symbol from a header inside the standard library. Nothing in that message
// says which package supplies it, or that the answer is one manifest line.

UTest names_the_allocator_feature_for_llds_spelling{"LinkFailureAdvice/NamesTheAllocatorFeatureForLldsSpelling", [] {
    const std::string out =
        "ld.lld: error: undefined symbol: operator new(unsigned long)\n"
        ">>> referenced by allocate.h:58\n";
    auto advice = mcpp::build::link_failure_advice(out);
    expect(ne(advice.find("alloc-kal"), std::string::npos));
    expect(ne(advice.find("std-freestanding"), std::string::npos));
    // The four that are most often forgotten are named, because omitting them
    // produces a SECOND link failure after the first is fixed.
    expect(ne(advice.find("align_val_t"), std::string::npos));
}};

UTest matches_gnu_lds_spelling_too{"LinkFailureAdvice/MatchesGnuLdsSpellingToo", [] {
    // Two linkers, two spellings of the same fact. Matching only lld's would
    // make the advice appear on some toolchains and not others.
    const std::string out =
        "/usr/bin/ld: main.o: undefined reference to `operator new(unsigned long)'\n";
    expect(!(mcpp::build::link_failure_advice(out).empty()));
}};

UTest stays_silent_on_unrelated_failures{"LinkFailureAdvice/StaysSilentOnUnrelatedFailures", [] {
    // The advice is appended to EVERY failed build, so a needle that matched
    // loosely would attach allocator advice to a missing board symbol.
    const std::string out =
        "ld.lld: error: undefined symbol: board_uart_init\n"
        "error: cannot open crt0.o: No such file or directory\n";
    expect(mcpp::build::link_failure_advice(out).empty());
}};

UTest carries_no_version_literal{"LinkFailureAdvice/CarriesNoVersionLiteral", [] {
    // The `import std` advice needed tests/e2e/135 to keep its version
    // honest. This one is built so it cannot go stale the same way: it names a
    // package and a FEATURE, and the feature pulls the implementation. A digit
    // sequence that looks like a version here would be a regression in kind.
    auto advice = mcpp::build::link_failure_advice(
        "ld.lld: error: undefined symbol: operator new(unsigned long)\n");
    expect(fatal(!advice.empty()));
    expect(eq(advice.find("0."), std::string::npos)) << advice;
}};

// ── graph_c_library_isolation_advice (mcpp#662) ─────────────────────────────
//
// A graph-supplied C library closes the compiler's own search of the host's
// copy (`-nostdlibinc`); a package that only compiled before because a host
// header filled a gap the graph did not now fails naming a header the reader
// has never asked for. The advice fires on the SAME EVIDENCE a human would
// use to diagnose it: the isolating token on the command line, together with
// the compiler's own "file not found".

UTest fires_only_when_both_the_token_and_the_error_are_there{"GraphCLibraryIsolationAdvice/FiresOnlyWhenBothTheTokenAndTheErrorAreThere", [] {
    // Neither alone is enough.
    expect(mcpp::build::graph_c_library_isolation_advice(
        "clang: error: something else entirely\n").empty());
    expect(mcpp::build::graph_c_library_isolation_advice(
        "-nostdlibinc -std=c11 -c probe.c\n"
        "probe.c:1:1: error: unknown type name 'nonsense'\n").empty());
    expect(mcpp::build::graph_c_library_isolation_advice(
        "clang -xc -std=c11 -c probe.c\n"
        "probe.c:1:10: fatal error: 'io.h' file not found\n").empty());
    // Both together: an ordinary compile failure the fix's own token is on.
    const std::string out =
        "FAILED: obj/probe.c.o\n"
        "clang -std=c11 --target=x86_64-w64-windows-gnu -nostdlibinc "
        "-nostdinc++ -c probe.c -o obj/probe.c.o\n"
        "probe.c:1:10: fatal error: 'io.h' file not found\n"
        "    1 | #include <io.h>\n";
    expect(!(mcpp::build::graph_c_library_isolation_advice(out).empty()));
}};

// WHETHER THE C LIBRARY IS THE GRAPH'S reaches the fast path through a file
// beside build.ninja (LLVM 23.1.3 Part 3, D3): a native build on a managed C
// library carries `-nostdlibinc` too, so the token alone no longer answers it.
UTest present_only_for_a_graph_c_library{"GraphCLibrarySidecar/PresentOnlyForAGraphCLibrary", [] {
    const auto dir = std::filesystem::temp_directory_path()
        / std::format("mcpp_graph_clib_{}", std::random_device{}());
    std::filesystem::create_directories(dir);

    mcpp::targetside::Layer graph;
    graph.origin = mcpp::targetside::Origin::Graph;
    graph.interfaceName = "musl";
    graph.impl = "openkal-musl@0.3.5";
    mcpp::build::write_graph_c_library_sidecar(dir, graph);
    auto read = mcpp::build::read_graph_c_library_sidecar(dir);
    expect(fatal(read.has_value()));
    expect(eq(read->first, "musl"));
    expect(eq(read->second, "openkal-musl@0.3.5"));

    // A payload C library (the native managed glibc) removes the file.
    mcpp::targetside::Layer payload;
    payload.origin = mcpp::targetside::Origin::Payload;
    payload.interfaceName = "glibc";
    mcpp::build::write_graph_c_library_sidecar(dir, payload);
    expect(!(mcpp::build::read_graph_c_library_sidecar(dir).has_value()));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}};

UTest names_the_library_when_given{"GraphCLibraryIsolationAdvice/NamesTheLibraryWhenGiven", [] {
    const std::string out =
        "clang -nostdlibinc -c probe.c\n"
        "probe.c:1:10: fatal error: 'io.h' file not found\n";
    auto advice = mcpp::build::graph_c_library_isolation_advice(
        out, "musl", "openkal-musl@0.3.5");
    expect(fatal(!advice.empty()));
    expect(ne(advice.find("musl"), std::string::npos));
    expect(ne(advice.find("openkal-musl@0.3.5"), std::string::npos));
    // Both remedies from the plan: adapting to the C library, and bringing
    // the platform dependency into the graph privately.
    expect(ne(advice.find("cfg(c-abi = \"musl\")"), std::string::npos));
    expect(ne(advice.find("private"), std::string::npos));
}};

UTest degrades_without_naming_any_package{"GraphCLibraryIsolationAdvice/DegradesWithoutNamingAnyPackage", [] {
    // The fast path (execute.cppm) has no BuildPlan to read a name from. The
    // note still fires — it does not stay silent just because the caller
    // could not name the library — and it does not fabricate a name either.
    const std::string out =
        "clang -nostdlibinc -c probe.c\n"
        "probe.c:1:10: fatal error: 'io.h' file not found\n";
    auto advice = mcpp::build::graph_c_library_isolation_advice(out);
    expect(!advice.empty());
    expect(ne(advice.find("this target's C library"), std::string::npos));
}};

// ── consumer_include_scope_advice (#690, design record F7) ──────────────────
//
// A dependency that compiled only because its consumer's include directories
// used to be broadcast to it now fails with an ordinary missing header. The
// note fires on the compiler's report AND a file of that name in one of the
// root's include directories; either alone is not this shape.

namespace {
std::filesystem::path consumer_include_fixture() {
    auto root = std::filesystem::temp_directory_path()
              / std::format("mcpp-consumer-include-{}",
                   std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root / "appinc" / "cfg");
    std::ofstream(root / "appinc" / "cfg7.h") << "#define WHO 1\n";
    std::ofstream(root / "appinc" / "cfg" / "nested.h") << "#define N 1\n";
    return root;
}
}  // namespace

UTest names_the_consumer_directory_for_each_driver_spelling{"ConsumerIncludeScopeAdvice/NamesTheConsumerDirectoryForEachDriverSpelling", [] {
    const auto root = consumer_include_fixture();
    const std::vector<std::filesystem::path> dirs{root / "absent", root / "appinc"};
    const std::string gcc =
        "FAILED: obj/dep/src/dep.c.o\n"
        "../dep/src/dep.c:1:10: fatal error: cfg7.h: No such file or directory\n";
    const std::string clang =
        "../dep/src/dep.cpp:1:10: fatal error: 'cfg/nested.h' file not found\n";
    const std::string cl =
        "dep.cpp(1): fatal error C1083: Cannot open include file: 'cfg7.h': "
        "No such file or directory\n";
    for (auto const& out : {gcc, clang, cl}) {
        auto advice = mcpp::build::consumer_include_scope_advice(out, dirs);
        expect(fatal(!advice.empty())) << out;
        expect(ne(advice.find((root / "appinc").string()), std::string::npos)) << advice;
        expect(ne(advice.find("no longer"), std::string::npos)) << advice;
    }
    std::filesystem::remove_all(root);
}};

UTest silent_when_the_header_is_not_the_consumers_or_no_header_is_missing{"ConsumerIncludeScopeAdvice/SilentWhenTheHeaderIsNotTheConsumersOrNoHeaderIsMissing", [] {
    const auto root = consumer_include_fixture();
    const std::vector<std::filesystem::path> dirs{root / "appinc"};
    // A missing header the consumer does not have either: an ordinary error.
    expect(mcpp::build::consumer_include_scope_advice(
        "dep.c:1:10: fatal error: other.h: No such file or directory\n", dirs).empty());
    // A failure that is not a missing header, although cfg7.h is the consumer's.
    expect(mcpp::build::consumer_include_scope_advice(
        "dep.c:3:1: error: unknown type name 'cfg7'\n", dirs).empty());
    // No root include directories at all (the fast path before any plan wrote
    // the sidecar).
    expect(mcpp::build::consumer_include_scope_advice(
        "dep.c:1:10: fatal error: cfg7.h: No such file or directory\n", {}).empty());
    std::filesystem::remove_all(root);
}};

UTest sidecar_round_trips{"ConsumerIncludeScopeAdvice/SidecarRoundTrips", [] {
    const auto root = consumer_include_fixture();
    const std::vector<std::filesystem::path> dirs{root / "appinc", root / "with space"};
    mcpp::build::write_consumer_include_sidecar(root, dirs);
    expect(eq(mcpp::build::read_consumer_include_sidecar(root), dirs));
    mcpp::build::write_consumer_include_sidecar(root, {});
    expect(mcpp::build::read_consumer_include_sidecar(root).empty());
    std::filesystem::remove_all(root);
}};

UTest does_not_rewrite_the_compilers_own_line{"GraphCLibraryIsolationAdvice/DoesNotRewriteTheCompilersOwnLine", [] {
    // "Reuse the channel, don't rewrite compiler output": the advice is
    // APPENDED text, and the original diagnostic line survives byte for byte
    // wherever a caller concatenates it (as both call sites do).
    const std::string compilerLine =
        "probe.c:1:10: fatal error: 'io.h' file not found\n";
    const std::string out = "clang -nostdlibinc -c probe.c\n" + compilerLine;
    auto advice = mcpp::build::graph_c_library_isolation_advice(out);
    expect(fatal(!advice.empty()));
    std::string combined = out + advice;
    expect(ne(combined.find(compilerLine), std::string::npos));
}};

// ═══ mcpp#533 / mcpp#534: emitter self-checks ══════════════════════════════

// ── A rule's command must begin with a program (mcpp#533) ──────────────────
//
// `c_shared` is `$cc -shared $in -o $out …`, and `cc` was emitted only when
// the COMPILE set held a C or asm unit — a property that says nothing about
// which LINK rules exist. A package with no sources still selected `c_shared`,
// `$cc` expanded to nothing, and `/bin/sh` was handed `-shared` as the program
// name. Twenty-six lines below `cc`, this file already carried the rule that
// prevents it, written for `c_ldflags` and never applied to its sibling.
//
// The checker is exported so it can be asserted against manifests
// `emit_ninja_string` is supposed never to produce. A test that could only
// reach it through a BuildPlan could not show the checker works at all.

UTest a_rule_whose_command_starts_with_an_undefined_variable_is_refused{"NinjaEmitterGuards/ARuleWhoseCommandStartsWithAnUndefinedVariableIsRefused", [] {
    const std::string manifest =
        "cxx = /usr/bin/g++\n"
        "\n"
        "rule c_shared\n"
        "  command = $cc -shared $in -o $out\n"
        "  description = SHARED $out\n";
    auto bad = check_rule_commands_name_a_program(manifest);
    expect(fatal(bad.has_value())) << manifest;
    if (auto&& failed = expect(ne(bad->find("c_shared"), std::string::npos)); !failed) failed << *bad;
    if (auto&& failed = expect(ne(bad->find("$cc"), std::string::npos)); !failed) failed << *bad;
}};

UTest a_rule_whose_leading_variable_is_defined_but_empty_is_refused{"NinjaEmitterGuards/ARuleWhoseLeadingVariableIsDefinedButEmptyIsRefused", [] {
    const std::string manifest =
        "cc =\n"
        "\n"
        "rule c_link\n"
        "  command = $cc $in -o $out\n";
    expect(check_rule_commands_name_a_program(manifest).has_value());
}};

// The negatives, which are what stop this from being an always-fires check.
// Note especially `$soname_flag`: variables set per-EDGE are legitimately
// undefined at the top level, and expanding them to nothing is a feature
// several rules depend on. That is why this guard is about the first token and
// not about undefined variables in general — the general form would have
// needed an allowlist, and a hand-maintained exception list is the shape this
// file has been burned by twice.
UTest well_formed_rules_and_optional_per_edge_variables_pass{"NinjaEmitterGuards/WellFormedRulesAndOptionalPerEdgeVariablesPass", [] {
    const std::string manifest =
        "cc = /usr/bin/gcc\n"
        "cxx = /usr/bin/g++\n"
        "\n"
        "rule c_shared\n"
        "  command = $cc -shared $in -o $out $soname_flag $unit_ldflags\n"
        "\n"
        "rule literal_tool\n"
        "  command = /usr/bin/strip $in\n"
        "\n"
        "rule rule_local\n"
        "  tool = /usr/bin/objcopy\n"
        "  command = $tool -O binary $in $out\n";
    auto bad = check_rule_commands_name_a_program(manifest);
    if (auto&& failed = expect(!bad.has_value()); !failed) failed << (bad ? *bad : std::string{});
}};

// The emitter's own output must satisfy its own guard — including the case
// that produced the defect: a plan whose compile set has no C or asm unit.
UTest emitted_manifests_always_name_a_program{"NinjaEmitterGuards/EmittedManifestsAlwaysNameAProgram", [] {
    auto plan = minimal_plan();
    auto ninja = emit_ninja_string(plan);
    auto bad = check_rule_commands_name_a_program(ninja);
    if (auto&& failed = expect(!bad.has_value()); !failed) failed << (bad ? *bad : std::string{});

    // `cc` unconditionally, not "when something C is being compiled".
    expect(ne(ninja.find("\ncc        = "), std::string::npos))
        << "cc is not defined in a plan with no C sources — this is mcpp#533\n"
        << ninja;
}};

// ── A package's compiles wait for its generators (mcpp#534) ────────────────

namespace {

// A plan with one package that generates a HEADER and compiles a source. The
// header is the whole point: a generated `.cpp` becomes an edge input and is
// ordered for free, a generated `.h` is reached through `-I` and never appears
// as an input at all.
BuildPlan plan_with_generated_header() {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/uses_header.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/uses_header.o",
        .packageName = "gen_pkg",
    });
    mcpp::manifest::BuildAction a;
    a.id = "gen:header";
    a.packageName = "gen_pkg";
    a.role = mcpp::manifest::BuildAction::Role::Source;
    a.command = {"/bin/true"};
    a.outputs = {"out/generated.h"};
    plan.actions.push_back(std::move(a));
    return plan;
}

}  // namespace

UTest a_generated_header_gets_a_phony_and_the_compile_edge_waits_for_it{"ActionOrdering/AGeneratedHeaderGetsAPhonyAndTheCompileEdgeWaitsForIt", [] {
    auto ninja = emit_ninja_string(plan_with_generated_header());

    expect(ne(ninja.find("build mcpp-actions-gen_pkg : phony"), std::string::npos))
        << ninja;
    expect(ne(ninja.find("|| mcpp-actions-gen_pkg"), std::string::npos))
        << "the compile edge does not wait for the generator\n" << ninja;
}};

// THE DENOMINATOR. "every edge that should carry the ordering does" is
// vacuously true when no edge does — and that vacuum is exactly the state this
// change fixes, so the count of edges is asserted separately from the count
// that carries it. A bare equality would have passed against the defect.
UTest every_compile_edge_of_that_package_carries_it{"ActionOrdering/EveryCompileEdgeOfThatPackageCarriesIt", [] {
    auto ninja = emit_ninja_string(plan_with_generated_header());

    const auto edges = count_occurrences(ninja, "build obj/uses_header.o :");
    const auto ordered =
        count_occurrences(ninja, "|| mcpp-actions-gen_pkg");
    expect(gt(edges, 0u)) << "no compile edge was emitted at all — the "
                            "assertion below would describe nothing\n" << ninja;
    expect(ge(ordered, edges)) << ninja;
}};

// A package with no gating action gets no phony, and its edges are untouched.
// Without this the change would read as "always add an order-only edge", which
// is not what it does and would serialise builds that have nothing to wait for.
UTest a_package_with_no_actions_is_unchanged{"ActionOrdering/APackageWithNoActionsIsUnchanged", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/plain.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/plain.o",
        .packageName = "plain_pkg",
    });
    auto ninja = emit_ninja_string(plan);
    expect(eq(ninja.find("mcpp-actions-plain_pkg"), std::string::npos)) << ninja;
}};

// `Object` and `Artifact` really are ordered by file dependency — their
// outputs are link inputs and their inputs are link outputs — so pulling them
// into the phony would add an edge that expresses nothing. This is the half of
// the original comment that was true, and it has to stay true.
UTest object_role_actions_do_not_join_the_compile_gate{"ActionOrdering/ObjectRoleActionsDoNotJoinTheCompileGate", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/x.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/x.o",
        .packageName = "obj_pkg",
    });
    mcpp::manifest::BuildAction a;
    a.id = "blob";
    a.packageName = "obj_pkg";
    a.role = mcpp::manifest::BuildAction::Role::Object;
    a.command = {"/bin/true"};
    a.outputs = {"out/blob.o"};
    plan.actions.push_back(std::move(a));

    auto ninja = emit_ninja_string(plan);
    expect(eq(ninja.find("mcpp-actions-obj_pkg"), std::string::npos)) << ninja;
}};

// A non-blocking check runs alongside compilation; a blocking one precedes it.
// `blocking` was typed, transported, parsed, documented in two languages and
// demonstrated in a shipped example — and read by nothing, which made it a
// no-op with a paper trail.
UTest blocking_decides_whether_a_check_gates_the_compile{"ActionOrdering/BlockingDecidesWhetherACheckGatesTheCompile", [] {
    auto make = [](bool blocking) {
        auto plan = minimal_plan();
        plan.compileUnits.push_back({
            .source = "src/c.cpp",
            .kind = mcpp::SourceKind::Cxx,
            .object = "obj/c.o",
            .packageName = "chk_pkg",
        });
        mcpp::manifest::BuildAction a;
        a.id = "tidy";
        a.packageName = "chk_pkg";
        a.role = mcpp::manifest::BuildAction::Role::Check;
        a.blocking = blocking;
        a.command = {"/bin/true"};
        a.outputs = {"out/tidy.stamp"};
        plan.actions.push_back(std::move(a));
        return emit_ninja_string(plan);
    };

    expect(ne(make(true).find("|| mcpp-actions-chk_pkg"), std::string::npos))
        << "blocking = true did not gate the compile";
    expect(eq(make(false).find("mcpp-actions-chk_pkg"), std::string::npos))
        << "blocking = false gated the compile anyway";
}};

// ── depfile: ninja must track what the action's COMMAND read, not only what
// the build program declared as an input ──────────────────────────────────
//
// `inputs` is fixed when build.mcpp runs, before the command has executed. A
// device-source compiler (glslangValidator, glslc, slangc, nvcc/clang)
// discovers its own `#include` graph only by parsing the source, and reports
// it afterward as a Make-style depfile. Without `depfile`/`deps = gcc` on the
// action's own rule, that graph reaches nothing: the compile edge tracks
// exactly the files build.mcpp named, and editing an `#include`d `.glsl` or
// `.cuh` reruns nothing.

namespace {

// Isolates ONE rule's own variable block (`rule NAME` up to the next blank
// line), because `cxx_object`/`cxx_module` unconditionally carry their own
// `depfile = $out.d` / `deps = gcc` for a plan's ordinary compile rules
// (mcpp#235/#257) — a search over the WHOLE manifest for either string would
// pass whether or not the action under test ever asked for a depfile.
std::string action_rule_block(std::string_view ninja, std::string_view rule_header) {
    auto start = ninja.find(rule_header);
    if (start == std::string_view::npos) return {};
    auto end = ninja.find("\n\n", start);
    auto count = end == std::string_view::npos ? std::string_view::npos : end - start;
    return std::string(ninja.substr(start, count));
}

}  // namespace

UTest a_declared_depfile_emits_depfile_and_deps_gcc{"ActionDepfile/ADeclaredDepfileEmitsDepfileAndDepsGcc", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/shader_user.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/shader_user.o",
        .packageName = "shader_pkg",
    });
    mcpp::manifest::BuildAction a;
    a.id = "shader";
    a.packageName = "shader_pkg";
    a.role = mcpp::manifest::BuildAction::Role::Source;
    a.command = {"/bin/true"};
    a.outputs = {"out/shader.spv"};
    a.depfile = "out/shader.spv.d";
    plan.actions.push_back(std::move(a));

    auto ninja = emit_ninja_string(plan);
    auto block = action_rule_block(ninja, "rule mcpp_action_0\n");
    expect(fatal(!block.empty())) << "no rule emitted for the action\n" << ninja;
    expect(ne(block.find("\n  depfile = out/shader.spv.d\n"), std::string::npos))
        << block;
    // No trailing "\n" required: `deps = gcc` is the last line this backend
    // emits for the rule before the blank separator, so the block extracted
    // above (up to, not including, that separator) ends exactly here.
    expect(ne(block.find("\n  deps = gcc"), std::string::npos)) << block;

    // The depfile must never also be a declared ninja OUTPUT of the same
    // edge: `deps = gcc` makes ninja consume and delete it once read, and a
    // file simultaneously promised as an output would be one ninja expects
    // to still exist afterward.
    expect(eq(ninja.find("shader.spv.d :"), std::string::npos))
        << "the depfile must not appear as a build edge's own output\n" << ninja;
}};

// THE CONTROL. Without it, a backend that emitted `depfile =`/`deps = gcc`
// unconditionally for every action would also pass the test above — exactly
// the shape `blocking` took before anything read it (mcpp#534 / e2e 315):
// typed, transported, parsed, and a no-op with a paper trail.
UTest no_depfile_emits_neither_line{"ActionDepfile/NoDepfileEmitsNeitherLine", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/plain_user.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/plain_user.o",
        .packageName = "plain_pkg2",
    });
    mcpp::manifest::BuildAction a;
    a.id = "plain";
    a.packageName = "plain_pkg2";
    a.role = mcpp::manifest::BuildAction::Role::Source;
    a.command = {"/bin/true"};
    a.outputs = {"out/plain.txt"};
    plan.actions.push_back(std::move(a));

    auto ninja = emit_ninja_string(plan);
    auto block = action_rule_block(ninja, "rule mcpp_action_0\n");
    expect(fatal(!block.empty())) << "no rule emitted for the action\n" << ninja;
    expect(eq(block.find("depfile ="), std::string::npos)) << block;
    expect(eq(block.find("deps = gcc"), std::string::npos)) << block;
}};

// A module-extension file need not provide a module: an implementation unit
// (`module M;`) is a legal inhabitant of a `.cppm` and provides nothing
// importable. The rule is chosen from the EXTENSION and the BMI binding comes
// from the SCAN, so the two disagree exactly there — and both flags that depend
// on the answer used to be rule-level constants applied to every edge alike.
//
// Measured on clang 22.1.8 before this changed:
//   -fmodule-output=   with no value: accepted, exit 0, no BMI written, and the
//                      failure surfaced at an unrelated consumer.
//   -x c++-module      on an implementation unit: `missing 'export' specifier
//                      in module declaration`. GCC's interface spelling is the
//                      plain language, so the identical project built on GCC
//                      and not on Clang.
UTest module_edge_flags_come_from_the_scan_not_the_extension{"NinjaBackend/ModuleEdgeFlagsComeFromTheScanNotTheExtension", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.toolchain.binaryPath = "/usr/bin/clang++";
    plan.compileUnits.push_back({
        .source = "src/iface.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/iface.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.back().providesModule = "iface";
    plan.compileUnits.push_back({
        .source = "src/impl.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/impl.o",
        .packageName = "objc_rule_test",
    });   // provides nothing: an implementation unit in a module extension

    auto ninja = emit_ninja_string(plan);

    // THE RULE STATES NEITHER FLAG. Holding this is what stops the fix from
    // being undone by someone restoring a rule-level constant that "works"
    // for every edge that happens to provide a module.
    auto rule_start = ninja.find("rule cxx_module\n");
    expect(fatal(ne(rule_start, std::string::npos))) << ninja;
    auto rule = ninja.substr(rule_start, ninja.find("\n\n", rule_start) - rule_start);
    expect(ne(rule.find("$module_output $module_lang"), std::string::npos)) << rule;
    expect(eq(rule.find("-fmodule-output=$bmi_out"), std::string::npos))
        << "the BMI flag is per-edge; a rule-level one reaches edges with no BMI";
    expect(eq(rule.find("-x c++-module"), std::string::npos))
        << "the interface spelling is per-edge, not a property of the rule";

    // EVERY cxx_module EDGE BINDS module_lang, AND THE DENOMINATOR IS ASSERTED.
    // A count-based check whose denominator is zero reads exactly like a pass.
    std::size_t edges = 0, with_lang = 0;
    for (std::size_t pos = 0; (pos = ninja.find(" : cxx_module ", pos)) != std::string::npos; ) {
        ++edges;
        auto end = ninja.find("\nbuild ", pos);
        if (end == std::string::npos) end = ninja.size();
        if (ninja.substr(pos, end - pos).find("\n  module_lang =") != std::string::npos)
            ++with_lang;
        pos = end;
    }
    expect(fatal(eq(edges, 2u))) << ninja;
    expect(eq(with_lang, edges)) << ninja;

    // The interface: told it is an interface, and given somewhere to put the BMI.
    expect(ne(ninja.find("  module_lang = -x c++-module\n"), std::string::npos)) << ninja;
    expect(ne(ninja.find("  module_output = -fmodule-output=pcm.cache/iface.pcm\n"), std::string::npos)) << ninja;
    // The implementation unit: told it is C++, and given no BMI path at all.
    expect(ne(ninja.find("  module_lang = -x c++\n"), std::string::npos)) << ninja;
    expect(eq(count_occurrences(ninja, "  module_output ="), 1u))
        << "only the unit that provides a module names a BMI";
    // The empty-value spelling must not appear anywhere in the graph.
    expect(eq(ninja.find("-fmodule-output= "), std::string::npos)) << ninja;
    expect(eq(ninja.find("-fmodule-output=\n"), std::string::npos)) << ninja;
}};

// GCC needs no explicit module output at all, so the same two units must emit
// `module_lang` twice and `module_output` never. Without this the test above
// would leave "the GCC branch still emits a flag it does not want" unmeasured.
UTest gcc_module_edges_carry_the_language_and_no_bmi_flag{"NinjaBackend/GccModuleEdgesCarryTheLanguageAndNoBmiFlag", [] {
    auto plan = minimal_plan();   // GCC
    plan.compileUnits.push_back({
        .source = "src/iface.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/iface.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.back().providesModule = "iface";
    plan.compileUnits.push_back({
        .source = "src/impl.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/impl.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);
    expect(eq(count_occurrences(ninja, "  module_lang = -x c++\n"), 2u)) << ninja;
    expect(eq(count_occurrences(ninja, "  module_output ="), 0u)) << ninja;
}};

// The third dialect, and the one where the empty value was worst. `/ifcOutput`
// takes its path as a SEPARATE token, so `/ifcOutput ` followed by nothing
// consumed whatever came next on the command line -- the observed failure was
// `could not open output file '/interface'`, naming a flag as a filename.
UTest msvc_module_edges_split_the_interface_flag_from_the_language_flag{"NinjaBackend/MsvcModuleEdgesSplitTheInterfaceFlagFromTheLanguageFlag", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath = "cl.exe";
    plan.compileUnits.push_back({
        .source = "src/iface.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/iface.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.back().providesModule = "iface";
    plan.compileUnits.push_back({
        .source = "src/impl.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/impl.o",
        .packageName = "objc_rule_test",
    });

    auto ninja = emit_ninja_string(plan);

    // `/interface /TP` says both "this is an interface" and "this is C++"; a
    // unit that is not an interface keeps only the second half.
    expect(ne(ninja.find("  module_lang = /interface /TP\n"), std::string::npos)) << ninja;
    expect(ne(ninja.find("  module_lang = /TP\n"), std::string::npos)) << ninja;
    expect(eq(count_occurrences(ninja, "  module_lang = /interface /TP\n"), 1u)) << ninja;
    // Exactly one edge names an .ifc, and it is the one that provides.
    expect(eq(count_occurrences(ninja, "  module_output = /ifcOutput "), 1u)) << ninja;
    // The trailing-space spelling with nothing after it must not exist.
    expect(eq(ninja.find("/ifcOutput \n"), std::string::npos)) << ninja;
}};

// ── #618: `windows_subsystem` / `windows_entry` ─────────────────────────────
//
// Every row of the rendering tables in the design record (§1.2, §1.3), stated
// against the function the emitter calls, and once through the emitted graph.

namespace {

BuildPlan plan_for_triple(std::string_view triple) {
    auto plan = minimal_plan();
    plan.toolchain.targetTriple = std::string(triple);
    return plan;
}

using Flags = std::vector<std::string>;

}  // namespace

UTest windows_keys_render_nothing_off_pe{"NinjaBackend/WindowsKeysRenderNothingOffPe", [] {
    for (auto triple : {"x86_64-linux-gnu", "x86_64-linux-musl", "aarch64-macos"}) {
        auto plan = plan_for_triple(triple);
        for (bool sep : {false, true})
            expect(eq(windows_executable_link_flags(plan, sep, "windows", "wWinMain"), Flags{}))
                << triple;
    }
}};

UTest windows_defaults_render_nothing_on_either_abi{"NinjaBackend/WindowsDefaultsRenderNothingOnEitherAbi", [] {
    for (auto triple : {"x86_64-windows-msvc", "x86_64-windows-gnu"}) {
        auto plan = plan_for_triple(triple);
        for (auto [subsystem, entry] : {std::pair{"", ""}, std::pair{"console", ""},
                                        std::pair{"", "main"}, std::pair{"console", "main"}})
            expect(eq(windows_executable_link_flags(plan, false, subsystem, entry), Flags{}))
                << triple << " " << subsystem << " " << entry;
    }
}};

UTest windows_msvc_abi_states_both_subsystem_and_crt_entry{"NinjaBackend/WindowsMsvcAbiStatesBothSubsystemAndCrtEntry", [] {
    auto plan = plan_for_triple("x86_64-windows-msvc");
    const std::tuple<std::string_view, std::string_view, Flags> rows[] = {
        {"windows", "",         {"/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup"}},
        {"windows", "main",     {"/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup"}},
        {"windows", "WinMain",  {"/SUBSYSTEM:WINDOWS", "/ENTRY:WinMainCRTStartup"}},
        {"windows", "wWinMain", {"/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup"}},
        {"windows", "wmain",    {"/SUBSYSTEM:WINDOWS", "/ENTRY:wmainCRTStartup"}},
        {"console", "wmain",    {"/SUBSYSTEM:CONSOLE", "/ENTRY:wmainCRTStartup"}},
        {"",        "WinMain",  {"/SUBSYSTEM:CONSOLE", "/ENTRY:WinMainCRTStartup"}},
    };
    for (auto const& [subsystem, entry, expected] : rows) {
        // link.exe invoked directly takes the flag bare...
        expect(eq(windows_executable_link_flags(plan, true, subsystem, entry), expected))
            << subsystem << " " << entry;
        // ...and a GNU-style driver (clang targeting the MSVC ABI) passes it on.
        Flags wrapped;
        for (auto const& f : expected) wrapped.push_back("-Wl," + f);
        expect(eq(windows_executable_link_flags(plan, false, subsystem, entry), wrapped))
            << subsystem << " " << entry;
    }
}};

UTest windows_gnu_abi_uses_the_driver_flags{"NinjaBackend/WindowsGnuAbiUsesTheDriverFlags", [] {
    auto plan = plan_for_triple("x86_64-windows-gnu");
    const std::tuple<std::string_view, std::string_view, Flags> rows[] = {
        {"windows", "",         {"-mwindows"}},
        {"windows", "main",     {"-mwindows"}},
        {"windows", "WinMain",  {"-mwindows"}},
        {"windows", "wWinMain", {"-mwindows", "-municode"}},
        {"console", "wmain",    {"-municode"}},
        {"",        "WinMain",  {}},
    };
    for (auto const& [subsystem, entry, expected] : rows)
        expect(eq(windows_executable_link_flags(plan, false, subsystem, entry), expected))
            << subsystem << " " << entry;
}};

// Through the emitted graph: the flags reach the declaring executable's link
// edge and no other edge, including a test binary of the same package.
UTest windows_subsystem_reaches_only_the_declaring_executable{"NinjaBackend/WindowsSubsystemReachesOnlyTheDeclaringExecutable", [] {
    auto plan = plan_for_triple("x86_64-windows-gnu");
    plan.compileUnits.push_back({
        .source = "src/gui.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/gui.o",
        .packageName = "objc_rule_test",
    });
    plan.compileUnits.push_back({
        .source = "src/cli.cpp",
        .kind = mcpp::SourceKind::Cxx,
        .object = "obj/cli.o",
        .packageName = "objc_rule_test",
    });
    LinkUnit gui{
        .targetName = "gui",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/gui.o"},
        .output = "bin/gui.exe",
        .entryMain = "src/gui.cpp",
    };
    gui.windowsSubsystem = "windows";
    plan.linkUnits.push_back(gui);
    plan.linkUnits.push_back({
        .targetName = "cli",
        .kind = mcpp::build::LinkUnit::Binary,
        .objects = {"obj/cli.o"},
        .output = "bin/cli.exe",
        .entryMain = "src/cli.cpp",
    });
    LinkUnit test{
        .targetName = "unit",
        .kind = mcpp::build::LinkUnit::TestBinary,
        .objects = {"obj/cli.o"},
        .output = "bin/unit.exe",
    };
    // A test binary never carries the fields (the manifest refuses them), and
    // even when a plan sets them the emitter does not render them.
    test.windowsSubsystem = "windows";
    plan.linkUnits.push_back(test);

    auto ninja = emit_ninja_string(plan);
    expect(eq(count_occurrences(ninja, "-mwindows"), 1u)) << ninja;
    const auto guiEdge = ninja.find("build bin/gui.exe");
    expect(fatal(ne(guiEdge, std::string::npos))) << ninja;
    const auto nextEdge = ninja.find("\nbuild ", guiEdge + 1);
    const auto flag = ninja.find("-mwindows");
    expect(gt(flag, guiEdge)) << ninja;
    expect(lt(flag, nextEdge)) << ninja;
}};

// #694 (W1d): THE LEVEL A PROFILE DECLARES IS THE LEVEL EVERY ROW REALISES.
//
// A branch keyed on the target triple once replaced every non-zero level with
// `-Og` on `*-linux-musl`, for GCC and clang alike, while the `Finished` line
// still said `[optimized]`; it set the level of mcpp's own Linux release
// binaries. The walk covers every row of the target table with both GNU-dialect
// compilers, so a later branch that overrides a declared level fails here and
// not in a published binary. `realised_opt_level` is the answer both the flags
// and the `Finished` line read, so it is asserted against the same rows.
UTest every_target_row_realises_the_declared_level{"NinjaBackendOptimization/EveryTargetRowRealisesTheDeclaredLevel", [] {
    using mcpp::toolchain::CompilerId;
    for (auto const& row : mcpp::toolchain::triple::kKnownTargets) {
        for (auto compiler : {CompilerId::GCC, CompilerId::Clang}) {
            for (std::string level : {"0", "1", "2", "3", "s"}) {
                auto plan = minimal_plan();
                plan.toolchain.targetTriple = std::string(row.canonical);
                plan.toolchain.compiler = compiler;
                plan.toolchain.binaryPath = compiler == CompilerId::Clang
                    ? "/usr/bin/clang++" : "/usr/bin/g++";
                plan.manifest.buildConfig.optLevel = level;

                auto f = compute_flags(plan);
                const std::string want = " -O" + level;
                const std::string where = std::format(
                    "target={} compiler={} opt={}", row.canonical,
                    static_cast<int>(compiler), level);
                expect(ne(f.cxx.find(want), std::string::npos)) << where << "\n" << f.cxx;
                expect(ne(f.cc.find(want), std::string::npos)) << where << "\n" << f.cc;
                expect(eq(f.cxx.find(" -Og"), std::string::npos)) << where << "\n" << f.cxx;
                expect(eq(f.cc.find(" -Og"), std::string::npos)) << where << "\n" << f.cc;
                expect(eq(realised_opt_level(plan.manifest.buildConfig), level)) << where;
                expect(eq(realises_optimization(plan.manifest.buildConfig), level != "0"))
                    << where;
            }
        }
    }
}};

// The MSVC dialect spells the same levels its own way: `/Od` for zero (there is
// no `/O0`) and `/O<n>` otherwise.
UTest msvc_dialect_realises_the_declared_level{"NinjaBackendOptimization/MsvcDialectRealisesTheDeclaredLevel", [] {
    for (std::string level : {"0", "1", "2"}) {
        auto plan = minimal_plan();
        plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
        plan.toolchain.binaryPath = "cl.exe";
        plan.toolchain.targetTriple = "x86_64-pc-windows-msvc";
        plan.manifest.buildConfig.optLevel = level;
        auto f = compute_flags(plan);
        const std::string want = level == "0" ? " /Od" : " /O" + level;
        expect(ne(f.cxx.find(want), std::string::npos)) << level << "\n" << f.cxx;
        expect(ne(f.cc.find(want), std::string::npos)) << level << "\n" << f.cc;
    }
}};

// An empty level is zero, for the flags and for the `Finished` line alike. The
// old spelling rendered it as a bare `-O`, which GCC reads as `-O1`, while the
// line called the build unoptimized.
UTest an_empty_level_is_zero_for_flags_and_descriptor{"NinjaBackendOptimization/AnEmptyLevelIsZeroForFlagsAndDescriptor", [] {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.optLevel.clear();
    auto f = compute_flags(plan);
    expect(ne(f.cxx.find(" -O0"), std::string::npos)) << f.cxx;
    expect(eq(f.cxx.find(" -O "), std::string::npos)) << f.cxx;
    expect(eq(realised_opt_level(plan.manifest.buildConfig), "0"));
    expect(!realises_optimization(plan.manifest.buildConfig));
}};

// #695 and #690 (C2): THE FILE-LEVEL FLAGS CARRY NOTHING A PACKAGE DECLARES FOR
// ITSELF.
//
// Every unit of every package reads `$cflags` and `$cxxflags`, so a value placed
// there is broadcast to the whole graph. The root's include directories rode
// that channel until #691, and the root's C standard until #695. The root here
// declares every package-private key; none of those values may appear in the
// file-level flags. The C standard there is the engine's constant.
UTest the_file_level_flags_carry_no_package_private_value{"NinjaBackendScope/TheFileLevelFlagsCarryNoPackagePrivateValue", [] {
    auto plan = minimal_plan();
    auto& bc = plan.manifest.buildConfig;
    bc.cStandard = "gnu11";
    bc.includeDirs = {"/root-private/include"};
    bc.privateIncludeDirs = {"/root-private/include"};
    bc.cflags = {"-DROOT_ONLY_CFLAG=1"};
    bc.cxxflags = {"-DROOT_ONLY_CXXFLAG=1"};
    bc.defines = {"ROOT_ONLY_DEFINE=1"};

    auto f = compute_flags(plan);
    for (auto const* line : {&f.cc, &f.cxx}) {
        if (auto&& failed = expect(eq(line->find("gnu11"), std::string::npos)); !failed) failed << *line;
        if (auto&& failed = expect(eq(line->find("/root-private/include"), std::string::npos)); !failed) failed << *line;
        if (auto&& failed = expect(eq(line->find("ROOT_ONLY"), std::string::npos)); !failed) failed << *line;
    }
    expect(ne(f.cc.find("-std=c11"), std::string::npos)) << f.cc;
}};

// #696 (W2b): AN ELF LINK OVER A GRAPH-SUPPLIED C LIBRARY SEARCHES NOTHING OF
// THE HOST'S. With no sysroot, clang derived its library search from `/` and,
// on x86_64, from the host's GCC; the link now names an empty directory inside
// the build as its sysroot.
namespace {
BuildPlan graph_c_library_plan(std::string_view triple,
                               mcpp::toolchain::CompilerId compiler) {
    auto plan = minimal_plan();
    plan.toolchain.targetTriple = std::string(triple);
    plan.toolchain.compiler = compiler;
    plan.toolchain.binaryPath = compiler == mcpp::toolchain::CompilerId::Clang
        ? "/usr/bin/clang++" : "/usr/bin/g++";
    plan.targetSide.kernelAbi = { mcpp::targetside::Origin::Graph, "openkal",
                                  "openkal-linux@0.15.0", false };
    plan.targetSide.cAbi = { mcpp::targetside::Origin::Graph, "musl",
                             "openkal-musl@0.19.2", false };
    plan.targetSide.cxx = { mcpp::targetside::Origin::Graph, "libc++",
                            "openkal-llvm-runtime@0.15.2", false };
    return plan;
}
}  // namespace

UTest an_elf_graph_link_by_clang_carries_the_empty_sysroot{"NinjaBackendGraphLink/AnElfGraphLinkByClangCarriesTheEmptySysroot", [] {
    for (auto triple : {"x86_64-linux-musl", "aarch64-linux-musl"}) {
        auto plan = graph_c_library_plan(triple, mcpp::toolchain::CompilerId::Clang);
        auto f = compute_flags(plan);
        expect(bool(f.graphLinkIsolated)) << triple;
        expect(ne(f.ld.find("--sysroot="), std::string::npos)) << triple << "\n" << f.ld;
        expect(ne(f.ld.find(std::string(kGraphLinkSysrootDir)), std::string::npos))
            << triple << "\n" << f.ld;
    }
}};

// The measured case is clang on ELF; GCC over the graph and PE links keep their
// line until they are measured, and a payload C library keeps its own sysroot.
UTest other_links_do_not_carry_the_empty_sysroot{"NinjaBackendGraphLink/OtherLinksDoNotCarryTheEmptySysroot", [] {
    {
        auto plan = graph_c_library_plan("x86_64-linux-musl", mcpp::toolchain::CompilerId::GCC);
        auto f = compute_flags(plan);
        expect(!f.graphLinkIsolated);
        expect(eq(f.ld.find(std::string(kGraphLinkSysrootDir)), std::string::npos)) << f.ld;
    }
    {
        auto plan = graph_c_library_plan("x86_64-windows-musl", mcpp::toolchain::CompilerId::Clang);
        auto f = compute_flags(plan);
        expect(!f.graphLinkIsolated);
        expect(eq(f.ld.find(std::string(kGraphLinkSysrootDir)), std::string::npos)) << f.ld;
    }
    {
        auto plan = minimal_plan();
        auto f = compute_flags(plan);
        expect(!f.graphLinkIsolated);
        expect(eq(f.ld.find(std::string(kGraphLinkSysrootDir)), std::string::npos)) << f.ld;
    }
}};

// #696: the note for an unanswered `-l` on an isolated graph link.
UTest names_the_library_and_the_musl_release{"GraphLinkLibraryAdvice/NamesTheLibraryAndTheMuslRelease", [] {
    const std::string out =
        "FAILED: bin/app\n"
        "clang++ @bin/app.rsp -o bin/app --sysroot=/p/target/x/graph-sysroot -lm -lpthread\n"
        "ld.lld: error: unable to find library -lm\n"
        "ld.lld: error: unable to find library -lpthread\n"
        "ld.lld: error: unable to find library -lm\n";
    auto advice = graph_link_library_advice(out, "musl", "openkal-musl@0.19.1");
    expect(ne(advice.find("musl (openkal-musl@0.19.1)"), std::string::npos)) << advice;
    expect(ne(advice.find("-lm, -lpthread"), std::string::npos)) << advice;
    expect(ne(advice.find("openkal-musl 0.19.2"), std::string::npos)) << advice;
    expect(ne(advice.find("mcpp#696"), std::string::npos)) << advice;
}};

UTest another_library_gets_no_musl_paragraph{"GraphLinkLibraryAdvice/AnotherLibraryGetsNoMuslParagraph", [] {
    const std::string out =
        "clang++ -o bin/app --sysroot=/p/target/x/graph-sysroot -lz\n"
        "ld.lld: error: unable to find library -lz\n";
    auto advice = graph_link_library_advice(out, "musl", "openkal-musl@0.19.2");
    expect(ne(advice.find("-lz"), std::string::npos)) << advice;
    expect(eq(advice.find("openkal-musl 0.19.2 and later"), std::string::npos)) << advice;
}};

UTest stays_silent_without_the_graph_sysroot{"GraphLinkLibraryAdvice/StaysSilentWithoutTheGraphSysroot", [] {
    const std::string out =
        "clang++ -o bin/app -lm\n"
        "ld.lld: error: unable to find library -lm\n";
    expect(graph_link_library_advice(out, "musl", "openkal-musl@0.19.1").empty());
    expect(graph_link_library_advice(
        "clang++ --sysroot=/p/graph-sysroot\nld.lld: error: undefined symbol: foo\n").empty());
}};

// ─── #693 (W4d): the encodings the tools read ───────────────────────────────
//
// cl.exe, link.exe and lib.exe read a response file as UTF-8 only when it
// begins with a byte order mark, and in the ANSI code page otherwise (measured
// on windows-latest; see the emitter). Every response file of the msvc dialect
// begins with one; a GNU driver would read the mark as part of its first
// argument, so no response file of the gnu dialect has one.

namespace {

// The `rspfile_content` line of `rule`, or empty when the rule has none.
std::string rsp_line(const std::string& ninja, std::string_view rule) {
    auto start = ninja.find(rule);
    if (start == std::string::npos) return {};
    auto end = ninja.find("\n\n", start);
    auto body = ninja.substr(start, end - start);
    auto at = body.find("rspfile_content =");
    if (at == std::string::npos) return {};
    return body.substr(at, body.find('\n', at) - at);
}

}  // namespace

UTest msvc_response_files_begin_with_the_byte_order_mark{"NinjaBackendEncoding/MsvcResponseFilesBeginWithTheByteOrderMark", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.binaryPath = "cl.exe";
    plan.toolchain.targetTriple = "x86_64-pc-windows-msvc";
    plan.compileUnits.push_back({
        .source = "src/m.cppm",
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = "obj/m.o",
        .packageName = "objc_rule_test",
        .providesModule = "m",
    });
    plan.compileUnits.push_back({
        .source = "src/a.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/a.o",
        .packageName = "objc_rule_test",
    });
    auto ninja = emit_ninja_string(plan);
    for (std::string_view rule : {"rule c_object\n", "rule cxx_scan\n",
                                  "rule cxx_link\n", "rule cxx_archive\n",
                                  "rule cxx_shared\n"}) {
        auto line = rsp_line(ninja, rule);
        expect(fatal(!line.empty())) << rule << ninja;
        expect(ne(line.find("= \xEF\xBB\xBF"), std::string::npos)) << rule << line;
    }
}};

UTest gnu_response_files_carry_no_byte_order_mark{"NinjaBackendEncoding/GnuResponseFilesCarryNoByteOrderMark", [] {
    auto plan = minimal_plan();
    plan.compileUnits.push_back({
        .source = "src/a.c",
        .kind = mcpp::SourceKind::C,
        .object = "obj/a.o",
        .packageName = "objc_rule_test",
    });
    auto ninja = emit_ninja_string(plan);
    expect(!(rsp_line(ninja, "rule cxx_link\n").empty())) << ninja;
    expect(eq(ninja.find("\xEF\xBB\xBF"), std::string::npos)) << ninja;
}};

// Ninja reports the encoding it reads build.ninja in; mcpp writes UTF-8.
UTest a_ninja_that_reads_utf8_passes{"NinjaBackendEncoding/ANinjaThatReadsUtf8Passes", [] {
    expect(!(ninja_encoding_mismatch("Build file encoding: UTF-8\r\n", 65001,
                                         "ninja.exe").has_value()));
    expect(!(ninja_encoding_mismatch("Build file encoding: UTF-8\n", 1252,
                                         "ninja.exe").has_value()));
}};

UTest a_ninja_that_reads_ansi_is_refused_by_name{"NinjaBackendEncoding/ANinjaThatReadsAnsiIsRefusedByName", [] {
    auto modern = ninja_encoding_mismatch("Build file encoding: ANSI\r\n", 65001,
                                          "C:/tools/ninja.exe");
    expect(fatal(modern.has_value()));
    if (auto&& failed = expect(ne(modern->find("C:/tools/ninja.exe"), std::string::npos)); !failed) failed << *modern;
    if (auto&& failed = expect(ne(modern->find("ANSI"), std::string::npos)); !failed) failed << *modern;
    if (auto&& failed = expect(ne(modern->find("1.11"), std::string::npos)); !failed) failed << *modern;

    // On a host that ignores the declaration the advice names the code page.
    auto legacy = ninja_encoding_mismatch("Build file encoding: ANSI\n", 936,
                                          "ninja.exe");
    expect(fatal(legacy.has_value()));
    if (auto&& failed = expect(ne(legacy->find("936"), std::string::npos)); !failed) failed << *legacy;
}};

UTest output_that_names_no_encoding_decides_nothing{"NinjaBackendEncoding/OutputThatNamesNoEncodingDecidesNothing", [] {
    expect(!(ninja_encoding_mismatch("", 65001, "ninja").has_value()));
    expect(!(ninja_encoding_mismatch("ninja: error: unknown tool 'wincodepage'",
                                         65001, "ninja").has_value()));
}};

UTest pe_exports_patterns_and_a_consumer_reach_the_def_edge{"NinjaBackend/PeExportsPatternsAndAConsumerReachTheDefEdge", [] {
    auto plan = minimal_plan();
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.toolchain.targetTriple = "x86_64-pc-windows-msvc";
    plan.toolchain.binaryPath = "/native/bin/cl.exe";
    plan.outputDir = std::filesystem::temp_directory_path() / "mcpp-ninja-pe-exports";
    LinkUnit dll;
    dll.kind = LinkUnit::SharedLibrary;
    dll.targetName = "probe";
    dll.output = "bin/probe.dll";
    dll.importLibrary = "bin/probe.lib";
    dll.defFile = "bin/probe.def";
    dll.exportPatterns = {"probe_*"};
    dll.objects = {"obj/probe.o"};
    plan.linkUnits.push_back(dll);
    auto alone = emit_ninja_string(plan);
    expect(ne(alone.find("build bin/probe.def : coff_def obj/probe.o | obj/probe.exports.gen"), std::string::npos)) << alone;
    expect(ne(alone.find("--exports-file obj/probe.exports.gen"), std::string::npos));
    expect(eq(alone.find("--required"), std::string::npos));
    std::ifstream in(plan.outputDir / "obj" / "probe.exports.gen");
    std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(eq(written, "probe_*\n"));

    LinkUnit consumer;
    consumer.kind = LinkUnit::Binary;
    consumer.targetName = "app";
    consumer.output = "bin/app.exe";
    consumer.objects = {"obj/app.o"};
    consumer.implicitInputs = {"bin/probe.dll", "bin/probe.lib"};
    plan.linkUnits.push_back(consumer);
    expect(ne(emit_ninja_string(plan).find("--required"), std::string::npos));
    std::error_code ec;
    std::filesystem::remove_all(plan.outputDir, ec);
}};

// `trim_paths` PER PACKAGE (mcpp#844). The map of the whole plan sat in the
// flags every unit shares, so a dependency's command line named the roots of
// whichever members the plan built, and switching members recompiled it. A
// unit now carries its own package's map, and the shared flags carry none.
UTest a_trim_map_is_the_units_packages_and_no_other{"NinjaBackend/ATrimMapIsTheUnitsPackagesAndNoOther", [] {
    auto plan = minimal_plan();
    plan.manifest.buildConfig.trimPaths = true;
    const std::filesystem::path build = "/w/target/x";
    const std::filesystem::path core  = "/w/core";
    const std::filesystem::path app   = "/w/app";
    plan.trimMap = {{core, "core@0.1.0"}, {app, "app@0.1.0"}, {build, "/mcpp/build"}};
    plan.packageTrimMaps["core"] = {{core, "core@0.1.0"}, {build, "/mcpp/build"}};
    plan.packageTrimMaps["app"]  = {{core, "core@0.1.0"}, {app, "app@0.1.0"}, {build, "/mcpp/build"}};
    plan.compileUnits.push_back({.source = "/w/core/src/core.cpp", .kind = mcpp::SourceKind::Cxx,
                                 .object = "obj/core.o", .packageName = "core"});
    plan.compileUnits.push_back({.source = "/w/app/src/main.cpp", .kind = mcpp::SourceKind::Cxx,
                                 .object = "obj/main.o", .packageName = "app"});

    const auto flags = compute_flags(plan);
    expect(eq(flags.cxx.find("file-prefix-map"), std::string::npos)) << flags.cxx;
    expect(eq(flags.cc.find("file-prefix-map"), std::string::npos)) << flags.cc;

    const auto ninja = emit_ninja_string(plan);
    auto edge = [&](std::string_view object) {
        const auto at = ninja.find(std::format("build {} :", object));
        expect(ne(at, std::string::npos)) << ninja;
        return ninja.substr(at, ninja.find("\nbuild ", at + 1) - at);
    };
    const auto coreEdge = edge("obj/core.o");
    expect(ne(coreEdge.find("-ffile-prefix-map=/w/core=core@0.1.0"), std::string::npos)) << coreEdge;
    expect(ne(coreEdge.find("-ffile-prefix-map=/w/target/x=/mcpp/build"), std::string::npos)) << coreEdge;
    expect(eq(coreEdge.find("/w/app"), std::string::npos)) << "a consumer's root reached its dependency: " << coreEdge;
    const auto appEdge = edge("obj/main.o");
    expect(ne(appEdge.find("-ffile-prefix-map=/w/app=app@0.1.0"), std::string::npos)) << appEdge;
    expect(ne(appEdge.find("-ffile-prefix-map=/w/core=core@0.1.0"), std::string::npos)) << appEdge;
}};

// The map of a package is the fixed roots and its closure's roots, in the
// order of the whole plan's map: shortest prefix first, so that a driver
// trying the last mapping first takes the longest match.
UTest a_trim_map_keeps_the_fixed_roots_and_the_marked_packages{"NinjaBackend/ATrimMapKeepsTheFixedRootsAndTheMarkedPackages", [] {
    // Absolute on every host: `trim_map` maps absolute prefixes only, and a
    // path without a volume is not absolute on Windows.
    const auto w = std::filesystem::current_path().root_path() / "w";
    mcpp::toolchain::Toolchain tc;
    tc.binaryPath = std::filesystem::current_path().root_path() / "opt" / "tc" / "bin" / "g++";
    std::vector<mcpp::modgraph::PackageRoot> packages(3);
    packages[0].root = w / "app";  packages[0].manifest.package.name = "app";  packages[0].manifest.package.version = "1.0.0";
    packages[1].root = w / "core"; packages[1].manifest.package.name = "core"; packages[1].manifest.package.version = "0.1.0";
    packages[2].root = w / "gui";  packages[2].manifest.package.name = "gui";  packages[2].manifest.package.version = "0.2.0";

    const auto all = trim_map(packages, tc, w / "target" / "build-dir", {});
    const auto core = trim_map(packages, tc, w / "target" / "build-dir", {}, {0, 1, 0});
    auto names = [](auto const& m) {
        std::vector<std::string> out;
        for (auto const& [from, to] : m) out.push_back(to);
        return out;
    };
    const auto coreNames = names(core);
    expect(ne(std::ranges::find(coreNames, "core@0.1.0"), coreNames.end()));
    expect(ne(std::ranges::find(coreNames, "/mcpp/toolchain"), coreNames.end()));
    expect(ne(std::ranges::find(coreNames, "/mcpp/build"), coreNames.end()));
    expect(eq(std::ranges::find(coreNames, "app@1.0.0"), coreNames.end()));
    expect(eq(std::ranges::find(coreNames, "gui@0.2.0"), coreNames.end()));
    expect(eq(names(all).size(), coreNames.size() + 2));
    for (std::size_t i = 1; i < core.size(); ++i)
        expect(le(core[i - 1].first.native().size(), core[i].first.native().size()));
}};

// Design 2026-10-26 §24.4 item 9: the entry symbol of a freestanding image, and
// the `--defsym` that makes an undefined one a link error, through the driver
// and to ld.lld itself.
UTest start_renders_the_entry_and_its_definition_check{"NinjaBackend/StartRendersTheEntryAndItsDefinitionCheck", [] {
    using mcpp::build::start_link_flags;
    using Flags = std::vector<std::string>;
    expect(eq(start_link_flags("", false), Flags{}));
    expect(eq(start_link_flags("_reset", false), (Flags{"-Wl,-e,_reset", "-Wl,--defsym=__mcpp_start=_reset"})));
    expect(eq(start_link_flags("_reset", true), (Flags{"-e", "_reset", "--defsym=__mcpp_start=_reset"})));
}};

UTest start_failure_advice_names_the_statement{"NinjaBackend/StartFailureAdviceNamesTheStatement", [] {
    using mcpp::build::start_failure_advice;
    const std::string lld = "ld.lld: error: --defsym:1: symbol not found: _missing\n";
    const auto named = start_failure_advice(lld, {{"_missing", "[targets.k] start = \"_missing\""}});
    expect(ne(named.find("It was named by [targets.k] start = \"_missing\""), std::string::npos)) << named;
    const auto generic = start_failure_advice(lld, {});
    expect(ne(generic.find("`_missing`"), std::string::npos)) << generic;
    expect(ne(generic.find("mcpp:link-start"), std::string::npos)) << generic;
    expect(eq(start_failure_advice("undefined symbol: foo", {}), ""));
}};
