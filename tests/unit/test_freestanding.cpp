#include <gtest/gtest.h>

import std;
import mcpp.freestanding.target;
import mcpp.freestanding.linkline;
import mcpp.freestanding.runner;
import mcpp.toolchain.triple;
import mcpp.xlings;
import mcpp.build.build_program;

using namespace mcpp::freestanding;

// ── the target table ────────────────────────────────────────────────────────

TEST(FreestandingTarget, ResolvesTheKnownBareMetalTriples) {
    auto rv64 = resolve("riscv64-none-elf");
    ASSERT_TRUE(rv64.has_value());
    EXPECT_EQ(rv64->march,  "rv64gc");
    EXPECT_EQ(rv64->mabi,   "lp64d");
    EXPECT_EQ(rv64->libdir, "rv64gc/lp64d");

    auto rv32 = resolve("riscv32-none-elf");
    ASSERT_TRUE(rv32.has_value());
    EXPECT_EQ(rv32->march,  "rv32imac");
    EXPECT_EQ(rv32->mabi,   "ilp32");
    EXPECT_EQ(rv32->libdir, "rv32imac/ilp32");
}

TEST(FreestandingTarget, HostedTriplesResolveToNothing) {
    // The same call answers "is this a target I know how to build
    // freestanding", so a hosted triple must not produce a Spec — a caller
    // that got one would put -ffreestanding on an ordinary Linux build.
    for (const char* s : { "x86_64-linux-gnu", "x86_64-linux-musl",
                           "aarch64-macos", "x86_64-windows-gnu" }) {
        EXPECT_FALSE(resolve(s).has_value()) << s;
    }
}

TEST(FreestandingTarget, UnknownBareMetalTripleIsNotInvented) {
    // Parses as freestanding, but mcpp has no ISA profile for it. Returning a
    // Spec here would mean guessing an -march, and a wrong -march produces an
    // object file that links and then executes garbage.
    auto t = mcpp::toolchain::triple::parse("arm-none-eabi");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->is_freestanding());
    EXPECT_FALSE(resolve(*t).has_value());
}

TEST(FreestandingTarget, CompileFlagsCarryTheIsaAndFreestanding) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    auto fl = compile_flags(*s);
    auto has = [&](std::string_view f) {
        return std::ranges::find(fl, f) != fl.end();
    };
    EXPECT_TRUE(has("-march=rv64gc"));
    EXPECT_TRUE(has("-mabi=lp64d"));
    EXPECT_TRUE(has("-mcmodel=medany"));
    // -ffreestanding is a COMPILE flag, not a link one: it changes what the
    // compiler may assume about the library, and that has to hold for every
    // TU including a dependency's.
    EXPECT_TRUE(has("-ffreestanding"));
    // The toolchain's libc++ headers are the HOST's: `#include <stdio.h>`
    // resolves to libc++'s wrapper, which opens a `__config_site` generated
    // for the host and absent here. A target-side C++ library reaches a
    // consumer as a MODULE, never as an include path.
    EXPECT_TRUE(has("-nostdinc++"));
}

// ── the link line ───────────────────────────────────────────────────────────

TEST(FreestandingLink, CarriesTargetSelectionAndRefusesTheHostedWorld) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    LinkInputs in;
    in.lld = "/payload/bin/ld.lld";
    auto line = link_flags(*s, in, [](const std::filesystem::path& p) {
        return p.string();
    });

    // --target is the load-bearing one: clang is ONE binary for every target,
    // so without it the link silently produces host objects.
    EXPECT_NE(line.find("--target=riscv64-none-elf"), std::string::npos);
    EXPECT_NE(line.find("-nostdlib"), std::string::npos);
    EXPECT_NE(line.find("-nostartfiles"), std::string::npos);
    EXPECT_NE(line.find("-static"), std::string::npos);
}

