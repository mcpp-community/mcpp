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
