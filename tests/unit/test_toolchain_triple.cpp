#include <gtest/gtest.h>

import std;
import mcpp.toolchain.triple;

using namespace mcpp::toolchain::triple;

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
// ⚠️ The absent/empty distinction is the whole point of these three tests. A
// plain `std::string` would make "the project said nothing" and "the project
// asked for no C library" the same value, and a kernel project would silently
// get picolibc back.

TEST(Triple, EffectiveSysrootFallsBackToTheTargetRow) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, std::nullopt), "xim:picolibc-riscv@1.8.12");
}

TEST(Triple, EffectiveSysrootHonoursAnOverride) {
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, std::optional<std::string>{"xim:newlib-riscv@4.4"}),
              "xim:newlib-riscv@4.4");
}

TEST(Triple, EffectiveSysrootEmptyStringIsTheZeroLibcTier) {
    // Present-and-empty must NOT fall through to the target row. Measured
    // end-to-end alongside this: with `sysroot = ""`, `#include <stdio.h>`
    // stops resolving and a self-contained image links at 108 bytes.
    auto t = parse("riscv64-none-elf");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, std::optional<std::string>{""}), "");
}

TEST(Triple, EffectiveSysrootIsEmptyForHostedTargets) {
    auto t = parse("x86_64-linux-musl");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(effective_sysroot(*t, std::nullopt), "");
}
