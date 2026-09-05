#include <gtest/gtest.h>

import std;
import mcpp.toolchain.triple;

using namespace mcpp::toolchain::triple;
namespace triple = mcpp::toolchain::triple;

// ── parse: canonical spellings round-trip ────────────────────────────────────

TEST(Triple, ParsesCanonicalThreeSegment) {
    auto t = parse("x86_64-linux-musl");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "x86_64");
    EXPECT_EQ(t->os, "linux");
    EXPECT_EQ(t->env, "musl");
    EXPECT_EQ(t->str(), "x86_64-linux-musl");
    EXPECT_TRUE(t->is_musl());
    EXPECT_TRUE(is_known_target(*t));
}

TEST(Triple, ParsesCanonicalWindowsGnu) {
    auto t = parse("x86_64-windows-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "x86_64-windows-gnu");
    EXPECT_TRUE(t->is_windows_gnu());
    EXPECT_TRUE(t->is_pe());
    EXPECT_EQ(t->family(), "windows");
    EXPECT_TRUE(is_known_target(*t));
}

TEST(Triple, ParsesCanonicalMacos) {
    auto t = parse("aarch64-macos");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "aarch64");
    EXPECT_EQ(t->os, "macos");
    EXPECT_EQ(t->env, "");
    EXPECT_EQ(t->str(), "aarch64-macos");
    EXPECT_EQ(t->family(), "unix");
}

// ── parse: alias spellings normalize to canonical ────────────────────────────

TEST(Triple, NormalizesGnuMingwSpelling) {
    // GNU vendor triple: "w64" = vendor, "mingw32" = os segment (historical —
    // 64-bit targets still say mingw32). Canonicalizes to windows-gnu.
    auto t = parse("x86_64-w64-mingw32");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "x86_64-windows-gnu");
    EXPECT_TRUE(t->is_windows_gnu());
}

TEST(Triple, NormalizesFourSegmentRustSpellings) {
    EXPECT_EQ(parse("x86_64-unknown-linux-musl")->str(), "x86_64-linux-musl");
    EXPECT_EQ(parse("x86_64-unknown-linux-gnu")->str(), "x86_64-linux-gnu");
    EXPECT_EQ(parse("x86_64-pc-windows-msvc")->str(), "x86_64-windows-msvc");
    EXPECT_EQ(parse("x86_64-pc-windows-gnu")->str(), "x86_64-windows-gnu");
}

TEST(Triple, NormalizesAppleSpellings) {
    // arm64 → aarch64 (GNU arch spelling); darwin/macosx version suffixes drop.
    EXPECT_EQ(parse("arm64-apple-darwin24.1.0")->str(), "aarch64-macos");
    EXPECT_EQ(parse("arm64-apple-macosx15.0")->str(), "aarch64-macos");
    EXPECT_EQ(parse("aarch64-apple-darwin")->str(), "aarch64-macos");
}

TEST(Triple, NormalizesBareLinuxToGnuEnv) {
    EXPECT_EQ(parse("x86_64-linux")->str(), "x86_64-linux-gnu");
}

// ── resolve_request: the REQUEST is completed from the vocabulary ────────────
//
// `parse` fills lexically so the IDENTITY stays total and host-independent (the
// test just above pins that). `resolve_request` answers the other half — which
// row a request that named no C library should resolve to — and it is the one
// that consults `kKnownTargets`.

TEST(TripleRequest, ASupportedLexicalDefaultIsKept) {
    // x86_64-linux-gnu is `verified`, so nothing moves. This is the control for
    // the case below: a fix that sent every bare `-linux` to musl would pass the
    // aarch64 test and break every project on the planet.
    auto r = triple::resolve_request(*parse("x86_64-linux"));
    EXPECT_EQ(r.triple.str(), "x86_64-linux-gnu");
    EXPECT_FALSE(r.completedFromVocabulary);
    EXPECT_FALSE(r.ambiguous);
}

TEST(TripleRequest, TheOnlySupportedSiblingIsTaken) {
    // aarch64-linux-gnu is `planned`; aarch64-linux-musl is `verified`. Measured
    // on 2026.8.26.1: `--target aarch64-linux` refused as planned while
    // `--target aarch64-linux-musl` built.
    auto r = triple::resolve_request(*parse("aarch64-linux"));
    EXPECT_EQ(r.triple.str(), "aarch64-linux-musl");
    EXPECT_TRUE(r.completedFromVocabulary);
    // mcpp CHOOSING A ROW IS NOT THE PROJECT NAMING A C LIBRARY. `envExplicit`
    // feeds the request/fact comparison and the report's display name; setting
    // it here would make mcpp compare its own answer against itself.
    EXPECT_FALSE(r.triple.envExplicit);
}

