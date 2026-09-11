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

TEST(TripleRequest, ABareLinuxTripleIsNeverCompletedToAndroid) {
    // `aarch64-linux` asks for a C LIBRARY to be filled in. `android` shares
    // the `arch-os` prefix because its kernel is Linux -- the modelling
    // decision that makes every Linux-shaped answer in the tree right about it
    // -- and it is not an alternative C library for the same platform.
    //
    // This became reachable when the Android rows stopped being `planned`:
    // `aarch64-linux` then had two supported siblings and resolved as
    // ambiguous, where it had completed to `aarch64-linux-musl` before. Both
    // outcomes of that ambiguity are wrong -- refusing a request with an
    // obvious answer, or answering it with bionic.
    auto r = triple::resolve_request(*parse("aarch64-linux"));
    EXPECT_EQ(r.triple.str(), "aarch64-linux-musl");
    EXPECT_TRUE(r.completedFromVocabulary);
    EXPECT_FALSE(r.ambiguous);
    // Not offered as a suggestion either: `siblings` is what the diagnostic
    // prints, and naming it there would suggest building for another platform.
    for (auto& s : r.siblings)
        EXPECT_EQ(s.find("android"), std::string::npos) << s;

    // x86_64 has the same shape and a different supported set, so it exercises
    // the exclusion independently rather than re-testing one row.
    auto x = triple::resolve_request(*parse("x86_64-linux"));
    for (auto& s : x.siblings)
        EXPECT_EQ(s.find("android"), std::string::npos) << s;

    // AND THE WRITTEN SPELLING IS STILL HONOURED. The exclusion is about
    // filling a gap, not about refusing a request -- an explicit env returns
    // before the candidate loop runs.
    auto explicitAndroid = triple::resolve_request(*parse("aarch64-linux-android"));
    EXPECT_EQ(explicitAndroid.triple.str(), "aarch64-linux-android");
    EXPECT_FALSE(explicitAndroid.completedFromVocabulary);
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
    auto [name, tier] = std::pair{"x86_64-linux-musl", "verified"};
    auto t = parse(name);
    ASSERT_TRUE(t.has_value());
    auto* info = find_known_target(*t);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->tier, tier);
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

// ── The object-format axis, and the three platforms it exists for ───────────
//
// The binary format used to be re-derived from `os` at every site that needed
// it, which is affordable only while the answer has two values. These hold the
// single derivation, because a site that misses a third value does not fail --
// it silently answers ELF, which is what every `else` branch in the tree
// assumes.

TEST(Triple, TheObjectFormatIsOneAnswerAndNotADerivation) {
    EXPECT_EQ(parse("x86_64-linux-gnu")->object_format(),      ObjectFormat::Elf);
    EXPECT_EQ(parse("x86_64-linux-musl")->object_format(),     ObjectFormat::Elf);
    EXPECT_EQ(parse("aarch64-linux-android")->object_format(), ObjectFormat::Elf);
    EXPECT_EQ(parse("riscv64-none-elf")->object_format(),      ObjectFormat::Elf);
    EXPECT_EQ(parse("x86_64-windows-gnu")->object_format(),    ObjectFormat::Pe);
    EXPECT_EQ(parse("x86_64-windows-msvc")->object_format(),   ObjectFormat::Pe);
    EXPECT_EQ(parse("aarch64-macos")->object_format(),         ObjectFormat::MachO);
    EXPECT_EQ(parse("aarch64-ios")->object_format(),           ObjectFormat::MachO);
    EXPECT_EQ(parse("wasm32-emscripten")->object_format(),     ObjectFormat::Wasm);
}

TEST(Triple, TheFormatQuestionIsNotTheOperatingSystemQuestion) {
    // The two axes were conflated before `ObjectFormat` existed, and merging
    // them is the mistake it replaces: a bare-metal image is ELF with no OS,
    // and a wasm module has an OS-like layer and is not ELF.
    auto bare = parse("riscv64-none-elf");
    EXPECT_TRUE(bare->is_freestanding());
    EXPECT_EQ(bare->object_format(), ObjectFormat::Elf);

    auto web = parse("wasm32-emscripten");
    EXPECT_FALSE(web->is_freestanding());
    EXPECT_TRUE(web->is_wasm());
}

