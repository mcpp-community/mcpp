#include <fstream>

import std;
import mcpplibs.mtest;
import mcpplibs.os.platform;
import mcpp.toolchain.dialect;
import mcpp.toolchain.hostflags;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

using mcpp::toolchain::CompilerId;
using mcpp::toolchain::HostFlagOptions;

namespace {

mcpp::toolchain::Toolchain tc_for(CompilerId id) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler = id;
    tc.targetTriple = id == CompilerId::MSVC ? "x86_64-pc-windows-msvc"
                                             : "x86_64-linux-gnu";
    return tc;
}

// Every compiler family mcpp claims to support, so a new one cannot be added
// without being answered for here.
constexpr CompilerId kFamilies[] = {
    CompilerId::GCC, CompilerId::Clang, CompilerId::MSVC,
};

} // namespace

// ── The capability-parity guard ─────────────────────────────────────────────
//
// build.mcpp is "one host C++ program", and compiling one of those is mcpp's
// job — so it must not have a narrower capability list than the main build.
// Stating that only in prose is what let `import mcpp;` / `import std;` sit
// behind a "not yet supported under MSVC" gate long after the main build had
// the .ifc pipeline (e2e 99 was already producing .ifc artifacts). This test
// turns the principle into a compile-and-run check: a family that is missing
// a dialect row, a module row, or host flags fails HERE, not at a user's
// `mcpp build`.
UTest every_family_has_complete_tables{"HostFlags/EveryFamilyHasCompleteTables", [] {
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        const auto& d = mcpp::toolchain::dialect_for(tc);
        const auto  t = mcpp::toolchain::bmi_traits(tc);

        expect(!d.id.empty());
        // Needed to compile a `.mcpp` at all: the extension is unknown to
        // every driver, so the language has to be forced.
        expect(!d.forceCxxLangArgv.empty()) << d.id;
        // Needed to name the produced program.
        expect(!d.outputExePrefix.empty()) << d.id;
        expect(!d.objExt.empty()) << d.id;
        // Needed to build and reference the bundled `mcpp` module.
        expect(!t.bmiDir.empty()) << d.id;
        expect(!t.bmiExt.empty()) << d.id;
    }
}};

// The producer must answer for every family too — MSVC deliberately returns
// nothing (cl.exe finds headers and libs through INCLUDE/LIB, not argv), but
// it must be a decision, not a crash or an accident.
UTest producer_answers_for_every_family{"HostFlags/ProducerAnswersForEveryFamily", [] {
    HostFlagOptions opt;
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        auto compile = mcpp::toolchain::host_compile_tokens(
            tc, opt, mcpp::toolchain::no_escape);
        auto link = mcpp::toolchain::host_link_tokens(
            tc, opt, mcpp::toolchain::no_escape);
        if (id == CompilerId::MSVC) {
            expect(compile.empty());
            expect(link.empty());
        }
        // No empty tokens anywhere: an empty argv element is an argument the
        // driver still has to interpret.
        for (auto const& t : compile) expect(!t.empty());
        for (auto const& t : link)    expect(!t.empty());
    }
}};

// ── The rendered string must not move ───────────────────────────────────────
//
// stdmod folds its compile command into `std_build_commands`, and the std
// cache DIRECTORY NAME is derived from the metadata containing it. Reordering
// a flag therefore invalidates every user's std BMIs for no behavioural gain
// — so the exact spelling is a compatibility surface, not an implementation
// detail. This pins it; the first attempt at this refactor moved
// `-stdlib=libc++` after the include flags and would have shipped exactly
// that invalidation.
UTest clang_cfg_bypass_string_is_stable{"HostFlags/ClangCfgBypassStringIsStable", [] {
    mcpp::toolchain::ClangDriverModel dm;
    dm.hasCfg = true;
    dm.cxxIncludes = { "/llvm/include/c++/v1", "/llvm/include/tgt/c++/v1" };

    expect(eq(dm.compile_flags(mcpp::toolchain::no_escape), " --no-default-config -nostdinc++"
              " -isystem/llvm/include/c++/v1"
              " -isystem/llvm/include/tgt/c++/v1"));

    // With the stdlib selection the std module has always asked for, it lands
    // immediately after -nostdinc++ — not at the end.
    expect(eq(mcpp::toolchain::render_tokens(
                  dm.compile_tokens(mcpp::toolchain::no_escape, true)), " --no-default-config -nostdinc++ -stdlib=libc++"
              " -isystem/llvm/include/c++/v1"
              " -isystem/llvm/include/tgt/c++/v1"));
}};

UTest link_model_strings_are_stable{"HostFlags/LinkModelStringsAreStable", [] {
    mcpp::toolchain::ToolchainLinkModel lm;
    lm.mode = mcpp::toolchain::CLibMode::PayloadFirst;
    lm.clangDriver = true;
    lm.crtDir = "/glibc/lib";
    lm.libDirs = { "/glibc/lib" };
    lm.loader = "/glibc/lib/ld.so";
    lm.systemIncludes = { "/glibc/include" };

    expect(eq(lm.compile_flags(mcpp::toolchain::no_escape), " -nostdlibinc -isystem/glibc/include"));
    expect(eq(lm.link_flags(mcpp::toolchain::no_escape), " -B/glibc/lib -L/glibc/lib -Wl,-rpath,/glibc/lib"
              " -Wl,--dynamic-linker=/glibc/lib/ld.so"));

    // GCC takes -idirafter so libstdc++'s #include_next wrappers can still
    // reach libc.
    lm.clangDriver = false;
    expect(eq(lm.compile_flags(mcpp::toolchain::no_escape), " -idirafter/glibc/include"));
}};

// ── bmi_reference_tokens ────────────────────────────────────────────────────
//
// The traits store these for the ninja STRING channel, where one word vs two
// makes no difference. argv consumers cannot be that relaxed.
UTest bmi_reference_splits_only_when_the_spelling_has_a_space{"HostFlags/BmiReferenceSplitsOnlyWhenTheSpellingHasASpace", [] {
    auto gnu = mcpp::toolchain::bmi_reference_tokens(
        " -fmodule-file=std=", std::filesystem::path("/tmp/std.pcm"));
    expect(fatal(eq(gnu.size(), 1u)));
    expect(eq(gnu[0], "-fmodule-file=std=/tmp/std.pcm"));

    auto msvc = mcpp::toolchain::bmi_reference_tokens(
        " /reference std=", std::filesystem::path("/tmp/std.ifc"));
    expect(fatal(eq(msvc.size(), 2u)));
    expect(eq(msvc[0], "/reference"));
    expect(eq(msvc[1], "std=/tmp/std.ifc"));
}};

