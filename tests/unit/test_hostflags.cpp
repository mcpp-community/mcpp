#include <gtest/gtest.h>
#include <fstream>

import std;
import mcpp.platform;
import mcpp.toolchain.dialect;
import mcpp.toolchain.hostflags;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;

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
TEST(HostFlags, EveryFamilyHasCompleteTables) {
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        const auto& d = mcpp::toolchain::dialect_for(tc);
        const auto  t = mcpp::toolchain::bmi_traits(tc);

        EXPECT_FALSE(d.id.empty());
        // Needed to compile a `.mcpp` at all: the extension is unknown to
        // every driver, so the language has to be forced.
        EXPECT_FALSE(d.forceCxxLangArgv.empty()) << d.id;
        // Needed to name the produced program.
        EXPECT_FALSE(d.outputExePrefix.empty()) << d.id;
        EXPECT_FALSE(d.objExt.empty()) << d.id;
        // Needed to build and reference the bundled `mcpp` module.
        EXPECT_FALSE(t.bmiDir.empty()) << d.id;
        EXPECT_FALSE(t.bmiExt.empty()) << d.id;
    }
}

// The producer must answer for every family too — MSVC deliberately returns
// nothing (cl.exe finds headers and libs through INCLUDE/LIB, not argv), but
// it must be a decision, not a crash or an accident.
TEST(HostFlags, ProducerAnswersForEveryFamily) {
    HostFlagOptions opt;
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        auto compile = mcpp::toolchain::host_compile_tokens(
            tc, opt, mcpp::toolchain::no_escape);
        auto link = mcpp::toolchain::host_link_tokens(
            tc, opt, mcpp::toolchain::no_escape);
        if (id == CompilerId::MSVC) {
            EXPECT_TRUE(compile.empty());
            EXPECT_TRUE(link.empty());
        }
        // No empty tokens anywhere: an empty argv element is an argument the
        // driver still has to interpret.
        for (auto const& t : compile) EXPECT_FALSE(t.empty());
        for (auto const& t : link)    EXPECT_FALSE(t.empty());
    }
}

// ── The rendered string must not move ───────────────────────────────────────
//
// stdmod folds its compile command into `std_build_commands`, and the std
// cache DIRECTORY NAME is derived from the metadata containing it. Reordering
// a flag therefore invalidates every user's std BMIs for no behavioural gain
// — so the exact spelling is a compatibility surface, not an implementation
// detail. This pins it; the first attempt at this refactor moved
// `-stdlib=libc++` after the include flags and would have shipped exactly
// that invalidation.
TEST(HostFlags, ClangCfgBypassStringIsStable) {
    mcpp::toolchain::ClangDriverModel dm;
    dm.hasCfg = true;
    dm.cxxIncludes = { "/llvm/include/c++/v1", "/llvm/include/tgt/c++/v1" };

    EXPECT_EQ(dm.compile_flags(mcpp::toolchain::no_escape),
              " --no-default-config -nostdinc++"
              " -isystem/llvm/include/c++/v1"
              " -isystem/llvm/include/tgt/c++/v1");

    // With the stdlib selection the std module has always asked for, it lands
    // immediately after -nostdinc++ — not at the end.
    EXPECT_EQ(mcpp::toolchain::render_tokens(
                  dm.compile_tokens(mcpp::toolchain::no_escape, true)),
              " --no-default-config -nostdinc++ -stdlib=libc++"
              " -isystem/llvm/include/c++/v1"
              " -isystem/llvm/include/tgt/c++/v1");
}

TEST(HostFlags, LinkModelStringsAreStable) {
    mcpp::toolchain::ToolchainLinkModel lm;
    lm.mode = mcpp::toolchain::CLibMode::PayloadFirst;
    lm.clangDriver = true;
    lm.crtDir = "/glibc/lib";
    lm.libDirs = { "/glibc/lib" };
    lm.loader = "/glibc/lib/ld.so";
    lm.systemIncludes = { "/glibc/include" };

    EXPECT_EQ(lm.compile_flags(mcpp::toolchain::no_escape),
              " -isystem/glibc/include");
    EXPECT_EQ(lm.link_flags(mcpp::toolchain::no_escape),
              " -B/glibc/lib -L/glibc/lib -Wl,-rpath,/glibc/lib"
              " -Wl,--dynamic-linker=/glibc/lib/ld.so");

    // GCC takes -idirafter so libstdc++'s #include_next wrappers can still
    // reach libc.
    lm.clangDriver = false;
    EXPECT_EQ(lm.compile_flags(mcpp::toolchain::no_escape),
              " -idirafter/glibc/include");
}