TEST(Triple, IsPeAndIsMachOReadTheSingleAnswer) {
    EXPECT_TRUE (parse("x86_64-windows-gnu")->is_pe());
    EXPECT_FALSE(parse("x86_64-windows-gnu")->is_mach_o());
    EXPECT_TRUE (parse("aarch64-ios")->is_mach_o());
    EXPECT_FALSE(parse("aarch64-ios")->is_pe());
    // The row that used to answer this by `os == "windows"` and would have
    // answered ELF for wasm.
    EXPECT_FALSE(parse("wasm32-emscripten")->is_pe());
    EXPECT_FALSE(parse("wasm32-emscripten")->is_mach_o());
}

TEST(Triple, AndroidIsAnEnvOnALinuxOs) {
    auto t = parse("aarch64-linux-android");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "aarch64");
    // THE PLACEMENT IS THE MODELLING DECISION. The kernel is Linux, so ELF,
    // the `unix` family and `nasm -f elf64` are all already right; an
    // `os = "android"` would have made every one of them wrong by default.
    EXPECT_EQ(t->os,  "linux");
    EXPECT_EQ(t->env, "android");
    EXPECT_TRUE(t->is_android());
    EXPECT_EQ(t->family(), "unix");
    EXPECT_EQ(t->str(), "aarch64-linux-android");
    EXPECT_EQ(t->llvm_triple(), "aarch64-unknown-linux-android");

    // `androideabi` is the 32-bit ARM spelling of the same env: the EABI half
    // is the calling convention, which the arch segment already carries.
    auto eabi = parse("armv7a-linux-androideabi");
    ASSERT_TRUE(eabi.has_value());
    EXPECT_EQ(eabi->env, "android");

    // The env fill must not reach an Android request. `x86_64-linux` is still
    // `gnu`; `x86_64-linux-android` is not.
    EXPECT_EQ(parse("x86_64-linux")->env, "gnu");
    EXPECT_EQ(parse("x86_64-linux-android")->env, "android");
}

TEST(Triple, IosIsAppleWithoutBeingMacos) {
    auto t = parse("aarch64-ios");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->os, "ios");
    EXPECT_TRUE(t->env.empty());
    EXPECT_EQ(t->family(), "unix");
    EXPECT_EQ(t->str(), "aarch64-ios");
    // Apple's own spelling of the architecture, as the macOS branch already
    // produces. No deployment target is baked in: that flag belongs to the
    // layer that owns the SDK, and a default here would be a second answer.
    EXPECT_EQ(t->llvm_triple(), "arm64-apple-ios");

    // A SITE THAT MEANS "APPLE" AND ASKS "macOS" GETS iOS WRONG IN THE
    // DIRECTION THAT STILL LINKS, which is why the predicate exists.
    EXPECT_TRUE(parse("aarch64-macos")->is_apple());
    EXPECT_TRUE(parse("aarch64-ios")->is_apple());
    EXPECT_FALSE(parse("aarch64-linux-musl")->is_apple());

    // An effective triple carries the deployment target on this segment.
    auto eff = parse("arm64-apple-ios17.0");
    ASSERT_TRUE(eff.has_value());
    EXPECT_EQ(eff->os, "ios");
}

TEST(Triple, EmscriptenIsAnOsSegmentAndNotAnEnv) {
    auto t = parse("wasm32-emscripten");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "wasm32");
    // It names the platform layer a module is compiled against -- the POSIX
    // emulation, the filesystem shim, the main loop -- which is the kind of
    // thing `linux` names and not the kind of thing `musl` names.
    EXPECT_EQ(t->os, "emscripten");
    EXPECT_TRUE(t->env.empty());
    EXPECT_EQ(t->str(), "wasm32-emscripten");
    EXPECT_EQ(t->llvm_triple(), "wasm32-unknown-emscripten");
    // `unix` on the test the predicate actually applies -- what API surface a
    // source may assume -- rather than on a claim about wasm.
    EXPECT_EQ(t->family(), "unix");
    // NASM is x86-family by construction and must decline rather than choose.
    EXPECT_FALSE(t->nasm_format().has_value());
}

