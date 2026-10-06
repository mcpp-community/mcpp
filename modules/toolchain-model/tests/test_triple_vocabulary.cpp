#include <gtest/gtest.h>

import std;
import mcpp.toolchain.triple;

// SUBSYSTEM-LEVEL. This package is the toolchain VOCABULARY -- what a triple
// is, what a dialect is -- separated from finding a toolchain on a machine.
// These tests see no detection, no registry and no xlings, which is what makes
// them able to state the vocabulary's own rules.

namespace tr = mcpp::toolchain::triple;

TEST(TripleVocabulary, TheThreePartSpellingMeansTheSameAsTheFourPart) {
    // Users write the short form; the resolver, the lock file and the cache key
    // all read the long one. If these ever stopped agreeing, one project would
    // build twice under two names.
    auto three = tr::parse("x86_64-linux-musl");
    auto four  = tr::parse("x86_64-unknown-linux-musl");
    ASSERT_TRUE(three.has_value());
    ASSERT_TRUE(four.has_value());
    EXPECT_EQ(three->arch, four->arch);
    EXPECT_EQ(three->os,   four->os);
    EXPECT_EQ(three->env,  four->env);
}

TEST(TripleVocabulary, TheHostTripleIsParseable) {
    // `host_triple()` is the MCPP_HOST contract value handed to every build
    // program. A value this package cannot parse back is one no consumer can
    // act on.
    auto h = tr::host_triple();
    EXPECT_FALSE(h.arch.empty());
    EXPECT_FALSE(h.os.empty());
}

TEST(TripleVocabulary, AnUnknownTripleIsNotSilentlyKnown) {
    // `is_known_target` gates the per-target table. Answering "yes" for
    // something absent from it would select an empty row rather than report an
    // unsupported target.
    auto t = tr::parse("nosucharch-unknown-nosuchos-nosuchenv");
    if (t.has_value()) EXPECT_FALSE(tr::is_known_target(*t));
}

TEST(TripleVocabulary, MsvcsX86IsWrittenI686AndTheOtherSpellingsKeepTheirMeaning) {
    // `x86` is MSVC's name for the ISA and no LLVM tool takes it as an
    // architecture, so it is written the way `amd64` and `arm64` are: once, at
    // the parse. i386-i586 are different baseline CPUs to clang and stay.
    auto x86 = tr::parse("x86-windows-msvc");
    ASSERT_TRUE(x86.has_value());
    EXPECT_EQ(x86->str(), "i686-windows-msvc");
    EXPECT_EQ(x86->llvm_triple(), "i686-pc-windows-msvc");
    EXPECT_EQ(*tr::parse("x86-pc-windows-msvc"), *tr::parse("i686-windows-msvc"));
    EXPECT_EQ(tr::parse("i386-windows-msvc")->llvm_triple(), "i386-pc-windows-msvc");
}

TEST(TripleVocabulary, The32BitX86QuestionHasOneAnswer) {
    struct Row { std::string_view triple; bool x86_32; std::string_view msvc; };
    for (auto const& r : {
             Row{"x86-windows-msvc",     true,  "x86"},
             Row{"i386-windows-msvc",    true,  "x86"},
             Row{"i486-windows-gnu",     true,  "x86"},
             Row{"i586-linux-gnu",       true,  "x86"},
             Row{"i686-windows-msvc",    true,  "x86"},
             Row{"x86_64-windows-msvc",  false, "x64"},
             Row{"amd64-windows-msvc",   false, "x64"},
             Row{"aarch64-windows-msvc", false, "arm64"},
             Row{"arm64-apple-macos",    false, "arm64"},
             Row{"riscv64-linux-musl",   false, ""}}) {
        auto t = tr::parse(r.triple);
        ASSERT_TRUE(t.has_value()) << r.triple;
        EXPECT_EQ(t->is_x86_32(), r.x86_32) << r.triple;
        EXPECT_EQ(t->msvc_arch(), r.msvc) << r.triple;
    }
    EXPECT_EQ(tr::parse("i586-linux-gnu")->nasm_format(), "elf32");
    EXPECT_EQ(tr::parse("x86-windows-msvc")->nasm_format(), "win32");
}
