#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.triple;

// How a built artifact is NAMED is a property of the TARGET, never of the
// machine doing the build.
//
// mcpp used to answer it with mcpp::platform::{exe_suffix,lib_prefix,
// static_lib_ext,shared_lib_ext} — host constants selected by
// `#if defined(_WIN32)/__APPLE__`. On a host build the two questions coincide,
// which is why this survived; it only diverges once host != target.
//
// The host answer is threaded in as a parameter precisely so these can be
// pinned from any host: the `hostNaming` argument models "what the build
// machine would have said", and every assertion below that passes a deliberately
// wrong one is checking that the target answer wins.

namespace {

namespace tr = mcpp::toolchain::triple;

// A deliberately WRONG host answer. If any assertion below leaks through to it,
// the target axis is not being honoured.
constexpr tr::ArtifactNaming kBogusHost{
    .exeSuffix = ".HOST", .libPrefix = "HOST", .staticLibExt = ".HOST",
    .sharedLibExt = ".HOST", .sharedNeedsImportLib = false,
};

tr::Triple T(std::string_view s) {
    auto t = tr::parse(s);
    return t ? *t : tr::Triple{};
}

// ── The regression: naming must not come from the build host ────────────────

TEST(ArtifactNaming, LinuxTargetIgnoresHostAnswer) {
    auto n = tr::artifact_naming(T("x86_64-linux-musl"), kBogusHost);
    EXPECT_EQ(n.exeSuffix, "");
    EXPECT_EQ(n.libPrefix, "lib");
    EXPECT_EQ(n.staticLibExt, ".a");
    EXPECT_EQ(n.sharedLibExt, ".so");
    EXPECT_FALSE(n.sharedNeedsImportLib);
}

TEST(ArtifactNaming, MacosTargetIgnoresHostAnswer) {
    auto n = tr::artifact_naming(T("aarch64-macos"), kBogusHost);
    EXPECT_EQ(n.exeSuffix, "");
    EXPECT_EQ(n.libPrefix, "lib");
    EXPECT_EQ(n.staticLibExt, ".a");
    EXPECT_EQ(n.sharedLibExt, ".dylib");
    EXPECT_FALSE(n.sharedNeedsImportLib);
}

// ── The (os, env) split — the part a single _WIN32 branch cannot express ─────
//
// windows-gnu uses the GNU convention. mcpp names this `foo.lib` today even on
// a Windows host, so mingw's `ar` emits a GNU archive wearing an MSVC name.
// That is a pre-existing defect, independent of cross compilation.
TEST(ArtifactNaming, WindowsGnuUsesGnuConvention) {
    auto n = tr::artifact_naming(T("x86_64-windows-gnu"), kBogusHost);
    EXPECT_EQ(n.exeSuffix, ".exe");
    EXPECT_EQ(n.libPrefix, "lib");       // NOT ""
    EXPECT_EQ(n.staticLibExt, ".a");     // NOT ".lib"
    EXPECT_EQ(n.sharedLibExt, ".dll");
    EXPECT_TRUE(n.sharedNeedsImportLib);
}

TEST(ArtifactNaming, WindowsMsvcUsesMsvcConvention) {
    auto n = tr::artifact_naming(T("x86_64-windows-msvc"), kBogusHost);
    EXPECT_EQ(n.exeSuffix, ".exe");
    EXPECT_EQ(n.libPrefix, "");
    EXPECT_EQ(n.staticLibExt, ".lib");
    EXPECT_EQ(n.sharedLibExt, ".dll");
    EXPECT_TRUE(n.sharedNeedsImportLib);
}

// The legacy mingw spelling must resolve identically to the canonical one —
// the triple parser is the single source of truth, not a substring match.
TEST(ArtifactNaming, LegacyMingwSpellingMatchesCanonical) {
    auto legacy    = tr::artifact_naming(T("x86_64-w64-mingw32"), kBogusHost);
    auto canonical = tr::artifact_naming(T("x86_64-windows-gnu"), kBogusHost);
    EXPECT_EQ(legacy.exeSuffix,    canonical.exeSuffix);
    EXPECT_EQ(legacy.libPrefix,    canonical.libPrefix);
    EXPECT_EQ(legacy.staticLibExt, canonical.staticLibExt);
    EXPECT_EQ(legacy.sharedLibExt, canonical.sharedLibExt);
}

// ── Host target: the one case where the host answer IS correct ──────────────

TEST(ArtifactNaming, EmptyTripleFallsBackToHost) {
    auto n = tr::artifact_naming(tr::Triple{}, kBogusHost);
    EXPECT_EQ(n.exeSuffix, ".HOST");
    EXPECT_EQ(n.libPrefix, "HOST");
}

// An unparseable triple must not be guessed at — a wrong guess here silently
// misnames every artifact of the build.
TEST(ArtifactNaming, UnknownOsFallsBackToHost) {
    tr::Triple wasm; wasm.arch = "wasm32"; wasm.os = "unknown";
    auto n = tr::artifact_naming(wasm, kBogusHost);
    EXPECT_EQ(n.exeSuffix, ".HOST");
    EXPECT_EQ(n.staticLibExt, ".HOST");
}

// ── Host builds must be bit-for-bit unchanged ───────────────────────────────
// Passing the real host constants for the host target has to reproduce exactly
// what the old code produced on this machine.
TEST(ArtifactNaming, RealHostConstantsRoundTrip) {
    const tr::ArtifactNaming host{
        .exeSuffix = mcpp::platform::exe_suffix,
        .libPrefix = mcpp::platform::lib_prefix,
        .staticLibExt = mcpp::platform::static_lib_ext,
        .sharedLibExt = mcpp::platform::shared_lib_ext,
        .sharedNeedsImportLib = mcpp::platform::is_windows,
    };
    auto n = tr::artifact_naming(tr::Triple{}, host);
    EXPECT_EQ(n.exeSuffix,    mcpp::platform::exe_suffix);
    EXPECT_EQ(n.libPrefix,    mcpp::platform::lib_prefix);
    EXPECT_EQ(n.staticLibExt, mcpp::platform::static_lib_ext);
    EXPECT_EQ(n.sharedLibExt, mcpp::platform::shared_lib_ext);
}

} // namespace