TEST(Triple, EachRowsTierMatchesTheEvidenceThatExistsForIt) {
    // WHAT A TIER ASSERTS, AND THE THREE ANSWERS ARE DIFFERENT CLAIMS.
    //
    //   verified   an artefact was built AND RUN
    //   preview    an artefact was built; nothing has executed it
    //   planned    the vocabulary exists and nothing is wired
    //
    // A tier that moved on expectation would be the one thing this column
    // cannot be, so each row below names the measurement behind it.

    {
        // Measured 2026-09-11 on linux-x86_64 with xim:emsdk 6.0.9:
        // `mcpp run --target wasm32-emscripten` on a source that imports std
        // printed `1-2-3`. Built and run, so `verified`.
        auto [name, tier] = std::pair{"wasm32-emscripten", "verified"};
        auto t = parse(name);
        ASSERT_TRUE(t.has_value());
        auto* info = find_known_target(*t);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->tier, tier);
        // A ROW THAT IS WIRED NAMES ITS PAYLOAD. Without the pin the row's
        // tier was reachable only through an explicit
        // `[target.wasm32-emscripten] toolchain = "..."` override, which is
        // the escape hatch and not the support claim.
        EXPECT_EQ(info->pin, "emsdk@6.0.9");
        // No `sysroot` column, and that is a statement: the SDK ships one, so
        // there is no separate C library for the row to name.
        EXPECT_TRUE(info->sysroot.empty());
    }

    // BOTH ANDROID ROWS ARE NOW WIRED, AND `preview` IS THE HONEST TIER.
    //
    // Measured 2026-09-11 on linux-x86_64 with xim:android-ndk
    // 30.0.16248370, from a source that imports std and no project
    // vocabulary beyond `--target`:
    //
    //   aarch64-linux-android -> ELF 64-bit LSB pie, ARM aarch64,
    //                            interpreter /system/bin/linker64
    //   x86_64-linux-android  -> ELF 64-bit LSB pie, x86-64, same interpreter
    //
    // Neither has been EXECUTED, which is exactly the difference between this
    // tier and the wasm row's: running one needs a device or an emulator.
    //
    // ONE PIN SERVES BOTH ROWS, which is the property the whole Android path
    // rests on: the NDK names no arch, `--target` does, and that is why the
    // std module's own precompile had to be told the target as well.
    // ONE PIN, TWO ROWS, BOTH VERIFIED -- BY DIFFERENT VEHICLES.
    // `x86_64-linux-android` ran on the platform's own emulator (API 24
    // x86_64 image, KVM): `adb push` then `adb shell ./andtest` printed
    // `1-2-3`, exit 0. The device row has no such path from an x86_64 host --
    // Google's emulator refuses a foreign guest outright -- and that bounded
    // the EMULATOR, not the row: `7zz x <system.img>` with the packaged 7zip
    // extracts bionic, and `qemu-aarch64-static -L <root> <artefact>` printed
    // `1-2-3`, exit 0.
    //
    // AND THIS ASSERTION WAS THE FIFTH COPY OF THE TIER. The four documents
    // are compared to this table by `.github/tools/check_target_tiers.py`;
    // this line is in neither set, so it went on asserting `preview` after
    // the row and all four documents had moved. A structural check over the
    // documents cannot see a literal in a test.
    for (auto [name, tier] : {std::pair{"aarch64-linux-android", "verified"},
                              std::pair{"x86_64-linux-android",  "verified"}}) {
        auto t = parse(name);
        ASSERT_TRUE(t.has_value()) << name;
        EXPECT_EQ(t->str(), name);
        auto* info = find_known_target(*t);
        ASSERT_NE(info, nullptr) << name;
        EXPECT_EQ(info->tier, tier) << name;
        EXPECT_EQ(info->pin, "android-ndk@30.0.16248370") << name;
        // Same statement the wasm row makes: the SDK ships the sysroot.
        EXPECT_TRUE(info->sysroot.empty()) << name;
    }

    // THE THREE APPLE ROWS, AND WHAT THE SDK'S LICENCE DOES AND DOES NOT
    // BOUND.
    //
    // The NDK is Apache-2.0 and Emscripten is MIT; the iPhoneOS and
    // iPhoneSimulator SDKs ship inside Xcode and are neither. That bounds the
    // SYSROOT -- which is why these rows carry no `sysroot` entry, since that
    // column names a package and a located directory is not one -- and it
    // does NOT bound the compiler: `xim:llvm` emits arm64 Mach-O for an iOS
    // deployment target, so the rows pin it exactly as `x86_64-windows-musl`
    // does.
    //
    // A CONVENTION PIN. It answers "what does `--target aarch64-ios` resolve
    // when the project says nothing", and remains overridable -- asserted
    // below in ExactlyTheseRowsHaveACapabilityPin, whose expected set does
    // NOT contain these three.
    for (auto [name, tier] : {std::pair{"aarch64-ios",     "planned"},
                              std::pair{"aarch64-ios-sim", "planned"},
                              std::pair{"x86_64-ios-sim",  "planned"}}) {
        auto t = parse(name);
        ASSERT_TRUE(t.has_value()) << name;
        EXPECT_EQ(t->str(), name);
        auto* info = find_known_target(*t);
        ASSERT_NE(info, nullptr) << name;
        EXPECT_EQ(info->tier, tier) << name;
        EXPECT_EQ(info->pin, "llvm@22.1.8") << name;
        EXPECT_TRUE(info->sysroot.empty()) << name;
        EXPECT_FALSE(t->pin_is_capability()) << name;
    }
}