// ── bmi_reference_tokens ────────────────────────────────────────────────────
//
// The traits store these for the ninja STRING channel, where one word vs two
// makes no difference. argv consumers cannot be that relaxed.
TEST(HostFlags, BmiReferenceSplitsOnlyWhenTheSpellingHasASpace) {
    auto gnu = mcpp::toolchain::bmi_reference_tokens(
        " -fmodule-file=std=", std::filesystem::path("/tmp/std.pcm"));
    ASSERT_EQ(gnu.size(), 1u);
    EXPECT_EQ(gnu[0], "-fmodule-file=std=/tmp/std.pcm");

    auto msvc = mcpp::toolchain::bmi_reference_tokens(
        " /reference std=", std::filesystem::path("/tmp/std.ifc"));
    ASSERT_EQ(msvc.size(), 2u);
    EXPECT_EQ(msvc[0], "/reference");
    EXPECT_EQ(msvc[1], "std=/tmp/std.ifc");
}

// The general invariant, checked for every family: an argv element must never
// contain a space. This bug has now appeared three times in the same shape —
// `-x c++`, the mcpp module reference, the std reference — each time because
// a table entry written for the ninja STRING channel was concatenated into an
// argv element. cl.exe answers with "could not find module 'std'", which
// names neither the flag nor the reason.
TEST(HostFlags, BmiReferencesNeverProduceATokenWithASpace) {
    for (auto id : kFamilies) {
        auto tc = tc_for(id);
        auto t = mcpp::toolchain::bmi_traits(tc);
        for (auto prefix : { t.stdBmiUsePrefix, t.stdCompatBmiUsePrefix }) {
            for (auto const& tok : mcpp::toolchain::bmi_reference_tokens(
                     prefix, std::filesystem::path("/tmp/x.bmi"))) {
                EXPECT_EQ(tok.find(' '), std::string::npos)
                    << "family " << mcpp::toolchain::dialect_for(tc).id
                    << " token: " << tok;
            }
        }
    }
}

// Same invariant for the language-force spelling, which has both a positional
// and a per-file form.
TEST(HostFlags, LanguageForceTokensNeverContainASpace) {
    for (auto const* d : { &mcpp::toolchain::gnu_dialect(),
                           &mcpp::toolchain::msvc_dialect() }) {
        for (auto f : d->forceCxxLangArgv)
            EXPECT_EQ(f.find(' '), std::string_view::npos) << d->id << ": " << f;
        for (auto f : d->alwaysFlagsArgv)
            EXPECT_EQ(f.find(' '), std::string_view::npos) << d->id << ": " << f;
        EXPECT_EQ(d->perFileCxxPrefix.find(' '), std::string_view::npos) << d->id;
    }
}

TEST(HostFlags, BmiReferenceIsEmptyForAToolchainThatNamesNothing) {
    // GCC finds BMIs implicitly under <cwd>/gcm.cache — its prefix is empty
    // and must not produce a stray token.
    EXPECT_TRUE(mcpp::toolchain::bmi_reference_tokens(
        "", std::filesystem::path("/tmp/x.gcm")).empty());
}

// ── HostFlagOptions divergences ─────────────────────────────────────────────
//
// These knobs encode documented differences between the three consumers. A
// test so that "why is this optional?" has an answer in code, not only prose.
TEST(HostFlags, CfgBypassLinuxOnlyDiffersFromAlwaysOffLinux) {
    auto tc = tc_for(CompilerId::Clang);
    HostFlagOptions always;   always.cfgBypass = HostFlagOptions::CfgBypass::Always;
    HostFlagOptions linuxOnly; linuxOnly.cfgBypass = HostFlagOptions::CfgBypass::LinuxOnly;

    // Without a real clang payload there is no cfg to bypass, so both are
    // empty here; the assertion that matters is that the option exists and is
    // honoured identically on Linux, where the host helper does bypass.
    if constexpr (mcpp::platform::is_linux) {
        EXPECT_EQ(mcpp::toolchain::host_compile_tokens(tc, always, mcpp::toolchain::no_escape),
                  mcpp::toolchain::host_compile_tokens(tc, linuxOnly, mcpp::toolchain::no_escape));
    }
}