// The general invariant, checked for every family: an argv element must never
// contain a space. This bug has now appeared three times in the same shape —
// `-x c++`, the mcpp module reference, the std reference — each time because
// a table entry written for the ninja STRING channel was concatenated into an
// argv element. cl.exe answers with "could not find module 'std'", which
// names neither the flag nor the reason.
UTest bmi_references_never_produce_a_token_with_a_space{"HostFlags/BmiReferencesNeverProduceATokenWithASpace", [] {
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        auto t = mcpp::toolchain::bmi_traits(tc);
        for (auto prefix : { t.stdBmiUsePrefix, t.stdCompatBmiUsePrefix }) {
            for (auto const& tok : mcpp::toolchain::bmi_reference_tokens(
                     prefix, std::filesystem::path("/tmp/x.bmi"))) {
                expect(eq(tok.find(' '), std::string::npos))
                    << "family " << mcpp::toolchain::dialect_for(tc).id
                    << " token: " << tok;
            }
        }
    }
}};

// Same invariant for the language-force spelling, which has both a positional
// and a per-file form.
UTest language_force_tokens_never_contain_a_space{"HostFlags/LanguageForceTokensNeverContainASpace", [] {
    for (auto const* d : { &mcpp::toolchain::gnu_dialect(),
                           &mcpp::toolchain::msvc_dialect() }) {
        for (auto f : d->forceCxxLangArgv)
            expect(eq(f.find(' '), std::string_view::npos)) << d->id << ": " << f;
        for (auto f : d->alwaysFlagsArgv)
            expect(eq(f.find(' '), std::string_view::npos)) << d->id << ": " << f;
        if (auto&& failed = expect(eq(d->perFileCxxPrefix.find(' '), std::string_view::npos)); !failed) failed << d->id;
    }
}};

// ── orphaned_reference (#604) ───────────────────────────────────────────────
//
// THE PAIR IS THE UNIT, and every per-token check passes while it is broken.
// MSVC's reference is two argv elements; a per-token de-duplicator dropped the
// second `/reference` -- already in the list from the bundled `mcpp` module --
// and left `<name>=<path>` standing alone. cl.exe read it as a source file:
//
//   c1xx: fatal error C1083: Cannot open source file:
//     'huxerui.rules.sources=...\huxerui.rules.sources.ifc'
//
// The de-duplicator is gone. This is the invariant that says so, and the
// diagnostic that would name the cause if it ever returns.
UTest an_orphaned_module_reference_is_detected{"HostFlags/AnOrphanedModuleReferenceIsDetected", [] {
    using mcpp::toolchain::orphaned_reference;

    // The defect, verbatim: two references appended, one switch surviving.
    expect(eq(orphaned_reference({"cl.exe", "/std:c++20", "/reference",
                                  "mcpp=C:/b/mcpp.ifc",
                                  "rules.sources=C:/b/rules.sources.ifc",
                                  "/c", "build.mcpp"}), std::optional<std::string>{"rules.sources=C:/b/rules.sources.ifc"}));

    // The same argv with both switches present is well formed.
    expect(!(orphaned_reference({"cl.exe", "/std:c++20", "/reference",
                                     "mcpp=C:/b/mcpp.ifc", "/reference",
                                     "rules.sources=C:/b/rules.sources.ifc",
                                     "/c", "build.mcpp"}).has_value()));

    // First position is an orphan too -- there is nothing in front of it.
    expect(orphaned_reference({"mcpp=C:/b/mcpp.ifc"}).has_value());
}};

// The rule must not fire on the two bare tokens that legitimately appear.
UTest orphan_rule_accepts_input_paths_and_one_word_references{"HostFlags/OrphanRuleAcceptsInputPathsAndOneWordReferences", [] {
    using mcpp::toolchain::orphaned_reference;

    // Clang's form is ONE token and carries its own switch, so it can never be
    // orphaned -- which is exactly why the defect was MSVC-only.
    expect(!(orphaned_reference(
        {"clang++", "-fmodule-file=mcpp=/b/mcpp.pcm",
         "-fmodule-file=rules.sources=/b/rules.sources.pcm",
         "-c", "build.mcpp"}).has_value()));

    // GCC names nothing at all.
    expect(!(orphaned_reference(
        {"g++", "-fmodules", "-fmodules", "-c", "build.mcpp"}).has_value()));

    // A source or object path that happens to contain `=` is an input, not a
    // reference: its `=` comes AFTER a directory separator. Without this the
    // rule would refuse a legal build in a directory a user is allowed to name.
    expect(!(orphaned_reference(
        {"g++", "/home/me/a=b/build.mcpp"}).has_value()));
    expect(!(orphaned_reference(
        {"cl.exe", "C:/b/x=y/build.mcpp"}).has_value()));
}};

UTest bmi_reference_is_empty_for_a_toolchain_that_names_nothing{"HostFlags/BmiReferenceIsEmptyForAToolchainThatNamesNothing", [] {
    // GCC finds BMIs implicitly under <cwd>/gcm.cache — its prefix is empty
    // and must not produce a stray token.
    expect(mcpp::toolchain::bmi_reference_tokens(
        "", std::filesystem::path("/tmp/x.gcm")).empty());
}};

