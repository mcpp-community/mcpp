#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;

using namespace mcpp::toolchain;

static std::string host_musl() {
    return std::string(mcpp::platform::host_arch) + "-linux-musl";
}

// The host-native `musl-gcc` package only exists for Linux hosts; on other
// hosts the payload mapping resolves the triple-named package (a linux-musl
// target from macOS/Windows is cross by definition).
static std::string expected_musl_xim() {
    if constexpr (mcpp::platform::is_linux) return "musl-gcc";
    else                                    return host_musl() + "-gcc";
}

// Frontend candidates are host-aware for the same reason the mingw ones are:
// they are resolved with filesystem::exists, and on a Windows host the file on
// disk is `<triple>-g++.exe`. The .exe spelling comes first so it wins on a
// case-insensitive filesystem where both would match.
static std::string expected_musl_frontend(const std::string& triple) {
    if constexpr (mcpp::platform::is_windows) return triple + "-g++.exe";
    else                                      return triple + "-g++";
}

// ── canonical two-axis identity ──────────────────────────────────────────────

TEST(ToolchainRegistry, MapsGccSpecToGccPackage) {
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_TRUE(spec->is_host_target());
    EXPECT_EQ(spec->spec_str(), "gcc@16.1.0");
    EXPECT_EQ(spec->display(), "gcc@16.1.0");
    EXPECT_TRUE(spec->compatHint.empty());

    auto pkg = to_xim_package(*spec);
#if defined(_WIN32)
    // gcc family on a Windows host = MinGW-w64 (the GNU-env host toolchain).
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
#else
    EXPECT_EQ(pkg.ximName, "gcc");
    EXPECT_TRUE(pkg.needsGccPostInstallFixup);
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++");
#endif
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    EXPECT_EQ(pkg.display_spec(), "gcc@16.1.0");
}

TEST(ToolchainRegistry, LegacyMuslSuffixNormalizesToMuslTarget) {
    // "gcc@15.1.0-musl" — the variant moves out of the version and into the
    // target axis: (gcc, 15.1.0, <host>-linux-musl).
    auto spec = parse_toolchain_spec("gcc@15.1.0-musl");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_EQ(spec->version, "15.1.0");
    EXPECT_EQ(spec->target.str(), host_musl());
    EXPECT_FALSE(spec->compatHint.empty());      // legacy spelling → hint

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximName, expected_musl_xim());
    EXPECT_EQ(pkg.ximVersion, "15.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), expected_musl_frontend(host_musl()));
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(ToolchainRegistry, CrossArchMuslTargetPicksTripleNamedPackage) {
    // Target arch ≠ host arch → the triple-named cross package.
    ToolchainSpec spec;
    spec.family = Family::Gcc;
    spec.version = "16.1.0";
    spec.target = { "aarch64", "linux", "musl" };
    if (mcpp::platform::host_arch == std::string_view("aarch64"))
        spec.target.arch = "riscv64";                 // stay cross on any host

    auto pkg = to_xim_package(spec);
    EXPECT_EQ(pkg.ximName, spec.target.str() + "-gcc");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), expected_musl_frontend(spec.target.str()));
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(ToolchainRegistry, WindowsGnuTargetIsHostSplitAtDistributionLayer) {
    // ONE identity (gcc → x86_64-windows-gnu); the payload is host-split:
    // winlibs mingw-gcc on Windows hosts, the Linux-hosted MSVCRT cross
    // elsewhere. "cross" appears only in the xim package name — never in
    // the user-facing spec.
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(spec.has_value());
    spec->target = { "x86_64", "windows", "gnu" };

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
#if defined(_WIN32)
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++.exe");
#else
    EXPECT_EQ(pkg.ximName, "mingw-cross-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "x86_64-w64-mingw32-g++");
#endif
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
    EXPECT_EQ(pkg.display_spec(), "gcc@16.1.0 → x86_64-windows-gnu");
}

TEST(ToolchainRegistry, LegacyMingwCrossSpellingCollapses) {
    auto spec = parse_toolchain_spec("mingw-cross@16.1.0");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_EQ(spec->target.str(), "x86_64-windows-gnu");
    EXPECT_FALSE(spec->compatHint.empty());
}

TEST(ToolchainRegistry, MapsLlvmAndClangAliasesToLlvmPackage) {
    auto llvmSpec = parse_toolchain_spec("llvm", "20.1.7");
    auto clangSpec = parse_toolchain_spec("clang@20.1.7");
    ASSERT_TRUE(llvmSpec.has_value()) << llvmSpec.error();
    ASSERT_TRUE(clangSpec.has_value()) << clangSpec.error();

    EXPECT_EQ(llvmSpec->family, Family::Llvm);
    EXPECT_EQ(clangSpec->family, Family::Llvm);      // alias family → llvm
    EXPECT_TRUE(llvmSpec->compatHint.empty());
    EXPECT_FALSE(clangSpec->compatHint.empty());

    auto llvmPkg = to_xim_package(*llvmSpec);
    auto clangPkg = to_xim_package(*clangSpec);
    EXPECT_EQ(llvmPkg.ximName, "llvm");
    EXPECT_EQ(clangPkg.ximName, "llvm");
    EXPECT_EQ(clangPkg.display_spec(), "llvm@20.1.7");
    ASSERT_FALSE(clangPkg.frontendCandidates.empty());
#if defined(_WIN32)
    EXPECT_EQ(clangPkg.frontendCandidates.front(), "clang++.exe");
#else
    EXPECT_EQ(clangPkg.frontendCandidates.front(), "clang++");
#endif
}

TEST(ToolchainRegistry, ResolvesPartialMuslVersion) {
    auto spec = parse_toolchain_spec("gcc", "15-musl");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->version, "15");
    EXPECT_EQ(spec->target.str(), host_musl());

    auto resolved = with_resolved_xim_version(*spec, "15.1.0");
    auto pkg = to_xim_package(resolved);
    EXPECT_EQ(resolved.version, "15.1.0");
    EXPECT_EQ(pkg.ximName, expected_musl_xim());
    EXPECT_EQ(pkg.ximVersion, "15.1.0");
    EXPECT_EQ(pkg.display_spec(),
              std::format("gcc@15.1.0 → {}", host_musl()));
}