TEST(TripleRequest, AWrittenSegmentIsARequestAndIsNotRevised) {
    // The escape hatch: writing the segment opts into the `planned` row, and the
    // tier gate then refuses something the user actually typed.
    auto r = triple::resolve_request(*parse("aarch64-linux-gnu"));
    EXPECT_EQ(r.triple.str(), "aarch64-linux-gnu");
    EXPECT_FALSE(r.completedFromVocabulary);
}

TEST(TripleRequest, AFamilyWithNoSupportedRowKeepsTheLexicalFillAndReportsItsRows) {
    // riscv64-linux-musl is `planned` and riscv64-linux-gnu does not exist at
    // all, so the fill named a row outside the vocabulary and the refusal came
    // out as `unknown target 'riscv64-linux'` — false, the family is registered.
    auto r = triple::resolve_request(*parse("riscv64-linux"));
    EXPECT_EQ(r.triple.str(), "riscv64-linux-gnu");
    EXPECT_FALSE(r.completedFromVocabulary);
    ASSERT_EQ(r.siblings.size(), 1u);
    EXPECT_EQ(r.siblings[0], "riscv64-linux-musl");
    EXPECT_TRUE(r.supported.empty());
}

TEST(TripleRequest, MacosCarriesNoEnvAndIsLeftAlone) {
    EXPECT_EQ(triple::resolve_request(*parse("aarch64-macos")).triple.str(),
              "aarch64-macos");
    // x86_64-macos is `planned` with no sibling: the tier gate still speaks.
    auto r = triple::resolve_request(*parse("x86_64-macos"));
    EXPECT_EQ(r.triple.str(), "x86_64-macos");
    EXPECT_TRUE(r.supported.empty());
}

TEST(TripleRequest, WindowsAndBareMetalDefaultsAreSupportedRows) {
    // Both lexical fills (`gnu` on Windows, `elf` freestanding) name supported
    // rows, so completion is a no-op — recorded so that a future row change
    // which breaks that shows up here rather than in a user's build.
    EXPECT_EQ(triple::resolve_request(*parse("x86_64-windows")).triple.str(),
              "x86_64-windows-gnu");
    EXPECT_EQ(triple::resolve_request(*parse("riscv64-none")).triple.str(),
              "riscv64-none-elf");
}

TEST(TripleRequest, EverySupportedRowIsReachableFromItsOwnSpelling) {
    // The completion must never turn a written, supported triple into a
    // different one. Checked across the whole vocabulary rather than by example,
    // because the failure mode is one row nobody thought to test.
    for (auto const& row : triple::known_targets()) {
        if (row.tier == "planned") continue;
        auto t = parse(row.canonical);
        ASSERT_TRUE(t.has_value()) << row.canonical;
        EXPECT_EQ(triple::resolve_request(*t).triple.str(),
                  std::string(row.canonical));
    }
}

TEST(Triple, NormalizesDumpmachineSpellings) {
    // What real toolchains report via -dumpmachine.
    EXPECT_EQ(parse("x86_64-pc-linux-gnu")->str(), "x86_64-linux-gnu");
    EXPECT_EQ(parse("x86_64-linux-gnu")->str(), "x86_64-linux-gnu");
    EXPECT_EQ(parse("aarch64-linux-musl")->str(), "aarch64-linux-musl");
}

// ── parse: rejects non-triples ───────────────────────────────────────────────

TEST(Triple, RejectsNonTriples) {
    EXPECT_FALSE(parse("").has_value());
    EXPECT_FALSE(parse("gcc").has_value());
    EXPECT_FALSE(parse("x86_64").has_value());
    EXPECT_FALSE(parse("x86_64-linux-mus").has_value());   // typo'd env segment
    EXPECT_FALSE(parse("wasm32-wasi").has_value());        // outside the language
}

// ── known-target vocabulary ──────────────────────────────────────────────────