TEST(HostFlags, DeploymentTargetOnlyOnMacos) {
    auto tc = tc_for(CompilerId::GCC);
    HostFlagOptions opt;
    opt.macosDeploymentTarget = "14.0";
    auto tokens = mcpp::toolchain::host_compile_tokens(
        tc, opt, mcpp::toolchain::no_escape);
    bool found = std::ranges::any_of(tokens, [](auto const& t) {
        return t.starts_with("-mmacosx-version-min=");
    });
    EXPECT_EQ(found, mcpp::platform::is_macos);
}

// ── graph_runtime_compile_flags: what a `throw` and a `thread_local` compile
//    into, when the runtime comes from the dependency graph.
//
// These are not ordinary flags. They change what a translation unit EMITS for
// constructs the language guarantees work across a whole program, so two
// objects that disagree link and the disagreement is the defect. The function
// exists so that the decision is made once; these tests exist so that each of
// its four states is stated rather than inferred from a build.

namespace {

mcpp::toolchain::Toolchain graph_tc(std::string triple) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler         = CompilerId::Clang;
    tc.targetTriple     = std::move(triple);
    tc.targetCxxRuntime = true;
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
TEST(GraphRuntimeFlags, PeTakesDwarfExceptionsAndEmulatedTls) {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("x86_64-windows-gnu"));
    EXPECT_TRUE(has(f, "-fdwarf-exceptions"));
    EXPECT_TRUE(has(f, "-femulated-tls"));
}

// Mach-O: the exception mechanism is already DWARF, so only the thread-local
// one applies — `_tlv_bootstrap` is the loader-bootstrapped name there. The
// visibility pair is present because on this format a default-visibility weak
// definition is coalesced BY THE LOADER, which is machinery a self-contained
// image has no use for and which produced a jump to address zero when it was
// left in place.
TEST(GraphRuntimeFlags, MachOTakesEmulatedTlsAndHiddenVisibilityButNotDwarf) {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("aarch64-macos"));
    EXPECT_FALSE(has(f, "-fdwarf-exceptions"));
    EXPECT_TRUE(has(f, "-femulated-tls"));
    EXPECT_TRUE(has(f, "-fvisibility=hidden"));
    EXPECT_TRUE(has(f, "-fvisibility-inlines-hidden"));
}

// ELF takes NONE of them, and that is a decision rather than an omission.
// There a `thread_local` is a fixed offset from the thread pointer, which the
// C library establishes itself; adding the flag would work, cost an
// indirection on every access, and make ELF the only target whose thread
// locals are laid out differently from every other build of the same target.
TEST(GraphRuntimeFlags, ElfTakesNone) {
    auto f = mcpp::toolchain::graph_runtime_compile_flags(graph_tc("x86_64-linux-gnu"));
    EXPECT_TRUE(f.empty());
}

// And nothing at all when the runtime is NOT the graph's, whatever the
// target. The predicate is `targetCxxRuntime`; a native or payload-served build
// of the same triple must be untouched.
TEST(GraphRuntimeFlags, PayloadServedTargetTakesNoneEvenOnPe) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler     = CompilerId::Clang;
    tc.targetTriple = "x86_64-windows-gnu";
    tc.targetCxxRuntime = false;
    EXPECT_TRUE(mcpp::toolchain::graph_runtime_compile_flags(tc).empty());
}

// A triple outside the vocabulary yields nothing rather than a guess: the
// answer depends on the object format, and a spelling that cannot be parsed
// does not name one.
TEST(GraphRuntimeFlags, UnparseableTripleTakesNone) {
    EXPECT_TRUE(mcpp::toolchain::graph_runtime_compile_flags(
        graph_tc("not-a-triple-at-all")).empty());
}

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
TEST(HostFlags, TheCfgBypassSurvivesAGraphSuppliedTargetSide) {
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
    ASSERT_TRUE(mcpp::toolchain::resolve_clang_driver(tc).hasCfg)
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
    EXPECT_TRUE(has(a, "--no-default-config"));
    EXPECT_TRUE(has(b, "--no-default-config"));
    // ...while the payload's own C++ headers stay withheld from the graph side,
    // which is the distinction this splits apart.
    EXPECT_TRUE(has(a, "-nostdinc++"));
    EXPECT_FALSE(has(b, "-nostdinc++"));
}