// A CAPABILITY PIN CANNOT BE OVERRIDDEN; A CONVENTION PIN CAN.
//
// Asserted exhaustively over the table rather than on examples, because the
// failure this guards against is a row JOINING the set without its refusal
// sentence being written. That has now happened twice -- wasm, then Android --
// and each time the refusal explained a different row: "No gcc payload emits a
// PE with a musl C library", printed for a wasm target and then for an Android
// one, because a fourth case fell into an `else` written as the third's answer.
TEST(Triple, ExactlyTheseRowsHaveACapabilityPin) {
    std::set<std::string> capability;
    for (auto& row : known_targets()) {
        auto t = parse(row.canonical);
        ASSERT_TRUE(t.has_value()) << row.canonical;
        if (t->pin_is_capability()) capability.insert(std::string(row.canonical));
    }
    // Every freestanding row, the PE+musl row, wasm, and both Android rows.
    std::set<std::string> expected{
        "aarch64-none-elf", "armv7a-none-eabi", "armv7a-none-eabihf",
        "riscv32-none-elf", "riscv64-none-elf", "thumbv6m-none-eabi",
        "thumbv7em-none-eabi", "thumbv7em-none-eabihf", "thumbv7m-none-eabi",
        "thumbv8m.base-none-eabi", "thumbv8m.main-none-eabi",
        "thumbv8m.main-none-eabihf", "x86_64-none-elf",
        "x86_64-windows-musl",
        "wasm32-emscripten",
        "aarch64-linux-android", "x86_64-linux-android",
    };
    EXPECT_EQ(capability, expected);

    // ANDROID IS THE ONE THAT DOES NOT FIT THE OTHERS' REASON, and that is why
    // it was left out. The other entries are refused because the toolchain
    // cannot emit the FORMAT; a stock clang emits aarch64 ELF perfectly well.
    // What it cannot supply is bionic -- headers, per-API-level stubs, loader
    // path -- and no package adds those to another compiler.
    auto android = parse("aarch64-linux-android");
    ASSERT_TRUE(android.has_value());
    EXPECT_TRUE(android->pin_is_capability());
    EXPECT_FALSE(android->is_freestanding());
    EXPECT_FALSE(android->is_wasm());
    EXPECT_FALSE(android->is_pe() && android->is_musl());

    // And a hosted row's pin stays a convention: an author who supplies the
    // system may name any compiler.
    auto musl = parse("x86_64-linux-musl");
    ASSERT_TRUE(musl.has_value());
    EXPECT_FALSE(musl->pin_is_capability());
}

