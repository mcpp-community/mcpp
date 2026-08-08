#include <gtest/gtest.h>

import std;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;

using namespace mcpp::toolchain;

namespace {

FingerprintInputs baseline() {
    FingerprintInputs in;
    in.toolchain.compiler        = CompilerId::GCC;
    in.toolchain.version         = "16.1.0";
    in.toolchain.binaryPath     = "/usr/bin/g++";
    in.toolchain.driverIdent    = "g++ (xim-x-gcc 16.1.0) 16.1.0";
    in.toolchain.targetTriple   = "x86_64-linux-gnu";
    in.toolchain.stdlibId       = "libstdc++";
    in.toolchain.stdlibVersion  = "16.1.0";
    in.cppStandard              = "c++23";
    in.compileFlags             = "-O2";
    in.dependencyLockHash      = "deadbeefcafebabe";
    in.stdBmiHash              = "0123456789abcdef";
    return in;
}


// Two builds that differ ONLY in which libc they target must not share a
// fingerprint -- and therefore must not share `target/<fp>/`.
//
// Without this, switching subos leaves the build directory untouched: ninja
// sees the same graph, the objects inside were compiled against the other
// glibc, and the link succeeds. What comes out references symbols the
// interpreter cannot provide, and nothing along the way said anything. This
// is the prerequisite for building the same project under two subos, not a
// refinement of it.
TEST(Fingerprint, RuntimeBindingIsPartOfTheIdentity) {
    mcpp::toolchain::FingerprintInputs a;
    a.toolchain.compiler     = mcpp::toolchain::CompilerId::GCC;
    a.toolchain.version      = "16.1.0";
    a.toolchain.targetTriple = "x86_64-linux-gnu";
    a.toolchain.runtimeBinding = "glibc@2.39";

    auto b = a;
    b.toolchain.runtimeBinding = "glibc@2.44";

    EXPECT_NE(mcpp::toolchain::compute_fingerprint(a).hex,
              mcpp::toolchain::compute_fingerprint(b).hex)
        << "same toolchain, same triple, different libc — sharing target/<fp>/ "
           "would replay objects compiled against the other one";
}

// ...and an unset binding is its own identity, not a collision with any
// particular version.
TEST(Fingerprint, EmptyRuntimeBindingIsDistinct) {
    mcpp::toolchain::FingerprintInputs a;
    a.toolchain.targetTriple   = "x86_64-linux-gnu";
    a.toolchain.runtimeBinding = "glibc@2.39";
    auto b = a;
    b.toolchain.runtimeBinding.clear();
    EXPECT_NE(mcpp::toolchain::compute_fingerprint(a).hex,
              mcpp::toolchain::compute_fingerprint(b).hex);
}

} // namespace

TEST(Fingerprint, DeterministicForSameInputs) {
    auto a = compute_fingerprint(baseline());
    auto b = compute_fingerprint(baseline());
    EXPECT_EQ(a.hex, b.hex);
}

TEST(Fingerprint, ProducesSixteenHexChars) {
    auto fp = compute_fingerprint(baseline());
    EXPECT_EQ(fp.hex.size(), 16u);
    for (char c : fp.hex) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

#define EXPECT_DIFFERENT(field_mutation) do {                  \
        auto base = baseline();                                \
        auto bfp  = compute_fingerprint(base);                 \
        auto in   = baseline();                                \
        field_mutation;                                        \
        auto fp   = compute_fingerprint(in);                   \
        EXPECT_NE(bfp.hex, fp.hex)                             \
            << "Mutation '" #field_mutation "' did NOT change fingerprint"; \
    } while (0)

TEST(Fingerprint, AllTenFieldsAffectHash) {
    EXPECT_DIFFERENT(in.toolchain.compiler        = CompilerId::Clang);
    EXPECT_DIFFERENT(in.toolchain.version         = "16.0.0");
    EXPECT_DIFFERENT(in.toolchain.driverIdent    = "g++ (xim-x-gcc 15.1.0) 15.1.0");
    EXPECT_DIFFERENT(in.toolchain.targetTriple   = "aarch64-linux-gnu");
    EXPECT_DIFFERENT(in.toolchain.stdlibId       = "libc++");
    EXPECT_DIFFERENT(in.cppStandard              = "c++26");
    // c++20 gets its own target dir for the same reason c++26 does — cross-level
    // BMI reuse is rejected by the compiler itself.
    EXPECT_DIFFERENT(in.cppStandard              = "c++20");
    EXPECT_DIFFERENT(in.compileFlags             = "-O3");
    // mcpp version is hardcoded inside compute_fingerprint, can't mutate from here.
    EXPECT_DIFFERENT(in.dependencyLockHash      = "");
    EXPECT_DIFFERENT(in.stdBmiHash              = "ffffffffffffffff");
}

TEST(Fingerprint, StableAcrossBinaryPathsWhenDriverIdentMatches) {
    auto a = baseline();
    auto b = baseline();
    a.toolchain.binaryPath = "/home/speak/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++";
    b.toolchain.binaryPath = "/home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.14/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++";

    EXPECT_EQ(compute_fingerprint(a).hex, compute_fingerprint(b).hex);
}

TEST(Fingerprint, HashStringMatchesHashFile) {
    auto h1 = hash_string("hello");
    auto h2 = hash_string("hello");
    EXPECT_EQ(h1, h2);
    EXPECT_NE(hash_string("hello"), hash_string("hellp"));
}