TEST(ToolchainRegistry, RejectsUnknownFamily) {
    auto spec = parse_toolchain_spec("tcc@1.0");
    EXPECT_FALSE(spec.has_value());
}

// ── payload reverse mapping ──────────────────────────────────────────────────

TEST(ToolchainRegistry, IdentifiesToolchainPayloadsAndSkipsOthers) {
    auto gcc = identify_xim_payload("gcc");
    ASSERT_TRUE(gcc.has_value());
    EXPECT_EQ(gcc->family, Family::Gcc);
    EXPECT_TRUE(gcc->target.empty());

    auto musl = identify_xim_payload("musl-gcc");
    ASSERT_TRUE(musl.has_value());
    EXPECT_EQ(musl->target.str(), host_musl());

    auto crossMusl = identify_xim_payload("aarch64-linux-musl-gcc");
    ASSERT_TRUE(crossMusl.has_value());
    EXPECT_EQ(crossMusl->target.str(), "aarch64-linux-musl");

    auto llvm = identify_xim_payload("llvm");
    ASSERT_TRUE(llvm.has_value());
    EXPECT_EQ(llvm->family, Family::Llvm);

    // Non-toolchain xpkgs must not be identified (list/doctor filter on this).
    EXPECT_FALSE(identify_xim_payload("ninja").has_value());
    EXPECT_FALSE(identify_xim_payload("glibc").has_value());
    EXPECT_FALSE(identify_xim_payload("python").has_value());
    EXPECT_FALSE(identify_xim_payload("linux-headers").has_value());
}

// #367: which GCC payload backs a NATIVE Linux build.
//
// `xim:gcc` declares `archs = { "x86_64" }` and publishes assets for that arch
// only; the GCC the ecosystem ships for other Linux architectures is
// `musl-gcc` (xlings-res carries musl-gcc-16.1.0-linux-aarch64.tar.gz). Asking
// for `gcc` on aarch64 therefore 404s — which is what made every project whose
// graph contains a `build.mcpp` unbuildable there, since a build program's
// host compile resolves the spec with no target injection and landed on the
// glibc package.
//
// Tested through a free function taking the host arch rather than the
// compile-time constant, precisely so the aarch64 answer is checkable from an
// x86_64 machine.
TEST(ToolchainRegistry, NativeGccPayloadFollowsWhatTheArchActuallyPublishes) {
    using mcpp::toolchain::gcc_native_payload_is_musl;
    const mcpp::toolchain::triple::Triple none{};

    // x86_64: unchanged — the glibc package is the one that exists.
    EXPECT_FALSE(gcc_native_payload_is_musl("x86_64", true, none));
    EXPECT_FALSE(gcc_native_payload_is_musl(
        "x86_64", true, {"x86_64", "linux", "gnu"}));

    // aarch64: the glibc package has no asset, musl-gcc does.
    EXPECT_TRUE(gcc_native_payload_is_musl("aarch64", true, none));
    EXPECT_TRUE(gcc_native_payload_is_musl(
        "aarch64", true, {"aarch64", "linux", "gnu"}));

    // A CROSS target keeps its own payload rule — this is about the native
    // one, and `<triple>-gcc` packages answer for the rest.
    EXPECT_FALSE(gcc_native_payload_is_musl(
        "aarch64", true, {"x86_64", "linux", "gnu"}));

    // Non-Linux hosts are out of scope: macOS uses llvm, Windows mingw/msvc.
    EXPECT_FALSE(gcc_native_payload_is_musl("aarch64", false, none));
}