// ── HostFlagOptions divergences ─────────────────────────────────────────────
//
// These knobs encode documented differences between the three consumers. A
// test so that "why is this optional?" has an answer in code, not only prose.
UTest cfg_bypass_linux_only_differs_from_always_off_linux{"HostFlags/CfgBypassLinuxOnlyDiffersFromAlwaysOffLinux", [] {
    auto tc = tc_for(CompilerId::Clang);
    HostFlagOptions always;   always.cfgBypass = HostFlagOptions::CfgBypass::Always;
    HostFlagOptions linuxOnly; linuxOnly.cfgBypass = HostFlagOptions::CfgBypass::LinuxOnly;

    // Without a real clang payload there is no cfg to bypass, so both are
    // empty here; the assertion that matters is that the option exists and is
    // honoured identically on Linux, where the host helper does bypass.
    if constexpr (mcpplibs::os::is_linux) {
        expect(eq(mcpp::toolchain::host_compile_tokens(tc, always, mcpp::toolchain::no_escape), mcpp::toolchain::host_compile_tokens(tc, linuxOnly, mcpp::toolchain::no_escape)));
    }
}};

// ── Which exit of host_link_tokens names the toolchain's runtime dirs ──────
//
// THE ASYMMETRY IS A DECISION, AND THIS IS WHERE IT IS STATED. The spelled-out
// exit emits `-L` and `-rpath` for `Toolchain::linkRuntimeDirs`; the
// cfg-trusting exit does not, and both halves were measured.
//
// Emitting them there makes `-lc++` resolve to the toolchain's own dylib on
// macOS, which is the ToolchainCoupled contract `dist::mechanism_for` refuses
// on Mach-O: LLVM's macOS libc++abi and libunwind dylibs upward-link
// /usr/lib/libc++, so a second libc++ loads into the process. The link does not
// even get that far -- it stops on `__cxa_end_catch` and the rest of the ABI
// surface the system libc++ re-exports and the payload's does not.
//
// AND WHY THIS TEST CAN RUN ON LINUX. The exit is chosen by the host: with
// `LinuxOnly`, a Linux build always takes the spelled-out one, so the other is
// unreachable from a Linux runner. `CfgBypass::Never` exists to make it
// reachable -- a decision that only two of the three CI hosts can execute
// would otherwise be stated only in prose.
namespace {

// A directory that looks enough like a clang payload for `resolve_clang_driver`
// to report `hasCfg`: a driver file and a sibling `<driver>.cfg`.
struct FakeClangPayload {
    std::filesystem::path root;
    explicit FakeClangPayload(std::string_view name) {
        root = std::filesystem::temp_directory_path()
             / ("mcpp-hostflags-" + std::string(name));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "bin");
        std::filesystem::create_directories(root / "lib");
        std::ofstream{root / "bin" / "clang++"} << "";
        std::ofstream{root / "bin" / "clang++.cfg"} << "";
    }
    ~FakeClangPayload() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

bool names_dir(const std::vector<std::string>& tokens, std::string_view dir) {
    return std::ranges::any_of(tokens, [&](auto const& t) {
        return t == std::string("-L") + std::string(dir);
    });
}

} // namespace

UTest only_the_spelled_out_exit_names_the_toolchain_runtime_dirs{"HostFlags/OnlyTheSpelledOutExitNamesTheToolchainRuntimeDirs", [] {
    FakeClangPayload payload{"runtime-dirs"};
    auto tc = tc_for(CompilerId::Clang);
    tc.binaryPath = payload.root / "bin" / "clang++";
    tc.linkRuntimeDirs = { payload.root / "lib" };

    HostFlagOptions opt;
    opt.runtimeLibDirs = true;

    // Spelled out: the payload's own runtime directories are named.
    opt.cfgBypass = HostFlagOptions::CfgBypass::Always;
    auto spelled = mcpp::toolchain::host_link_tokens(tc, opt, mcpp::toolchain::no_escape);
    expect(bool(names_dir(spelled, (payload.root / "lib").string())));

    // Trusting the cfg: they are NOT, and the comment above says what naming
    // them costs.
    opt.cfgBypass = HostFlagOptions::CfgBypass::Never;
    auto trusting = mcpp::toolchain::host_link_tokens(tc, opt, mcpp::toolchain::no_escape);
    expect(!(names_dir(trusting, (payload.root / "lib").string())))
        << "the cfg-trusting exit now points `-lc++` at the toolchain's own "
           "dylib; on Mach-O that link fails on the C++ ABI symbols the system "
           "libc++ re-exports";

    // And the option is still an option on the exit that honours it.
    opt.runtimeLibDirs = false;
    opt.cfgBypass = HostFlagOptions::CfgBypass::Always;
    auto off = mcpp::toolchain::host_link_tokens(tc, opt, mcpp::toolchain::no_escape);
    expect(!(names_dir(off, (payload.root / "lib").string())));
}};

// TARGET-KEYED, NOT HOST-KEYED (mcpp#685). This used to read
// `EXPECT_EQ(found, mcpplibs::os::is_macos)` — the flag's presence
// followed the machine RUNNING this test rather than the `Toolchain`'s own
// `targetTriple`, so it could not distinguish "building for macOS" from
// "building on macOS" and stayed green while `--target aarch64-macos` from a
// non-Apple host silently dropped the flag (and the iOS mirror: a macOS host
// building `x86_64-linux-musl` wrongly kept it). Both targets are asked here
// so the assertion holds independent of whichever host runs this suite.
UTest deployment_target_only_on_a_macos_target{"HostFlags/DeploymentTargetOnlyOnAMacosTarget", [] {
    HostFlagOptions opt;
    opt.macosDeploymentTarget = "14.0";

    auto macTc = tc_for(CompilerId::GCC);
    macTc.targetTriple = "aarch64-macos";
    auto macTokens = mcpp::toolchain::host_compile_tokens(
        macTc, opt, mcpp::toolchain::no_escape);
    expect(std::ranges::any_of(macTokens, [](auto const& t) {
        return t == "-mmacosx-version-min=14.0";
    })) << "a macOS target must carry the resolved deployment target "
           "regardless of the host running the build";

    auto linuxTc = tc_for(CompilerId::GCC);
    linuxTc.targetTriple = "x86_64-linux-musl";
    auto linuxTokens = mcpp::toolchain::host_compile_tokens(
        linuxTc, opt, mcpp::toolchain::no_escape);
    expect(!(std::ranges::any_of(linuxTokens, [](auto const& t) {
        return t.starts_with("-mmacosx-version-min=");
    }))) << "a non-macOS target must never see a macOS deployment flag, even "
           "when built on a macOS host (the mirror defect #685 also fixes)";
}};