// THE SIMULATOR IS A TARGET, NOT A RUNNER, AND IT NEEDS A SPELLING.
TEST(Triple, TheSimulatorRowsCarryEnvSimAndTheirOwnEffectiveTriple) {
    // mcpp's three-field form. Rust spells this `aarch64-apple-ios-sim`; the
    // difference is the vendor segment this table elides everywhere.
    auto sim = parse("aarch64-ios-sim");
    ASSERT_TRUE(sim.has_value());
    EXPECT_EQ(sim->arch, "aarch64");
    EXPECT_EQ(sim->os,   "ios");
    EXPECT_EQ(sim->env,  "sim");
    EXPECT_EQ(sim->str(), "aarch64-ios-sim");
    EXPECT_TRUE(sim->is_apple());
    EXPECT_EQ(sim->object_format(), ObjectFormat::MachO);
    EXPECT_EQ(sim->family(), "unix");

    // APPLE'S OWN SPELLING PARSES TO THE SAME ROW. An effective triple clang
    // prints carries `-simulator`, and a reader who pastes one must not be
    // told mcpp has never heard of it.
    auto apple = parse("arm64-apple-ios-simulator");
    ASSERT_TRUE(apple.has_value());
    EXPECT_EQ(apple->str(), "aarch64-ios-sim");

    // TWO ROWS THAT MUST NOT SHARE AN IDENTITY. The device and the simulator
    // have different SDKs and different objects; one identity would put two
    // targets in one build directory.
    auto device = parse("aarch64-ios");
    ASSERT_TRUE(device.has_value());
    EXPECT_NE(device->str(), sim->str());
    EXPECT_TRUE(device->env.empty());

    // The effective triple differs too, and that is what the SDK selection
    // downstream keys off.
    EXPECT_EQ(sim->llvm_triple({}),    "arm64-apple-ios-simulator");
    EXPECT_EQ(device->llvm_triple({}), "arm64-apple-ios");

    // Both host arches, because the simulator runs the HOST's architecture: a
    // single row would describe a simulator half the machines cannot run.
    auto x86sim = parse("x86_64-ios-sim");
    ASSERT_TRUE(x86sim.has_value());
    EXPECT_EQ(x86sim->str(), "x86_64-ios-sim");
    EXPECT_EQ(x86sim->llvm_triple({}), "x86_64-apple-ios-simulator");
}

// THE API LEVEL IS FUSED ONTO THE ENV SEGMENT, AND IT IS NOT OPTIONAL.
TEST(Triple, AndroidFusesTheApiLevelAndBionicRequiresOne) {
    auto t = parse("aarch64-linux-android");
    ASSERT_TRUE(t.has_value());
    // Canonical identity carries no level: one row serves every level, which
    // is why the level is a project decision in `[target.<triple>]` and not a
    // multiplication of the table.
    EXPECT_EQ(t->str(), "aarch64-linux-android");
    // The effective triple is where it lands. Measured:
    // `clang -target aarch64-linux-android21 -print-effective-triple`
    // answers `aarch64-unknown-linux-android21`.
    EXPECT_EQ(t->llvm_triple("21"), "aarch64-unknown-linux-android21");
    EXPECT_EQ(t->llvm_triple("24"), "aarch64-unknown-linux-android24");
    // WITH NO LEVEL THE FORM IS STILL PRODUCED, and that is deliberate: this
    // function composes, it does not decide. The refusal lives where the level
    // is chosen, because bionic's own <sys/cdefs.h> stops the build --
    // "Unversioned target triples are not supported!" -- and the default comes
    // from the NDK's `meta/platforms.json` rather than from here.
    EXPECT_EQ(t->llvm_triple({}), "aarch64-unknown-linux-android");
    // An effective triple with a level parses back to the canonical row.
    auto back = parse("aarch64-unknown-linux-android21");
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->str(), "aarch64-linux-android");
}