TEST(FreestandingLink, AddressesTheLinkerByPathNotByName) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    LinkInputs in;
    in.lld = "/payload/bin/ld.lld";
    auto line = link_flags(*s, in, [](const std::filesystem::path& p) {
        return p.string();
    });
    // `-fuse-ld=lld` resolves through PATH and finds GNU ld on any machine
    // with binutils earlier on it, which then dies with "unrecognised
    // emulation mode: elf64lriscv". Reproduced 2026-08-19.
    EXPECT_NE(line.find("-fuse-ld=/payload/bin/ld.lld"), std::string::npos);
    EXPECT_EQ(line.find("-fuse-ld=lld "), std::string::npos);
}

TEST(FreestandingLink, LinkerScriptOnlyWhenOneIsSupplied) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    auto esc = [](const std::filesystem::path& p) { return p.string(); };

    LinkInputs none;
    EXPECT_EQ(link_flags(*s, none, esc).find(" -T "), std::string::npos);

    LinkInputs with;
    with.linkerScript = "/board/link.ld";
    EXPECT_NE(link_flags(*s, with, esc).find(" -T /board/link.ld"),
              std::string::npos);
}

TEST(FreestandingLink, LldIsIdentifiedByItsOwnVersionOutput) {
    // By output, not by filename — the filename is exactly what was wrong in
    // the failure this guards.
    EXPECT_TRUE(version_output_is_lld("LLD 22.1.8 (compatible with GNU linkers)"));
    EXPECT_FALSE(version_output_is_lld("GNU ld (GNU Binutils for Ubuntu) 2.42"));
}

// ── the runner ──────────────────────────────────────────────────────────────

TEST(FreestandingRunner, AppendsTheArtifactWhenNoPlaceholder) {
    std::vector<std::string> tmpl{ "qemu-system-riscv64", "-machine", "virt",
                                   "-kernel" };
    auto argv = expand(tmpl, "/build/firmware");
    ASSERT_EQ(argv.size(), 5u);
    EXPECT_EQ(argv.front(), "qemu-system-riscv64");
    EXPECT_EQ(argv.back(),  "/build/firmware");
}

TEST(FreestandingRunner, SubstitutesThePlaceholderInPlace) {
    // The emulator that wants the image in the middle rather than at the end.
    std::vector<std::string> tmpl{ "runner", "--image={}", "--verbose" };
    auto argv = expand(tmpl, "/build/firmware");
    ASSERT_EQ(argv.size(), 3u);
    EXPECT_EQ(argv[1], "--image=/build/firmware");
    EXPECT_EQ(argv[2], "--verbose");   // nothing appended when substituted
}

TEST(FreestandingRunner, TheMissingRunnerMessageNamesTheKeyAndTheTriple) {
    // The user has just been told the build succeeded and the run did not; the
    // gap between those two is the whole content of the message, so it has to
    // carry something pasteable.
    auto msg = no_runner_message("riscv64-none-elf");
    EXPECT_NE(msg.find("[target.riscv64-none-elf]"), std::string::npos);
    EXPECT_NE(msg.find("runner = ["), std::string::npos);
    EXPECT_NE(msg.find("board-support package"), std::string::npos);
}

// ── `[xlings] deps` → payload directory ─────────────────────────────────────
//
// The channel a board-support package uses to find the libc payload it
// declared. An INTERFACE rather than a naming convention: a build.mcpp that
// reconstructed `<home>/data/xpkgs/<ns>-x-<name>/<version>` would be coupled
// to store internals mcpp is free to change.

TEST(XpkgRef, ParsesEverySpellingAManifestMayWrite) {
    using mcpp::xlings::paths::parse_xpkg_ref;

    auto full = parse_xpkg_ref("xim:picolibc-riscv@1.8.12");
    EXPECT_EQ(full.ns, "xim");
    EXPECT_EQ(full.name, "picolibc-riscv");
    EXPECT_EQ(full.version, "1.8.12");

    // No namespace: the manifest's default, spelled once in the parser rather
    // than at each call site.
    auto bare = parse_xpkg_ref("ninja");
    EXPECT_EQ(bare.ns, "xim");
    EXPECT_EQ(bare.name, "ninja");
    EXPECT_TRUE(bare.version.empty());

    // A non-xim namespace is ordinary, not exotic: `local:` is what a package
    // under development resolves to.
    auto local = parse_xpkg_ref("local:qemu-riscv@9.2.4-1");
    EXPECT_EQ(local.ns, "local");
    EXPECT_EQ(local.name, "qemu-riscv");
    EXPECT_EQ(local.version, "9.2.4-1");
}