TEST(Triple, KnownTargetTableExposesTierAndPins) {
    auto t = parse("x86_64-linux-musl");
    ASSERT_TRUE(t.has_value());
    auto* info = find_known_target(*t);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->tier, "verified");
    EXPECT_EQ(info->pin, "gcc@16.1.0");
    EXPECT_TRUE(info->defaultStatic);

    auto w = parse("x86_64-w64-mingw32");   // alias resolves to the same row
    ASSERT_TRUE(w.has_value());
    auto* winfo = find_known_target(*w);
    ASSERT_NE(winfo, nullptr);
    EXPECT_EQ(winfo->canonical, "x86_64-windows-gnu");
    EXPECT_EQ(winfo->pin, "gcc@16.1.0");
    EXPECT_TRUE(winfo->defaultStatic);
}

TEST(Triple, UnknownButParseableTripleIsNotKnown) {
    auto t = parse("riscv64-linux-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(is_known_target(*t));
}

// ── did-you-mean ─────────────────────────────────────────────────────────────

TEST(Triple, DidYouMeanCatchesTypos) {
    auto s = did_you_mean("x86_64-linux-mus");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "x86_64-linux-musl");

    auto w = did_you_mean("x86_64-w64-mingw");
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(*w, "x86_64-windows-gnu");
}

TEST(Triple, DidYouMeanStaysQuietOnGarbage) {
    EXPECT_FALSE(did_you_mean("totally-unrelated-string-xyz").has_value());
}

// ── nasm output format ───────────────────────────────────────────────────────

TEST(Triple, NasmFormatCoversX86Targets) {
    EXPECT_EQ(parse("x86_64-linux-gnu")->nasm_format(), "elf64");
    EXPECT_EQ(parse("x86_64-linux-musl")->nasm_format(), "elf64");
    EXPECT_EQ(parse("x86_64-windows-gnu")->nasm_format(), "win64");
    EXPECT_EQ(parse("x86_64-windows-msvc")->nasm_format(), "win64");
    EXPECT_EQ(parse("x86_64-macos")->nasm_format(), "macho64");
    EXPECT_EQ(parse("i686-linux-gnu")->nasm_format(), "elf32");
    EXPECT_EQ(parse("i686-windows-gnu")->nasm_format(), "win32");
}

TEST(Triple, NasmFormatIsNulloptOffX86) {
    // NASM is an x86-family assembler; non-x86 targets have no format and the
    // caller must hard-error (with a cfg-gating hint) instead of guessing.
    EXPECT_FALSE(parse("aarch64-linux-musl")->nasm_format().has_value());
    EXPECT_FALSE(parse("aarch64-macos")->nasm_format().has_value());
    EXPECT_FALSE(parse("riscv64-linux-musl")->nasm_format().has_value());
}

// ── host ─────────────────────────────────────────────────────────────────────

TEST(Triple, HostTripleIsCanonicalAndNonEmpty) {
    auto h = host_triple();
    EXPECT_FALSE(h.empty());
    auto reparsed = parse(h.str());
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_EQ(reparsed->str(), h.str());
}

// ── bare metal: `none` is a vendor segment AND an OS segment ─────────────────
//
// Which one it is depends on the rest of the triple, so it cannot be decided
// in a single left-to-right pass. Both sides are pinned here because getting
// it backwards is a SILENT failure, not a parse error: `riscv64-none-elf`
// would fall through to the host and the build would report success while
// producing an x86-64 binary.

TEST(Triple, NoneIsTheOsWhenNoOtherOsToken) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "riscv64");
    EXPECT_EQ(t->os, "none");
    EXPECT_EQ(t->env, "elf");
    EXPECT_EQ(t->str(), "riscv64-none-elf");
    EXPECT_TRUE(t->is_freestanding());
    EXPECT_TRUE(is_known_target(*t));
    // No OS means no cfg family — a freestanding target is neither unix nor
    // windows, and claiming either would enable the wrong cfg branches.
    EXPECT_EQ(t->family(), "");
}