// DOES THIS TARGET'S TOOLCHAIN ARRIVE WITH ITS OWN COMPLETE SYSTEM?
//
// The predicate exists because mcpp reconstructs a target's system by hand --
// libc++'s headers, glibc's, the Linux UAPI headers, the C-runtime prefix, the
// loader -- and for a target whose SDK ships a sysroot every one of those is an
// answer competing with one the driver already has. Three independent sites
// read it, and each was found by the previous one's failure:
//
//   host_compile_tokens        this host's stdint.h reached a wasm compile
//   resolve_link_model         --dynamic-linker=...ld-linux-x86-64.so.2 reached wasm-ld
//   discover_link_runtime_dirs the COMPILER's libatomic reached the ARTIFACT's link line
//
// Asserted as an exhaustive statement over the table rather than on examples,
// so a new row cannot join the set by accident or be left out of it.
TEST(Triple, OnlyTheSdkTargetsShipTheirOwnSysroot) {
    std::set<std::string> shipsOwn;
    for (auto& row : known_targets()) {
        auto t = parse(row.canonical);
        ASSERT_TRUE(t.has_value()) << row.canonical;
        if (t->has_own_sysroot()) shipsOwn.insert(std::string(row.canonical));
    }
    EXPECT_EQ(shipsOwn, (std::set<std::string>{
        "aarch64-linux-android", "x86_64-linux-android", "wasm32-emscripten"}));

    // `aarch64-ios` is NOT in the set, and that is the interesting exclusion.
    // The iPhoneOS SDK does ship a sysroot -- but mcpp reaches it with
    // `-isysroot`, which this predicate is not about: the question here is
    // whether the DRIVER resolves the system without being told, and an
    // ordinary clang pointed at an SDK does not.
    auto ios = parse("aarch64-ios");
    ASSERT_TRUE(ios.has_value());
    EXPECT_FALSE(ios->has_own_sysroot());
}

// A CAPABILITY PIN CANNOT BE OVERRIDDEN, BECAUSE NOTHING ELSE CAN EMIT THE
// TARGET. A convention pin is a preference; this is a fact about the world.
// THE EFFECTIVE TRIPLE CARRIES THE PROJECT'S MINIMUM PLATFORM VERSION, AND THE
// CANONICAL ONE NEVER DOES.
//
// Two platforms fuse it and each names it in its own words: macOS's deployment
// target, Android's minimum API level. Measured on a real clang --
// `-target aarch64-linux-android21 -print-effective-triple` answers
// `aarch64-unknown-linux-android21` -- so the level belongs on the ENV segment
// of the effective triple.
//
// Asserted as the PAIR, because the whole design is that the two differ: if
// `str()` ever carried the version, the output directory and `cfg()` would
// multiply per level and the table would need a row for each.
TEST(Triple, TheMinimumPlatformVersionReachesTheEffectiveTripleAndNotTheCanonicalOne) {
    auto droid = parse("aarch64-linux-android");
    ASSERT_TRUE(droid.has_value());
    EXPECT_EQ(droid->str(), "aarch64-linux-android");
    EXPECT_EQ(droid->llvm_triple("24"), "aarch64-unknown-linux-android24");
    EXPECT_EQ(droid->llvm_triple("21"), "aarch64-unknown-linux-android21");
    // Unset is legal and means the NDK's own default -- what clang normalises
    // when no level is given.
    EXPECT_EQ(droid->llvm_triple(""),   "aarch64-unknown-linux-android");
    // And the canonical form is unmoved by any of it.
    EXPECT_EQ(droid->str(), "aarch64-linux-android");

    // macOS, the platform this parameter already served, is unchanged.
    auto mac = parse("aarch64-macos");
    ASSERT_TRUE(mac.has_value());
    EXPECT_EQ(mac->llvm_triple("15.2"), "arm64-apple-macos15.2");
    EXPECT_EQ(mac->str(), "aarch64-macos");

    // AND NO OTHER ROW TAKES IT. One parameter serves both platforms, so the
    // risk is a caller handing one platform's answer to another's row -- an
    // ordinary Linux target must ignore it rather than fuse it.
    auto lin = parse("x86_64-linux-gnu");
    ASSERT_TRUE(lin.has_value());
    EXPECT_EQ(lin->llvm_triple("24"), "x86_64-unknown-linux-gnu");
    auto musl = parse("aarch64-linux-musl");
    ASSERT_TRUE(musl.has_value());
    EXPECT_EQ(musl->llvm_triple("24"), "aarch64-unknown-linux-musl");
}