TEST(XpkgPayload, APinnedRefResolvesToThatVersionOrToNothing) {
    namespace xp = mcpp::xlings::paths;
    auto base = std::filesystem::temp_directory_path()
              / std::format("mcpp-xpkg-test-{}", ::getpid());
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "xim-x-demo" / "1.0.0");
    std::filesystem::create_directories(base / "xim-x-demo" / "2.0.0");

    // ⚠️ The whole point: asking for 1.0.0 and silently getting 2.0.0 is an
    // answer only discovered later, in the artifact.
    auto pinned = xp::xpkg_payload_at(base, xp::parse_xpkg_ref("xim:demo@1.0.0"));
    ASSERT_TRUE(pinned.has_value());
    EXPECT_EQ(pinned->filename(), "1.0.0");

    EXPECT_FALSE(xp::xpkg_payload_at(base, xp::parse_xpkg_ref("xim:demo@9.9.9"))
                     .has_value());

    std::filesystem::remove_all(base);
}

TEST(XpkgPayload, AnUnpinnedRefTakesTheHighestVersionNumerically) {
    namespace xp = mcpp::xlings::paths;
    auto base = std::filesystem::temp_directory_path()
              / std::format("mcpp-xpkg-test2-{}", ::getpid());
    std::filesystem::remove_all(base);
    for (auto v : { "0.4.2", "0.4.9", "0.4.11" })
        std::filesystem::create_directories(base / "xim-x-demo" / v);

    // A plain string sort answers 0.4.9; picking the wrong payload is silent.
    auto latest = xp::xpkg_payload_at(base, xp::parse_xpkg_ref("demo"));
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->filename(), "0.4.11");

    std::filesystem::remove_all(base);
}

TEST(XpkgEnvVar, BothSpellingsAreDerivedFromOneSanitizer) {
    using mcpp::build::xpkg_env_var;
    // The two sides of the channel must agree; drifting apart would make the
    // interface answer "" for a package that is right there.
    EXPECT_EQ(xpkg_env_var("xim", "picolibc-riscv"),
              "MCPP_XPKG_XIM_PICOLIBC_RISCV_DIR");
    EXPECT_EQ(xpkg_env_var("", "picolibc-riscv"),
              "MCPP_XPKG_PICOLIBC_RISCV_DIR");
}

// ── the target owns its C library ───────────────────────────────────────────

TEST(FreestandingTarget, BareMetalRowsNameTheirSysroot) {
    // ⚠️ The axis that stops every bare-metal PACKAGE from naming a libc.
    // Before it, a board-support package and a standard-library subset each
    // had to carry `[xlings] deps = ["xim:picolibc-riscv@..."]`, which bound
    // both to one libc, one ISA and one version of each — none of which is a
    // property of either package.
    for (const char* s : { "riscv64-none-elf", "riscv32-none-elf" }) {
        auto* k = mcpp::toolchain::triple::find_known_target(
            *mcpp::toolchain::triple::parse(s));
        ASSERT_NE(k, nullptr) << s;
        EXPECT_FALSE(k->sysroot.empty()) << s;
        // Same spelling the install channel takes, so the two cannot drift.
        EXPECT_TRUE(k->sysroot.starts_with("xim:")) << s;
    }
}

TEST(FreestandingTarget, HostedRowsNameNone) {
    // A hosted target's libc comes with the compiler payload or through the
    // runtime binding. Naming one here would resolve a second copy.
    for (const char* s : { "x86_64-linux-gnu", "x86_64-linux-musl",
                           "aarch64-macos", "x86_64-windows-gnu" }) {
        auto* k = mcpp::toolchain::triple::find_known_target(
            *mcpp::toolchain::triple::parse(s));
        ASSERT_NE(k, nullptr) << s;
        EXPECT_TRUE(k->sysroot.empty()) << s;
    }
}