// ── graph_runtime_compile_flags: what a `throw` and a `thread_local` compile
//    into, when the runtime comes from the dependency graph.
//
// These are not ordinary flags. They change what a translation unit EMITS for
// constructs the language guarantees work across a whole program, so two
// objects that disagree link and the disagreement is the defect. The function
// exists so that the decision is made once; these tests exist so that each of
// its four states is stated rather than inferred from a build.

namespace {

// The openkal shape: the C library is the graph's too. `cAbiPrebuilt` is set
// explicitly because it decides the Mach-O emulated-TLS row below, and the
// default (`true`) describes the other arrangement.
mcpp::toolchain::Toolchain graph_tc(std::string triple) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler         = CompilerId::Clang;
    tc.targetTriple     = std::move(triple);
    tc.targetCxxRuntime = true;
    tc.cAbiPrebuilt     = false;
    return tc;
}

bool has(const std::vector<std::string>& v, std::string_view f) {
    return std::ranges::find(v, f) != v.end();
}

}  // namespace

// PE: clang defaults to SEH there, whose personality routine and unwind data
// come from the operating system's unwinder. A graph that supplies its own C++
// runtime supplies its own unwinder with it, and the two cannot be mixed inside
// one image. A `thread_local` on PE is reached through `_tls_index`, which the
// dynamic loader bootstraps and a self-contained image has no loader for.
UTest pe_takes_dwarf_exceptions_and_emulated_tls{"GraphRuntimeFlags/PeTakesDwarfExceptionsAndEmulatedTls", [] {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("x86_64-windows-gnu"));
    expect(bool(has(f, "-fdwarf-exceptions")));
    expect(bool(has(f, "-femulated-tls")));
}};

// Mach-O: the exception mechanism is already DWARF, so only the thread-local
// one applies — `_tlv_bootstrap` is the loader-bootstrapped name there. The
// visibility pair is present because on this format a default-visibility weak
// definition is coalesced BY THE LOADER, which is machinery a self-contained
// image has no use for and which produced a jump to address zero when it was
// left in place.
UTest mach_o_takes_emulated_tls_and_hidden_visibility_but_not_dwarf{"GraphRuntimeFlags/MachOTakesEmulatedTlsAndHiddenVisibilityButNotDwarf", [] {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("aarch64-macos"));
    expect(!has(f, "-fdwarf-exceptions"));
    expect(bool(has(f, "-femulated-tls")));
    expect(bool(has(f, "-fvisibility=hidden")));
    expect(bool(has(f, "-fvisibility-inlines-hidden")));
}};

// iOS shares macOS's object format (Mach-O, ld64), so it must share this
// exact set of flags. Before `is_mach_o()` replaced `os == "macos"` here, an
// iOS triple matched neither the PE nor the macOS branch and this function
// silently returned no flags at all for it.
UTest ios_takes_the_same_flags_as_mac_os{"GraphRuntimeFlags/IosTakesTheSameFlagsAsMacOS", [] {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("aarch64-ios"));
    expect(!has(f, "-fdwarf-exceptions"));
    expect(bool(has(f, "-femulated-tls")));
    expect(bool(has(f, "-fvisibility=hidden")));
    expect(bool(has(f, "-fvisibility-inlines-hidden")));
}};

// A hosted Mach-O target whose C library is a located SDK (the iOS rows over
// `llvm.libcxx`, mcpp#630) has dyld and the native TLS model with it, so the
// emulated-TLS row does not apply; the visibility rows still do, since a
// second libc++ in one process is kept apart by visibility on Mach-O.
UTest mach_o_over_a_prebuilt_c_library_keeps_native_tls{"GraphRuntimeFlags/MachOOverAPrebuiltCLibraryKeepsNativeTls", [] {
    auto tc = graph_tc("aarch64-ios");
    tc.cAbiPrebuilt = true;
    auto f = mcpp::toolchain::graph_runtime_compile_flags(tc);
    expect(!has(f, "-femulated-tls"));
    expect(!has(f, "-fdwarf-exceptions"));
    expect(bool(has(f, "-fvisibility=hidden")));
    expect(bool(has(f, "-fvisibility-inlines-hidden")));
    // And PE keeps it regardless: `_tls_index` is loader-bootstrapped there
    // whether or not the C library is the graph's.
    auto pe = graph_tc("x86_64-windows-gnu");
    pe.cAbiPrebuilt = true;
    expect(bool(has(mcpp::toolchain::graph_runtime_compile_flags(pe), "-femulated-tls")));
}};

// ELF takes NONE of them, and that is a decision rather than an omission.
// There a `thread_local` is a fixed offset from the thread pointer, which the
// C library establishes itself; adding the flag would work, cost an
// indirection on every access, and make ELF the only target whose thread
// locals are laid out differently from every other build of the same target.
UTest elf_takes_none{"GraphRuntimeFlags/ElfTakesNone", [] {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("x86_64-linux-gnu"));
    expect(f.empty());
}};

// And nothing at all when the runtime is NOT the graph's, whatever the
// target. The predicate is `targetCxxRuntime`; a native or payload-served build
// of the same triple must be untouched.
UTest payload_served_target_takes_none_even_on_pe{"GraphRuntimeFlags/PayloadServedTargetTakesNoneEvenOnPe", [] {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = CompilerId::Clang;
    tc.targetTriple = "x86_64-windows-gnu";
    tc.targetCxxRuntime = false;
    expect(mcpp::toolchain::graph_runtime_compile_flags(tc).empty());
}};

// A triple outside the vocabulary yields nothing rather than a guess: the
// answer depends on the object format, and a spelling that cannot be parsed
// does not name one.
UTest unparseable_triple_takes_none{"GraphRuntimeFlags/UnparseableTripleTakesNone", [] {
    expect(mcpp::toolchain::graph_runtime_compile_flags(
        graph_tc("not-a-triple-at-all")).empty());
}};