// THE FOUR-FIELD SPELLING IS WHAT EVERY OTHER TOOLCHAIN PRINTS, so refusing it
// is a cost with no design benefit. `em++ -v` passes
// `-target wasm32-unknown-emscripten`, rustc's table lists that spelling, and a
// user copying either into a manifest should be understood.
//
// mcpp's canonical form elides the vendor -- `unknown`, `pc` and `w64` carry no
// information for any row in the table -- so this is a normalisation and not a
// second vocabulary: `str()` returns the three-field form either way, which is
// what keeps the output directory, `cfg()` and the ABI tag single-valued.
TEST(Triple, TheFourFieldSpellingParsesToTheSameCanonicalTriple) {
    struct Case { const char* spelled; const char* canonical; };
    for (auto [spelled, canonical] : {
             Case{"wasm32-unknown-emscripten", "wasm32-emscripten"},
             Case{"aarch64-apple-ios",         "aarch64-ios"},
             Case{"aarch64-unknown-linux-android", "aarch64-linux-android"},
             Case{"x86_64-unknown-linux-gnu",  "x86_64-linux-gnu"},
             Case{"x86_64-pc-windows-msvc",    "x86_64-windows-msvc"},
             Case{"aarch64-unknown-linux-musl","aarch64-linux-musl"},
         }) {
        auto t = parse(spelled);
        ASSERT_TRUE(t.has_value()) << spelled;
        EXPECT_EQ(t->str(), canonical) << spelled;
        // And the three-field form still parses to itself, so accepting the
        // longer spelling did not make the canonical one a second dialect.
        auto c = parse(canonical);
        ASSERT_TRUE(c.has_value()) << canonical;
        EXPECT_EQ(c->str(), canonical) << canonical;
    }
}

TEST(Triple, WasmJoinsTheCapabilityPinsBecauseNothingElseEmitsIt) {
    auto wasm = parse("wasm32-emscripten");
    ASSERT_TRUE(wasm.has_value());
    EXPECT_TRUE(wasm->pin_is_capability())
        << "a declared gcc@16.1.0 would otherwise override emsdk@6.0.9 and "
           "fail inside a compiler that cannot emit WebAssembly";

    // The two that were there before, unchanged.
    EXPECT_TRUE(parse("riscv64-none-elf")->pin_is_capability());
    EXPECT_TRUE(parse("x86_64-windows-musl")->pin_is_capability());
    // And an ordinary hosted row is still a convention: a project may name
    // whichever compiler it likes for its own Linux.
    EXPECT_FALSE(parse("x86_64-linux-musl")->pin_is_capability());
    // ANDROID IS ONE NOW, AND THIS ASSERTION PREDICTED ITS OWN EXPIRY. It
    // read EXPECT_FALSE, with the note "the row carries no pin yet [...] it
    // moves when the row does" -- the row moved, so it did.
    //
    // The reason is not the other three's. They are refused because the
    // toolchain cannot emit the FORMAT; a stock clang emits aarch64 ELF
    // perfectly well. What it cannot supply is bionic, which lives inside the
    // NDK and is not packaged onto another compiler.
    EXPECT_TRUE(parse("aarch64-linux-android")->pin_is_capability());
    EXPECT_TRUE(parse("x86_64-linux-android")->pin_is_capability());
    // The iOS rows are NOT capability pins, and the distinction is worth an
    // assertion: they carry no pin at all, so there is nothing to override and
    // nothing to refuse. Their tier is what stops a build, not their pin.
    EXPECT_FALSE(parse("aarch64-ios")->pin_is_capability());
    EXPECT_FALSE(parse("aarch64-ios-sim")->pin_is_capability());
}

