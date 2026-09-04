#include <gtest/gtest.h>

import std;
import mcpp.toolchain.devicehost;

using mcpp::toolchain::parse_host_config;
using mcpp::toolchain::host_compiler_accepted;

namespace {
// The two guards as CUDA 12.0 writes them, reduced to what the parse reads.
constexpr std::string_view kCuda120 = R"(
#if __GNUC__ > 12
#error -- unsupported GNU version! gcc versions later than 12 are not supported! The nvcc flag '-allow-unsupported-compiler' can be used to override this version check
#endif /* __GNUC__ > 12 */
#if defined(__clang__)
#error -- unsupported clang version! clang version must be less than 15 and greater than 3.2 .
#endif
)";
} // namespace

TEST(DeviceHost, ReadsBothGuardsOutOfTheVendorHeader) {
    auto b = parse_host_config(kCuda120);
    EXPECT_TRUE(b.known());
    EXPECT_EQ(b.gccMax,   12);
    EXPECT_EQ(b.clangMax, 14);   // "less than 15" is an exclusive bound
}

TEST(DeviceHost, AcceptsWithinTheBoundAndRefusesAbove) {
    auto b = parse_host_config(kCuda120);
    EXPECT_TRUE (host_compiler_accepted(b, "gcc",   12));
    EXPECT_FALSE(host_compiler_accepted(b, "gcc",   13));
    EXPECT_FALSE(host_compiler_accepted(b, "gcc",   16));   // mcpp's own payload
    EXPECT_TRUE (host_compiler_accepted(b, "clang", 14));
    EXPECT_FALSE(host_compiler_accepted(b, "clang", 18));
    EXPECT_TRUE (host_compiler_accepted(b, "llvm",  14));   // mcpp's family name
}

TEST(DeviceHost, AnUnreadableHeaderMakesNoClaim) {
    // A refusal invented from a file the parse did not understand would be
    // worse than the failure it prevents: the user cannot act on it.
    auto b = parse_host_config("nothing to see here");
    EXPECT_FALSE(b.known());
    EXPECT_TRUE(host_compiler_accepted(b, "gcc",   99));
    EXPECT_TRUE(host_compiler_accepted(b, "clang", 99));
}

TEST(DeviceHost, SilenceAboutOneFamilyIsNotARefusalOfIt) {
    auto b = parse_host_config("#if __GNUC__ > 11\n#error nope\n#endif\n");
    EXPECT_EQ(b.gccMax, 11);
    EXPECT_EQ(b.clangMax, 0);
    EXPECT_FALSE(host_compiler_accepted(b, "gcc",   12));
    EXPECT_TRUE (host_compiler_accepted(b, "clang", 20));
}

TEST(DeviceHost, AnUnknownVersionMakesNoClaimEither) {
    auto b = parse_host_config(kCuda120);
    EXPECT_TRUE(host_compiler_accepted(b, "gcc", 0));
}
