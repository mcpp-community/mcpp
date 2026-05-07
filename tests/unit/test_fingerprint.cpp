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
    in.toolchain.targetTriple   = "x86_64-linux-gnu";
    in.toolchain.stdlibId       = "libstdc++";
    in.toolchain.stdlibVersion  = "16.1.0";
    in.cppStandard              = "c++23";
    in.compileFlags             = "-O2";
    in.dependencyLockHash      = "deadbeefcafebabe";
    in.stdBmiHash              = "0123456789abcdef";
    return in;
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
    EXPECT_DIFFERENT(in.toolchain.binaryPath     = "/elsewhere/g++");
    EXPECT_DIFFERENT(in.toolchain.targetTriple   = "aarch64-linux-gnu");
    EXPECT_DIFFERENT(in.toolchain.stdlibId       = "libc++");
    EXPECT_DIFFERENT(in.cppStandard              = "c++26");
    EXPECT_DIFFERENT(in.compileFlags             = "-O3");
    // mcpp version is hardcoded inside compute_fingerprint, can't mutate from here.
    EXPECT_DIFFERENT(in.dependencyLockHash      = "");
    EXPECT_DIFFERENT(in.stdBmiHash              = "ffffffffffffffff");
}

TEST(Fingerprint, HashStringMatchesHashFile) {
    auto h1 = hash_string("hello");
    auto h2 = hash_string("hello");
    EXPECT_EQ(h1, h2);
    EXPECT_NE(hash_string("hello"), hash_string("hellp"));
}