TEST(Triple, TheCanonicalSpellingIsNotSEARCHABLEForAVENDORNAME) {
    // WHY A SUBSTRING TEST ON THE CANONICAL TRIPLE IS WRONG, stated as a fact
    // about the vocabulary rather than as a comment somewhere else.
    //
    // Two sites derived the object format by looking for "apple" / "darwin" /
    // "windows" / "mingw" in `plan.toolchain.targetTriple`. That string is
    // mcpp's CANONICAL spelling, and `aarch64-macos` contains none of those
    // words -- so an explicit `--target aarch64-macos`, a `verified` row, was
    // recorded and linked as ELF. A NATIVE macOS build was right by a
    // different branch (an empty triple), which is why the two paths through
    // one function disagreed and only the exercised one was correct.
    //
    // The words appear in the LLVM spelling, which is a different string and
    // the reason the mistake is easy to make:
    //
    //     aarch64-macos  ->  arm64-apple-macos14.0
    //     ^ the identity     ^ what clang is given
    for (auto name : {"aarch64-macos", "x86_64-macos", "aarch64-ios"}) {
        auto t = parse(name);
        ASSERT_TRUE(t.has_value()) << name;
        const std::string canonical = t->str();
        EXPECT_EQ(canonical.find("apple"),  std::string::npos) << canonical;
        EXPECT_EQ(canonical.find("darwin"), std::string::npos) << canonical;
        // And the format is right anyway, because it is asked of the fields.
        EXPECT_EQ(t->object_format(), ObjectFormat::MachO) << canonical;
        // The LLVM spelling is where the vendor name lives.
        EXPECT_NE(t->llvm_triple().find("apple"), std::string::npos)
            << t->llvm_triple();
    }
    // The one family the substring test got right, and only by luck: the
    // canonical spelling happens to carry the OS name.
    EXPECT_NE(std::string(parse("x86_64-windows-gnu")->str()).find("windows"),
              std::string::npos);
}

TEST(Triple, EveryKnownRowHasAnObjectFormatAndNoneFallsThrough) {
    // THE DENOMINATOR IS THE TABLE. A row added without an answer here would
    // otherwise be covered by a test whose name says every row is -- and the
    // answer it would get is ELF, because ELF is what every `else` branch in
    // the tree assumes.
    std::size_t elf = 0, macho = 0, pe = 0, wasm = 0;
    for (auto const& row : known_targets()) {
        auto t = parse(row.canonical);
        ASSERT_TRUE(t.has_value()) << row.canonical;
        EXPECT_EQ(t->str(), row.canonical) << "a row that is not its own canonical form";
        switch (t->object_format()) {
            case ObjectFormat::Elf:   ++elf;   break;
            case ObjectFormat::MachO: ++macho; break;
            case ObjectFormat::Pe:    ++pe;    break;
            case ObjectFormat::Wasm:  ++wasm;  break;
        }
    }
    // Each format has at least one row, which is what makes the axis worth
    // having: a fourth value with no row would be an enum nothing produces.
    EXPECT_GT(elf, 0u);
    EXPECT_GT(macho, 0u);
    EXPECT_GT(pe, 0u);
    EXPECT_EQ(wasm, 1u) << "wasm32-emscripten is the only wasm row today";
    EXPECT_EQ(elf + macho + pe + wasm, known_targets().size());
}