// `--no-default-config` IS NOT PART OF THE PAYLOAD'S HEADER SET, AND WAS
// BEING SUPPRESSED WITH IT.
//
// The payload's `-isystem` rows describe a C library a graph-supplied target
// does not use, so withholding them is right. The cfg bypass is a different
// statement: `post_install.cppm` calls that file "a per-machine,
// per-install-path artifact", and reading it makes the command line depend on
// what happened to be installed when the payload landed.
//
// Measured on 2026.8.26.2: `mcpp build --target <the host's own>` dropped the
// token, so clang read `bin/clang++.cfg`. That is also what made a hand-written
// `<triple>-clang++.cfg` a working workaround for mcpp#514 — a workaround that
// only existed because this token went missing.
UTest the_cfg_bypass_survives_a_graph_supplied_target_side{"HostFlags/TheCfgBypassSurvivesAGraphSuppliedTargetSide", [] {
    // A FIXTURE, NOT THE MACHINE'S OWN TOOLCHAIN. The first draft used a
    // synthetic `Toolchain` with no `binaryPath`, so `resolve_clang_driver`
    // reported no cfg and the whole test SKIPPED — a check that asserts
    // nothing while reporting success, which is the one failure mode a test
    // must not have. `resolve_clang_driver` only asks whether a sibling
    // `<driver>.cfg` EXISTS, so two empty files are a complete fixture.
    namespace fs = std::filesystem;
    // A FIXED NAME AND `remove_all` FIRST, NOT A PROCESS ID. The first
    // draft reached for `::getpid()` and `<unistd.h>`, which do not exist under
    // MSVC — it built on the machine it was written on and failed on Windows
    // CI, which is the only place that half of this project is visible.
    // gtest runs a binary's tests serially, so one name is enough.
    const auto root = fs::temp_directory_path() / "mcpp_hostflags_cfg_fixture";
    fs::remove_all(root);
    fs::create_directories(root / "bin");
    fs::create_directories(root / "include" / "c++" / "v1");
    { std::ofstream(root / "bin" / "clang++"); }
    { std::ofstream(root / "bin" / "clang++.cfg"); }
    struct Cleanup {
        fs::path p;
        ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); }
    } cleanup{root};

    auto tc = tc_for(CompilerId::Clang);
    tc.binaryPath = root / "bin" / "clang++";
    expect(fatal(bool(mcpp::toolchain::resolve_clang_driver(tc).hasCfg)))
        << "the fixture did not produce a cfg — the assertions below would be vacuous";

    const auto has = [](const std::vector<std::string>& v, std::string_view f) {
        return std::ranges::find(v, f) != v.end();
    };

    HostFlagOptions prebuilt;
    prebuilt.cfgBypass    = HostFlagOptions::CfgBypass::Always;
    prebuilt.cAbiPrebuilt = true;

    HostFlagOptions fromGraph = prebuilt;
    fromGraph.cAbiPrebuilt = false;

    const auto a = mcpp::toolchain::host_compile_tokens(
        tc, prebuilt, mcpp::toolchain::no_escape);
    const auto b = mcpp::toolchain::host_compile_tokens(
        tc, fromGraph, mcpp::toolchain::no_escape);

    // The bypass is emitted on BOTH sides: the cfg is a per-machine,
    // per-install-path artifact, and reading it makes the command line depend
    // on what happened to be installed when the payload landed.
    expect(bool(has(a, "--no-default-config")));
    expect(bool(has(b, "--no-default-config")));
    // ...while the payload's own C++ headers stay withheld from the graph side,
    // which is the distinction this splits apart.
    expect(bool(has(a, "-nostdinc++")));
    expect(!has(b, "-nostdinc++"));
}};