// ── whole-graph properties ──────────────────────────────────────────────────

TEST(FreestandingFlags, ExceptionsAndRttiAreOffForTheWholeGraph) {
    auto spec = resolve("riscv64-none-elf");
    ASSERT_TRUE(spec.has_value());
    auto f = compile_flags(*spec);
    // ⚠️ These live with the TARGET, not in a project's cxxflags, because a
    // BMI records them: a dependency compiled with exceptions cannot be
    // imported by a TU without them, and clang reports that as a .pcm
    // "configuration mismatch" rather than as a flag disagreement.
    EXPECT_TRUE(std::ranges::find(f, "-fno-exceptions") != f.end());
    EXPECT_TRUE(std::ranges::find(f, "-fno-rtti") != f.end());
    // The company they keep — same argument, already shipped.
    EXPECT_TRUE(std::ranges::find(f, "-ffreestanding") != f.end());
    EXPECT_TRUE(std::ranges::find(f, "-nostdinc++") != f.end());
}

TEST(FreestandingFlags, HostedTargetsGetNoneOfThis) {
    // The flags are a property of a freestanding triple, and the cache key
    // reads them through the same resolve(). A hosted target must produce no
    // Spec, or every hosted key would move for no reason.
    EXPECT_FALSE(resolve("x86_64-linux-gnu").has_value());
}

// ── engine floor: a package that needs a newer mcpp ─────────────────────────

TEST(BuildProgramCompatHint, RecognisesAllThreeFrontendSpellings) {
    using mcpp::build::mentions_missing_mcpp_api;
    // ⚠️ Measured, not assumed: `if constexpr (requires { mcpp::runner("x"); })`
    // is a HARD ERROR when the name is absent, so a package cannot probe for a
    // newer API in-language. The compiler's error IS the compat channel, and
    // these are the three ways it arrives.
    EXPECT_TRUE(mentions_missing_mcpp_api(
        "build.mcpp:57:11: error: 'runner' is not a member of 'mcpp'"));          // gcc
    EXPECT_TRUE(mentions_missing_mcpp_api(
        "build.mcpp:57:11: error: no member named 'runner' in namespace 'mcpp'"));// clang
    EXPECT_TRUE(mentions_missing_mcpp_api(
        "build.mcpp(57): error C2039: 'runner': is not a member of 'mcpp'"));     // cl.exe
}

TEST(BuildProgramCompatHint, StaysOffFailuresThatAreNotAboutOurApi) {
    using mcpp::build::mentions_missing_mcpp_api;
    // The hint says "your mcpp may be too old". Attaching that to an ordinary
    // compile error would send the reader to the wrong place, so the match is
    // anchored on OUR namespace — a package's own missing symbol names its own.
    EXPECT_FALSE(mentions_missing_mcpp_api(
        "build.mcpp:12:5: error: 'runner' is not a member of 'board'"));
    EXPECT_FALSE(mentions_missing_mcpp_api(
        "build.mcpp:3:1: error: expected ';' after top level declarator"));
    EXPECT_FALSE(mentions_missing_mcpp_api(
        "build.mcpp:9:9: error: use of undeclared identifier 'mcpp_runner'"));
}

// ── artifact set ───────────────────────────────────────────────────────────

TEST(FreestandingArtifacts, SizeOutputIsParsedNotPassedThrough) {
    // Parsed so the printed shape is mcpp's, not the tool's: llvm-size and GNU
    // size differ in their headers, and a build whose output changed
    // appearance with the toolchain would be reporting the tool, not the size.
    auto s = parse_size_output(
        "   text\t   data\t    bss\t    dec\t    hex\tfilename\n"
        "   8836\t     80\t   5668\t  14584\t   38f8\tfirmware\n");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->text, 8836);
    EXPECT_EQ(s->data, 80);
    EXPECT_EQ(s->bss,  5668);
    // The number that decides whether the image fits.
    EXPECT_EQ(size_total(*s), 14584);
}