TEST(Triple, NoneStaysAVendorWhenARealOsFollows) {
    auto t = parse("x86_64-none-linux-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->os, "linux");
    EXPECT_EQ(t->env, "gnu");
    EXPECT_FALSE(t->is_freestanding());
    EXPECT_EQ(t->family(), "unix");
}

TEST(Triple, Riscv32BareMetalParses) {
    auto t = parse("riscv32-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "riscv32-none-elf");
    EXPECT_TRUE(t->is_freestanding());
    EXPECT_TRUE(is_known_target(*t));
}

TEST(Triple, HostedTargetsAreNotFreestanding) {
    for (const char* s : { "x86_64-linux-gnu", "x86_64-linux-musl",
                           "x86_64-windows-gnu", "aarch64-macos" }) {
        auto t = parse(s);
        ASSERT_TRUE(t.has_value()) << s;
        EXPECT_FALSE(t->is_freestanding()) << s;
    }
}

TEST(Triple, BareMetalEabiSpellings) {
    // Accepted only under os=none, so a hosted triple cannot pick them up.
    auto hf = parse("arm-none-eabihf");
    ASSERT_TRUE(hf.has_value());
    EXPECT_EQ(hf->os, "none");
    EXPECT_EQ(hf->env, "eabihf");
    // Parsing is not the same as being supported: arm is not in the
    // vocabulary table yet, and the target gate is what says so.
    EXPECT_FALSE(is_known_target(*hf));
}

// ── effective_sysroot: the project's override, the target row otherwise ──────
//
// The override arrives as a POINTER — null means the project declared none. It
// was an `std::optional<std::string>` until that parameter type, reaching an
// exported module interface, broke every importer under clang + MSVC STL.
const std::string kNewlib = "xim:newlib-riscv@4.4";
const std::string kEmpty  = "";

//
// The absent/empty distinction is the whole point of these three tests. A
// plain `std::string` would make "the project said nothing" and "the project
// asked for no C library" the same value, and a kernel project would silently
// get picolibc back.

TEST(Triple, EffectiveSysrootFallsBackToTheTargetRow) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, nullptr), "xim:picolibc-riscv@1.8.12");
}

TEST(Triple, EffectiveSysrootHonoursAnOverride) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, &kNewlib),
              "xim:newlib-riscv@4.4");
}

TEST(Triple, EffectiveSysrootEmptyStringIsTheZeroLibcTier) {
    // Present-and-empty must NOT fall through to the target row. Measured
    // end-to-end alongside this: with `sysroot = ""`, `#include <stdio.h>`
    // stops resolving and a self-contained image links at 108 bytes.
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, &kEmpty), "");
}

TEST(Triple, EffectiveSysrootIsEmptyForHostedTargets) {
    auto t = parse("x86_64-linux-musl");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, nullptr), "");
}

// ── llvm_triple: the spelling a compiler takes, which is not the one mcpp uses

// mcpp's canonical form and LLVM's four-field form differ in more than
// punctuation: on Apple platforms the architecture has a different name and the
// operating system carries a version. Every one of these was a defect the
// three-host matrix found rather than a test.

TEST(Triple, LlvmTripleLinuxGnu) {
    auto t = parse("x86_64-linux-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->llvm_triple(""), "x86_64-unknown-linux-gnu");
}

TEST(Triple, LlvmTripleLinuxMusl) {
    auto t = parse("aarch64-linux-musl");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->llvm_triple(""), "aarch64-unknown-linux-musl");
}

TEST(Triple, LlvmTripleWindowsGnu) {
    auto t = parse("x86_64-windows-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->llvm_triple(""), "x86_64-w64-windows-gnu");
}

// `aarch64` becomes `arm64` and the version is appended. A build that
// emitted mcpp's own spelling produced `--target=aarch64-macos`, which clang
// accepts as a triple it has never heard of and then treats as bare-metal
// aarch64 — the module and its importers then agree with each other and with
// nothing else.
TEST(Triple, LlvmTripleMacosRenamesArchAndCarriesVersion) {
    auto t = parse("aarch64-macos");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->llvm_triple("14.0"), "arm64-apple-macos14.0");
    EXPECT_EQ(t->llvm_triple("15.2"), "arm64-apple-macos15.2");
}

TEST(Triple, LlvmTripleMacosX86KeepsArchName) {
    auto t = parse("x86_64-macos");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->llvm_triple("14.0"), "x86_64-apple-macos14.0");
}

// A freestanding triple is already LLVM's own form, so it is returned as
// written rather than expanded into four fields.
TEST(Triple, LlvmTripleFreestandingIsUnchanged) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->llvm_triple(""), "riscv64-none-elf");
}

// ── The object format each target has, asked of the triple rather than of the
//    machine running the build.

// The artefact-format decision used to test the triple for the substrings
// `apple` and `darwin`. Those are LLVM's words; mcpp's canonical form is
// `aarch64-macos`, which contains neither — so the test fell through to a
// question about the HOST, and produced opposite errors on opposite hosts: an
// ELF contract for a Mach-O when built on Linux, and a Mach-O contract for an
// ELF when built on macOS.
TEST(Triple, OsFieldIdentifiesMacosWithoutTheWordApple) {
    auto t = parse("aarch64-macos");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->os, "macos");
    EXPECT_FALSE(t->is_pe());
    EXPECT_FALSE(t->is_freestanding());
    EXPECT_EQ(t->str().find("apple"), std::string::npos);
    EXPECT_EQ(t->str().find("darwin"), std::string::npos);
}