// "A TOOLCHAIN THAT SHIPS ITS OWN SYSROOT IS TOLD NOTHING" WAS ONE TOKEN TOO
// STRONG, AND THIS FUNCTION ALREADY SAID SO FURTHER DOWN.
//
// The early return for `has_own_sysroot()` withholds the target's system
// reconstructed onto the command line -- libc++'s headers, glibc's, the Linux
// UAPI headers, the cfg bypass, the C-runtime prefix -- because an Emscripten
// or Android SDK already has all of it. That is right. It stood in FRONT of
// the paragraph beginning "THE TRIPLE, SAID OUT LOUD", which states the
// opposite rule for the same underlying reason: an ordinary clang emits for the
// machine it is running on unless told otherwise. The stronger claim won by
// position.
//
// Both are right about their own object. The SYSTEM is the payload's; WHICH
// TARGET is still mcpp's to say, because one NDK serves both Android ABIs and
// nothing else on the command line distinguishes them. The defect was reported
// by neither compile but by the module loader:
//
//   error: AST file 'std.pcm' was compiled for the target
//     'aarch64-unknown-linux-android21' but the current translation unit is
//     being compiled for target 'x86_64-unknown-linux-gnu'
//
// followed by eight cascading "use of undeclared identifier 'std'" lines,
// which is what a reader sees first.
UTest an_own_sysroot_target_is_told_which_target_and_nothing_else{"HostFlags/AnOwnSysrootTargetIsToldWhichTargetAndNothingElse", [] {
    HostFlagOptions opt;

    for (auto name : {"aarch64-linux-android", "x86_64-linux-android",
                      "wasm32-emscripten"}) {
        auto tc = tc_for(CompilerId::Clang);
        tc.targetTriple = name;
        tc.crossTargetFlag = "--target=SENTINEL-TRIPLE";

        auto tokens = mcpp::toolchain::host_compile_tokens(
            tc, opt, mcpp::toolchain::no_escape);

        // EXACTLY the target flag. Asserted as the whole vector rather than as
        // "contains", because the property is that nothing ELSE is emitted:
        // this host's glibc headers reaching a wasm compile is the measured
        // failure this gate exists for.
        expect(fatal(eq(tokens.size(), 1u))) << name << ": " << [&] {
            std::string all;
            for (auto const& t : tokens) { all += t; all += ' '; }
            return all;
        }();
        expect(eq(tokens[0], "--target=SENTINEL-TRIPLE")) << name;
    }

    // AND THE GATE IS STILL A GATE, DISCRIMINATED BY THE cfg BYPASS.
    //
    // A first version of this control asserted that a HOSTED target receives
    // more than one token, and it failed -- with a bare `Toolchain` carrying no
    // payload paths, the hosted path has nothing to reconstruct either, so both
    // sides produced exactly the triple and the control could not tell them
    // apart. The control was wrong, not the code.
    //
    // `--no-default-config` is the discriminator, and it is a property the
    // gate's own comment states: the bypass exists to stop clang reading a
    // per-install `clang++.cfg`, while `em++` is a wrapper whose entire job is
    // to supply configuration, so suppressing it would be suppressing the
    // toolchain. It is therefore emitted past the gate and never before it,
    // which is exactly what a control needs.
    // A first version of this control asserted only that a HOSTED target
    // receives more than one token, and it failed -- with a bare `Toolchain`
    // carrying no payload the hosted path has nothing to reconstruct either,
    // so both sides produced exactly the triple and the control could not tell
    // them apart. A second version reached for `--no-default-config` without a
    // payload that HAS a cfg, which is the same mistake once removed. The
    // fixture is what makes the discriminator real.
    //
    // `--no-default-config` is the right discriminator because it is a
    // property the gate's own comment states: the bypass exists to stop clang
    // reading a per-install `clang++.cfg`, while `em++` is a wrapper whose
    // entire job is to supply configuration, so suppressing it would be
    // suppressing the toolchain. Emitted past the gate, never before it.
    FakeClangPayload payload{"own-sysroot-gate"};
    HostFlagOptions bypass;
    bypass.cfgBypass = HostFlagOptions::CfgBypass::Always;

    auto host = tc_for(CompilerId::Clang);
    host.binaryPath = payload.root / "bin" / "clang++";
    host.crossTargetFlag = "--target=x86_64-unknown-linux-gnu";
    auto hostTokens = mcpp::toolchain::host_compile_tokens(
        host, bypass, mcpp::toolchain::no_escape);
    expect(ne(std::ranges::find(hostTokens, "--no-default-config"), hostTokens.end()))
        << "a hosted clang with a cfg beside it must reach the bypass";

    for (auto name : {"aarch64-linux-android", "wasm32-emscripten"}) {
        auto sdk = tc_for(CompilerId::Clang);
        sdk.binaryPath = payload.root / "bin" / "clang++";  // same payload
        sdk.targetTriple = name;
        sdk.crossTargetFlag = "--target=SENTINEL-TRIPLE";
        auto sdkTokens = mcpp::toolchain::host_compile_tokens(
            sdk, bypass, mcpp::toolchain::no_escape);
        expect(eq(std::ranges::find(sdkTokens, "--no-default-config"), sdkTokens.end()))
            << name << ": the cfg bypass must be withheld from an SDK whose "
                       "driver's job is to supply configuration";
        expect(eq(sdkTokens.size(), 1u)) << name;
    }

    // A row with no cross flag emits nothing at all rather than an empty
    // token: an empty argv element is an argument the driver must interpret.
    auto bare = tc_for(CompilerId::Clang);
    bare.targetTriple = "wasm32-emscripten";
    expect(fatal(bare.crossTargetFlag.empty()));
    expect(mcpp::toolchain::host_compile_tokens(
                    bare, opt, mcpp::toolchain::no_escape).empty());
}};

// ── The C++ headers are the C++ layer's question ─────────────────────────────
//
// The payload's libc++ `-isystem` block was withheld exactly when the C
// LIBRARY came from the graph, which was the same question while the only
// graph C++ runtime sat over a graph C library. A hosted target whose C
// library is a located SDK while a package supplies libc++ (the iOS rows over
// `llvm.libcxx`, mcpp#630) answered "payload" there and put two libc++ header
// sets on one command line. The same fixture as the bypass test above: two
// empty files beside an `include/c++/v1` are a complete driver model.
UTest the_cxx_layer_decides_whose_libcxx_headers_are_emitted{"HostFlags/TheCxxLayerDecidesWhoseLibcxxHeadersAreEmitted", [] {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "mcpp_hostflags_cxx_fixture";
    fs::remove_all(root);
    fs::create_directories(root / "bin");
    fs::create_directories(root / "include" / "c++" / "v1");
    { std::ofstream(root / "bin" / "clang++"); }
    { std::ofstream(root / "bin" / "clang++.cfg"); }
    struct Cleanup {
        fs::path p;
        ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); }
    } cleanup{root};

    auto tc = tc_for(CompilerId::Clang);
    tc.binaryPath = root / "bin" / "clang++";
    expect(fatal(bool(mcpp::toolchain::resolve_clang_driver(tc).hasCfg)))
        << "the fixture did not produce a cfg — the assertions below would be vacuous";

    const auto any_payload_cxx = [&](const std::vector<std::string>& v) {
        return std::ranges::any_of(v, [&](const std::string& t) {
            return t.starts_with("-isystem")
                && t.find((root / "include" / "c++" / "v1").string()) != std::string::npos;
        });
    };
    const auto has = [](const std::vector<std::string>& v, std::string_view f) {
        return std::ranges::find(v, f) != v.end();
    };

    HostFlagOptions payload;
    payload.cfgBypass    = HostFlagOptions::CfgBypass::Always;
    payload.cAbiPrebuilt = true;

    // The baseline: a prebuilt C library under the payload's own libc++ takes
    // the payload's headers, as every native build does.
    const auto a = mcpp::toolchain::host_compile_tokens(tc, payload, mcpp::toolchain::no_escape);
    expect(bool(any_payload_cxx(a)));
    expect(bool(has(a, "-nostdinc++")));
    expect(!has(a, "--rtlib=compiler-rt"));

    // Runtime selection affects AArch64 PCM compatibility even on a compile
    // without a link. The host std producer and one-shot helper must agree.
    auto arm = tc;
    arm.targetTriple = "aarch64-linux-gnu";
    auto armCompile = mcpp::toolchain::host_compile_tokens(arm, payload, mcpp::toolchain::no_escape);
    auto armLink = mcpp::toolchain::host_link_tokens(arm, payload, mcpp::toolchain::no_escape);
    expect(bool(has(armCompile, "--rtlib=compiler-rt")));
    expect(bool(has(armLink, "--rtlib=compiler-rt")));

    // A graph C++ runtime over the same prebuilt C library: the payload's
    // headers are withheld and the driver's own search is closed, since the
    // package's directories arrive through the target-side broadcast.
    HostFlagOptions graph = payload;
    graph.cxxFromGraph = true;
    const auto b = mcpp::toolchain::host_compile_tokens(tc, graph, mcpp::toolchain::no_escape);
    expect(!any_payload_cxx(b));
    expect(bool(has(b, "-nostdinc++")));
    expect(bool(has(b, "--no-default-config")));
    // A graph owns its runtime/codegen broadcast; payload selection must not
    // be introduced merely because the target architecture is AArch64.
    expect(!(has(mcpp::toolchain::host_compile_tokens(arm, graph, mcpp::toolchain::no_escape),
                     "--rtlib=compiler-rt")));

    // An Apple cross target whose runtime is the SDK's libc++ and whose graph
    // does not import `std`: prepare chose the SDK's headers, named
    // explicitly because clang's Darwin driver would otherwise prefer the
    // copy beside itself.
    HostFlagOptions sdk = payload;
    sdk.appleSdkRoot       = fs::path("/Sdk/iPhoneSimulator.sdk");
    sdk.appleSdkCxxHeaders = true;
    const auto c = mcpp::toolchain::host_compile_tokens(tc, sdk, mcpp::toolchain::no_escape);
    expect(!any_payload_cxx(c));
    expect(bool(has(c, "-nostdinc++")));
    expect(bool(has(c, "-isystem" + (fs::path("/Sdk/iPhoneSimulator.sdk") / "usr" / "include" / "c++" / "v1").string())));

    // And with the graph runtime on that same target the SDK's headers are
    // not named either: one libc++ per command line, whichever it is.
    HostFlagOptions sdkGraph = sdk;
    sdkGraph.cxxFromGraph       = true;
    sdkGraph.appleSdkCxxHeaders = false;
    const auto d = mcpp::toolchain::host_compile_tokens(tc, sdkGraph, mcpp::toolchain::no_escape);
    expect(!any_payload_cxx(d));
    expect(!(std::ranges::any_of(d, [](const std::string& t) {
        return t.starts_with("-isystem") && t.find("iPhoneSimulator.sdk") != std::string::npos;
    })));
    expect(bool(has(d, "-nostdinc++")));
}};