TEST(FreestandingArtifacts, MalformedSizeOutputIsNotGuessedAt) {
    // An informational line has no standing to invent numbers.
    EXPECT_FALSE(parse_size_output("").has_value());
    EXPECT_FALSE(parse_size_output("size: cannot open 'x'\n").has_value());
}

TEST(FreestandingArtifacts, MapFlagNamesTheArtifact) {
    auto f = map_flag("/build/bin/firmware",
                      [](const std::filesystem::path& p) { return p.string(); });
    EXPECT_EQ(f, " -Wl,-Map=/build/bin/firmware.map");
}

TEST(FreestandingArtifacts, ObjcopyOnlyResolvesForABareMetalTarget) {
    // A hosted binary is loaded by a loader that wants the ELF, so there is
    // nothing to convert — and resolving a tool for it would put an edge in
    // every hosted graph.
    EXPECT_TRUE(resolve_objcopy("/payload/bin/clang++", "x86_64-linux-gnu").empty());
    // (The bare-metal case needs a real payload on disk, so it is asserted by
    // tests/e2e/130 instead: the artifact set has to actually appear.)
}

// ── aarch64-none-elf: the second bare-metal architecture ────────────────────
//
// ⚠️ THESE ASSERT THE TWO VALUES THAT WOULD HAVE BEEN WRONG IF THE ROW HAD BEEN
// FILLED IN BY ANALOGY WITH THE RISC-V ROWS ABOVE IT.
//
// `-mabi` is the first. RISC-V spells its ABI as a data model (`lp64d`,
// `ilp32`), and aarch64's LP64 data model makes `lp64` the obvious entry — which
// clang rejects outright: `error: unknown target ABI 'lp64'`. aarch64's `-mabi`
// names a procedure call standard, and `aapcs` is the only value it takes here.
// Measured before the row was written rather than after it failed.
//
// The code model is the second. `medany` exists on the RISC-V rows because the
// default assumes the low 2GiB and bare-metal RISC-V runs at 0x80000000;
// aarch64's `small` already addresses +/-4GiB PC-relative, which covers any
// single image wherever a linker script places it.
TEST(FreestandingTarget, Aarch64UsesTheProcedureCallStandardNotADataModel) {
    auto t = mcpp::toolchain::triple::parse("aarch64-none-elf");
    ASSERT_TRUE(t.has_value());
    auto s = mcpp::freestanding::resolve(t->str());
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->mabi, "aapcs");
    EXPECT_NE(s->mabi, "lp64");
    EXPECT_EQ(s->march, "armv8-a");
    EXPECT_EQ(s->mcmodel, "small");
}