TEST(Triple, OsFieldIdentifiesPe) {
    auto t = parse("x86_64-windows-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->is_pe());
    EXPECT_EQ(t->os, "windows");
}

TEST(Triple, OsFieldIdentifiesFreestanding) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->is_freestanding());
    EXPECT_EQ(t->os, "none");
    EXPECT_FALSE(t->is_pe());
}

// ── The env segment may be declined on every platform ────────────────────────
//
// `x86_64-linux` parsed and `x86_64-windows` did not. The rule "a target
// triple states a REQUEST, and a request must be able to say nothing" therefore
// held on two platforms out of four, and the two where it did not were exactly
// the ones whose segment names something other than a C library — so a user was
// required to type a word that described nothing they had chosen.
//
// The identity stays total: `x86_64-windows` IS `x86_64-windows-gnu`, one output
// directory and one cache key. What differs is `envExplicit`, which records that
// nothing was asked for.
TEST(Triple, TheEnvSegmentMayBeDeclinedOnWindows) {
    auto t = parse("x86_64-windows");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "x86_64-windows-gnu");
    EXPECT_EQ(t->env, "gnu");
    EXPECT_FALSE(t->envExplicit);
}

TEST(Triple, TheEnvSegmentMayBeDeclinedOnBareMetal) {
    auto t = parse("riscv64-none");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "riscv64-none-elf");
    EXPECT_TRUE(t->is_freestanding());
    EXPECT_FALSE(t->envExplicit);
}

// Writing it out is still a request, and still recorded as one.
TEST(Triple, WritingTheSegmentOutIsStillARequest) {
    auto g = parse("x86_64-windows-gnu");
    ASSERT_TRUE(g.has_value());
    EXPECT_TRUE(g->envExplicit);

    auto m = parse("x86_64-windows-msvc");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->env, "msvc");
    EXPECT_TRUE(m->envExplicit);
}

// THE FILL IS `gnu` AND NOT THE HOST'S OWN ENV.
//
// `host_triple()` answers `msvc` on a Windows machine. Filling from it would
// give one command a different identity — a different output directory and
// cache key — on each host, and a target's identity may not depend on where it
// was built. The two spellings must therefore agree on every machine.
TEST(Triple, TheFillDoesNotDependOnTheHost) {
    EXPECT_EQ(parse("x86_64-windows")->str(), parse("x86_64-windows-gnu")->str());
    EXPECT_EQ(parse("riscv64-none")->str(),   parse("riscv64-none-elf")->str());
}

// macOS declines the segment by having none at all, which was already true and
// is asserted here so the four platforms are covered in one place.
TEST(Triple, MacosCarriesNoSegmentToDecline) {
    auto t = parse("aarch64-macos");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->env.empty());
    EXPECT_FALSE(t->envExplicit);
}

// NO TWO ROWS MAY SHARE A CANONICAL NAME, AND THIS WAS NOT A HYPOTHETICAL.
//
// Adding `x86_64-windows-musl` and later correcting its `pin` column produced
// TWO rows with that name — the edit inserted a corrected row without removing
// the original. `find_known_target` returns the first match, so every behaviour
// was correct and nothing failed; what the table carried was a second row of
// dead data whose columns disagreed with the live one.
//
// Caught by reading the diff, which is the wrong mechanism: a duplicate is a
// property of the table and a machine can see it. The cost of the check is four
// lines.
TEST(Triple, TheTargetTableHasNoDuplicateNames) {
    std::vector<std::string_view> seen;
    for (const auto& info : known_targets()) {
        for (auto s : seen)
            EXPECT_NE(s, info.canonical)
                << "duplicate row in kKnownTargets: " << info.canonical;
        seen.push_back(info.canonical);
    }
}

// And every row must parse to itself: a canonical name that does not survive a
// round trip through `parse`/`str` is a row no `--target` can reach.
TEST(Triple, EveryTableRowIsItsOwnCanonicalForm) {
    for (const auto& info : known_targets()) {
        auto t = parse(info.canonical);
        ASSERT_TRUE(t.has_value()) << info.canonical;
        EXPECT_EQ(t->str(), info.canonical)
            << info.canonical << " does not round-trip";
    }
}