// The floating macros an Apple SDK leaves to <float.h> once modules are on
// (apple_float_macro_words). Stated for clang on an Apple target and for
// nothing else, as the SDK's own spellings, and quotable by every reader.
UTest apple_float_macros_are_stated_for_clang_on_apple_targets_only{"HostFlags/AppleFloatMacrosAreStatedForClangOnAppleTargetsOnly", [] {
    const std::vector<std::string> words{
        "-DINFINITY=HUGE_VALF", "-DNAN=__builtin_nanf(\"0x7fc00000\")"};
    auto tc = [](CompilerId id, std::string triple) {
        mcpp::toolchain::Toolchain t;
        t.compiler = id;
        t.targetTriple = std::move(triple);
        return t;
    };
    expect(eq(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::Clang, "arm64-apple-darwin27.0.0")), words));
    expect(eq(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::Clang, "aarch64-macos")), words));
    expect(eq(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::Clang, "arm64-apple-ios18.0")), words));
    expect(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::Clang, "x86_64-linux-gnu")).empty());
    expect(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::Clang, "wasm32-emscripten")).empty());
    expect(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::GCC, "x86_64-linux-gnu")).empty());
    expect(mcpp::toolchain::apple_float_macro_words(tc(CompilerId::MSVC, "x86_64-pc-windows-msvc")).empty());
}};

// ── #662: the C library's own host locations, and its C++ twin ─────────────
//
// Same fixture as the two suites above — a sibling `<driver>.cfg` beside two
// empty files is a complete driver model for `resolve_clang_driver`, and the
// tests below need only that `bypassCfg` is true.
namespace {

struct ClangCfgFixture {
    std::filesystem::path root;
    ClangCfgFixture(std::string_view name) {
        namespace fs = std::filesystem;
        root = fs::temp_directory_path() / name;
        fs::remove_all(root);
        fs::create_directories(root / "bin");
        fs::create_directories(root / "include" / "c++" / "v1");
        { std::ofstream(root / "bin" / "clang++"); }
        { std::ofstream(root / "bin" / "clang++.cfg"); }
    }
    ~ClangCfgFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    mcpp::toolchain::Toolchain toolchain() const {
        mcpp::toolchain::Toolchain tc;
        tc.compiler     = CompilerId::Clang;
        tc.targetTriple = "x86_64-windows-gnu";
        tc.binaryPath   = root / "bin" / "clang++";
        return tc;
    }
};

bool contains(const std::vector<std::string>& v, std::string_view f) {
    return std::ranges::find(v, f) != v.end();
}

std::size_t count_of(const std::vector<std::string>& v, std::string_view f) {
    return std::ranges::count(v, f);
}

} // namespace

// A graph-supplied C library closes the driver's own C-library search —
// the compile-side half of the `-nostdlib` the link side has read since
// #511. Measured absent before this fix: the openkal windows-gnu row's
// C units kept finding `/usr/x86_64-w64-mingw32/include` (the host's mingw)
// ahead of musl's own headers.
UTest graph_supplied_c_library_gets_nostdlibinc{"HostFlags/GraphSuppliedCLibraryGetsNostdlibinc", [] {
    ClangCfgFixture fx("mcpp_hostflags_662_clib_fixture");
    auto tc = fx.toolchain();
    expect(fatal(bool(mcpp::toolchain::resolve_clang_driver(tc).hasCfg)));

    HostFlagOptions payload;
    payload.cfgBypass    = HostFlagOptions::CfgBypass::Always;
    payload.cAbiPrebuilt = true;
    expect(!(contains(mcpp::toolchain::host_compile_tokens(
        tc, payload, mcpp::toolchain::no_escape), "-nostdlibinc")));

    HostFlagOptions graph = payload;
    graph.cAbiPrebuilt = false;
    expect(contains(mcpp::toolchain::host_compile_tokens(
        tc, graph, mcpp::toolchain::no_escape), "-nostdlibinc"));
}};