// The row resolves no C library, which is what makes it the zero-libc tier: the
// first consumer of this target is a layer of machine mechanism that references
// no C library symbol, and there is no aarch64 picolibc in the index to resolve
// even if it wanted one.
TEST(FreestandingTarget, Aarch64ResolvesNoCLibrary) {
    auto t = mcpp::toolchain::triple::parse("aarch64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(mcpp::toolchain::triple::effective_sysroot(*t, nullptr).empty());

    // ⚠️ The other side: a project may still ASK for one, and the override is
    // what carries the request. An empty column means "nothing by default", not
    // "nothing is possible".
    const std::string want = "xim:some-aarch64-libc@1.0";
    EXPECT_EQ(mcpp::toolchain::triple::effective_sysroot(*t, &want), want);
}

// ⚠️ THIS TEST USED TO ASSERT THE OPPOSITE, AND IT WAS RIGHT UNTIL 2026-08-21.
//
// The libdir column names the sub-directory of a MULTILIB C library, and the
// aarch64 and x86_64 rows had none in the index to point into — so an empty
// column asserted something true, and a consumer reading it would have been
// reading a value nothing could check.
//
// `xim:picolibc-aarch64` and `xim:picolibc-x86` now exist, and their payloads
// use picolibc's own `<march>/<mabi>` convention. Measured with the column
// still empty: a project declaring
// `[target.aarch64-none-elf] sysroot = "xim:picolibc-aarch64@1.8.12"` got
// `'stdio.h' file not found` while the package sat installed and correct.
//
// ⭐ The values below are a CROSS-REPOSITORY fact: they must match the
// directory layout those two packages ship. `xim-pkgindex`'s
// `.agents/tools/build-baremetal-sysroot.sh` produces them from the same
// march/mabi pair this table carries, so the two cannot drift silently — but
// they are named here so a reader knows where the other half lives.
TEST(FreestandingTarget, MultilibDirectoriesMatchTheIndexPayloads) {
    auto a = mcpp::freestanding::resolve("aarch64-none-elf");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->libdir, "armv8-a/aapcs");
    // The directory is the march/mabi pair, so it cannot disagree with the
    // flags the same row produces.
    EXPECT_EQ(a->libdir, std::string(a->march) + "/" + std::string(a->mabi));

    auto x = mcpp::freestanding::resolve("x86_64-none-elf");
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(x->libdir, "x86-64/sysv");
    EXPECT_EQ(x->libdir, std::string(x->march) + "/" + std::string(x->mabi));

    // ⚠️ Filling this column does NOT give those targets a C library by
    // default. It is consulted only once a sysroot has been RESOLVED, and the
    // target table still binds none for these two rows — the zero-libc tier
    // stays the default and the package stays opt-in.
    auto t = mcpp::toolchain::triple::parse("aarch64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(mcpp::toolchain::triple::effective_sysroot(*t, nullptr).empty());
}

// ── The two tables keyed on the same triple must agree ───────────────────────
//
// ⚠️ A BARE-METAL TARGET IS DESCRIBED TWICE, IN TWO FILES, AND NOTHING WAS
// CHECKING THAT THE TWO DESCRIPTIONS COVER THE SAME ROWS.
//
// `kKnownTargets` (toolchain/triple.cppm) says a row exists, which tier it is
// in and what supplies its C library. `kTable` (freestanding/target.cppm) says
// what its ISA profile is and how its link is driven. A row present in the
// first and absent from the second parses, resolves a toolchain, and fails
// somewhere inside code generation with a message about flags rather than
// about a missing row. The reverse — present in the second, absent from the
// first — is dead data that reads as support.
//
// ⭐ The check is possible only because both are compile-time tables with a
// single read point each. It costs fifteen lines and removes an entire class
// of "added a target, forgot half of it".
TEST(FreestandingTarget, EveryBareRowInTheTargetTableHasAnIsaProfile) {
    for (const auto& info : mcpp::toolchain::triple::known_targets()) {
        if (info.note != "bare") continue;
        auto t = mcpp::toolchain::triple::parse(info.canonical);
        ASSERT_TRUE(t.has_value()) << info.canonical;
        EXPECT_TRUE(mcpp::freestanding::resolve(*t).has_value())
            << info.canonical
            << " is a bare row in kKnownTargets with no row in freestanding::kTable";
    }
}

TEST(FreestandingTarget, EveryIsaProfileHasABareRowInTheTargetTable) {
    for (const auto& spec : mcpp::freestanding::known()) {
        auto t = mcpp::toolchain::triple::parse(spec.triple);
        ASSERT_TRUE(t.has_value()) << spec.triple;
        const auto* info = mcpp::toolchain::triple::find_known_target(*t);
        ASSERT_NE(info, nullptr)
            << spec.triple << " has an ISA profile but no row in kKnownTargets";
        EXPECT_EQ(info->note, "bare")
            << spec.triple << " has an ISA profile but is not marked bare";
    }
}

// ── Cortex-M ────────────────────────────────────────────────────────────────
//
// ⭐ THESE ARE RULES OVER THE TABLE, WHICH IS THE ONLY PLACE THEY CAN BE
// STATED. The e2e boots four M-profile images and measures instruction counts,
// but it can only speak about the rows it happens to exercise. A rule quantified
// over every row is what stops the eighth one from being added wrong, and the
// property it protects is invisible in a build: a soft-float row that reaches
// the FPU compiles, links, and faults on real silicon.

namespace {
// ⚠️ EVERY 32-BIT ARM ROW, NOT ONLY THE M-PROFILE ONES. The first version of
// this predicate was `starts_with("thumb")` — a SPELLING rather than the
// property the rule is about. When `armv7a-none-eabi` was added, the rule
// applied to it and this test did not, silently: the loop simply skipped the
// new rows and every assertion still passed. The property is "32-bit ARM with
// a float-ABI suffix", and both spellings have it.
bool is_arm32(std::string_view triple) {
    return triple.starts_with("thumb") || triple.starts_with("armv");
}
bool has_flag(const mcpp::freestanding::Spec& s, std::string_view f) {
    for (auto e : s.extra) if (e == f) return true;
    return false;
}
}  // namespace

// The pair that the float-ABI suffix does NOT settle on its own. clang reads
// `eabi`/`eabihf` and sets `-mfloat-abi` accordingly, but the ABI governs how
// floats cross a call boundary, not whether the compiler may use the FPU inside
// one — and `thumbv7em` implies an FPU. Measured on llvm 22.1.8: without
// `-mfpu=none` the soft row emits `vmul.f32`.
TEST(FreestandingTarget, SoftFloatArm32RowsDisableTheFpu) {
    int soft = 0, hard = 0;
    for (const auto& spec : mcpp::freestanding::known()) {
        if (!is_arm32(spec.triple)) continue;
        if (spec.triple.ends_with("-eabihf")) {
            ++hard;
            EXPECT_FALSE(has_flag(spec, "-mfpu=none"))
                << spec.triple << " is a hard-float row and must not discard the FPU";
        } else {
            ++soft;
            EXPECT_TRUE(has_flag(spec, "-mfpu=none"))
                << spec.triple
                << " is a soft-float row without -mfpu=none: it will emit FPU"
                   " instructions that fault on a part with no FPU";
        }
    }
    // ⚠️ A denominator, because a loop that never ran satisfies every EXPECT
    // above. Both halves must be non-empty for the contrast to mean anything.
    EXPECT_GT(soft, 0);
    EXPECT_GT(hard, 0);
    // And the A-profile rows are in the denominator, which is the half a
    // spelling-based predicate lost without failing.
    int a_profile = 0;
    for (const auto& spec : mcpp::freestanding::known())
        if (spec.triple.starts_with("armv")) ++a_profile;
    EXPECT_GT(a_profile, 0);
}

// 32-bit ARM has no `-mcmodel`, and clang has a BareMetal toolchain for arm so
// its driver reaches `ld.lld` without the x86_64 row's workaround. Both columns
// being empty is what says so; a filled one would be a value nothing checks.
TEST(FreestandingTarget, Arm32RowsNeedNoCodeModelAndNoDirectLldDriving) {
    int rows = 0;
    for (const auto& spec : mcpp::freestanding::known()) {
        if (!is_arm32(spec.triple)) continue;
        ++rows;
        EXPECT_TRUE(spec.mcmodel.empty()) << spec.triple << " has no -mcmodel axis";
        EXPECT_TRUE(spec.lldEmulation.empty())
            << spec.triple << " reaches ld.lld through the clang driver";
        EXPECT_EQ(spec.mabi, "aapcs") << spec.triple;
    }
    EXPECT_GT(rows, 0);
}

// The compile half of `--gc-sections`, which has to hold for EVERY freestanding
// target rather than only the M-profile ones: a dependency's translation units
// carry these flags too, and a project cannot reach those.
TEST(FreestandingTarget, EveryRowCompilesWithPerFunctionSections) {
    int rows = 0;
    for (const auto& spec : mcpp::freestanding::known()) {
        ++rows;
        auto flags = mcpp::freestanding::compile_flags(spec);
        EXPECT_NE(std::find(flags.begin(), flags.end(), "-ffunction-sections"),
                  flags.end()) << spec.triple;
        EXPECT_NE(std::find(flags.begin(), flags.end(), "-fdata-sections"),
                  flags.end()) << spec.triple;
    }
    EXPECT_GT(rows, 0);
}