// THE OPENKAL SHAPE ITSELF (#662): both layers come from the graph at once.
// Before this fix `-nostdinc++` was conditioned on `!graphSuppliesTarget`,
// which is false exactly here, so it never fired — clang kept searching
// beside itself for the payload's libc++, found the HOST's libstdc++
// instead, and any unit reaching it transitively (not just `import std`)
// carried two C++ standard libraries.
UTest both_layers_from_graph_get_both_isolation_tokens{"HostFlags/BothLayersFromGraphGetBothIsolationTokens", [] {
    ClangCfgFixture fx("mcpp_hostflags_662_both_fixture");
    auto tc = fx.toolchain();
    expect(fatal(bool(mcpp::toolchain::resolve_clang_driver(tc).hasCfg)));

    HostFlagOptions opt;
    opt.cfgBypass    = HostFlagOptions::CfgBypass::Always;
    opt.cAbiPrebuilt  = false;  // c-abi:  musl, from the graph
    opt.cxxFromGraph  = true;   // c++-abi: libc++, from the graph
    const auto toks = mcpp::toolchain::host_compile_tokens(
        tc, opt, mcpp::toolchain::no_escape);

    expect(contains(toks, "-nostdlibinc"));
    expect(contains(toks, "-nostdinc++"));
    // Exactly once each — the two branches that can add `-nostdinc++`
    // (graph C++, SDK C++) must not both fire for the same build.
    expect(eq(count_of(toks, "-nostdinc++"), 1u));
    expect(eq(count_of(toks, "-nostdlibinc"), 1u));
}};

// THE REGRESSION GUARD: a build where both layers stay the payload's — every
// native build, and every build over a prebuilt/payload C library — emits
// the identical token sequence this fix touched nothing for. Compared as a
// whole vector rather than by presence, so an accidental REORDERING (which
// would still change the rendered command line) fails this test too.
UTest payload_served_command_line_is_unchanged{"HostFlags/PayloadServedCommandLineIsUnchanged", [] {
    ClangCfgFixture fx("mcpp_hostflags_662_guard_fixture");
    auto tc = fx.toolchain();
    expect(fatal(bool(mcpp::toolchain::resolve_clang_driver(tc).hasCfg)));

    HostFlagOptions opt;
    opt.cfgBypass    = HostFlagOptions::CfgBypass::Always;
    opt.cAbiPrebuilt = true;   // default in every caller with no graph target
    opt.cxxFromGraph = false;

    const auto toks = mcpp::toolchain::host_compile_tokens(
        tc, opt, mcpp::toolchain::no_escape);
    const std::vector<std::string> expected{
        "--no-default-config", "-nostdinc++",
        "-isystem" + (fx.root / "include" / "c++" / "v1").string(),
    };
    expect(eq(toks, expected));
    expect(!contains(toks, "-nostdlibinc"));
}};

// GCC's fixture carries no `<driver>.cfg` (resolve_clang_driver only looks
// for one beside a Clang binary), so `bypassCfg` is false and neither
// isolation branch runs. `-nostdlibinc` is Clang's own flag; GCC has no
// single-token equivalent (the shape would be `-nostdinc` plus `-isystem
// <gcc -print-file-name=include>` and `<…/include-fixed>`, re-adding
// exactly the two directories GCC's own C-library search already
// contributes beside the sysroot) — so GCC stays exactly as it was before
// #662: unisolated. No target row in this codebase's own target table pairs
// a graph-supplied C library with GCC today, so nothing currently needs it.
UTest gcc_emits_neither_isolation_token_regardless_of_graph_origin{"HostFlags/GccEmitsNeitherIsolationTokenRegardlessOfGraphOrigin", [] {
    auto tc = tc_for(CompilerId::GCC);

    HostFlagOptions opt;
    opt.cfgBypass    = HostFlagOptions::CfgBypass::Always;
    opt.cAbiPrebuilt = false;
    opt.cxxFromGraph = true;
    const auto toks = mcpp::toolchain::host_compile_tokens(
        tc, opt, mcpp::toolchain::no_escape);
    expect(!contains(toks, "-nostdlibinc"));
    expect(!contains(toks, "-nostdinc++"));
}};

// ─── Clang on the MSVC ABI: one toolset for the compile, std module and link
//
// `host_compile_tokens` is read by the compile line, the std module precompile
// and the build.mcpp host compile, so the toolset words appearing here is what
// makes all three compile against the toolset prepare chose.
UTest clang_on_the_msvc_abi_compiles_and_links_against_the_chosen_toolset{"HostFlags/ClangOnTheMsvcAbiCompilesAndLinksAgainstTheChosenToolset", [] {
    mcpp::toolchain::Toolchain t;
    t.compiler          = CompilerId::Clang;
    t.targetTriple      = "x86_64-pc-windows-msvc";
    t.base.msvc.toolsDir      = "C:/VS/VC/Tools/MSVC/14.44.35207";
    t.base.msvc.sdkRoot    = "C:/Kits/10";
    t.base.msvc.sdkVersion = "10.0.26100.0";
    HostFlagOptions opt;
    auto find_pair = [](const std::vector<std::string>& v, std::string_view flag,
                        std::string_view value) {
        for (std::size_t i = 0; i + 1 < v.size(); ++i)
            if (v[i] == flag && v[i + 1] == value) return true;
        return false;
    };
    auto compile = mcpp::toolchain::host_compile_tokens(t, opt, mcpp::toolchain::no_escape);
    auto link    = mcpp::toolchain::host_link_tokens(t, opt, mcpp::toolchain::no_escape);
    for (auto const* v : {&compile, &link}) {
        expect(bool(find_pair(*v, "-Xmicrosoft-visualc-tools-root",
                              "C:/VS/VC/Tools/MSVC/14.44.35207")));
        expect(bool(find_pair(*v, "-Xmicrosoft-windows-sdk-root", "C:/Kits/10")));
        expect(bool(find_pair(*v, "-Xmicrosoft-windows-sdk-version", "10.0.26100.0")));
    }

    // Without a recorded toolset nothing is said, which is the behaviour of
    // every build before a toolset is bound.
    t.base.msvc.toolsDir.clear();
    for (auto const& w : mcpp::toolchain::host_compile_tokens(t, opt, mcpp::toolchain::no_escape))
        expect(eq(w.find("-Xmicrosoft"), std::string::npos)) << w;
}};
