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

// A bare-metal `none` OS is not the emscripten row and must keep falling back
// to the host answer — the emscripten branch below must not widen the
// fallback it sits next to.
TEST(ArtifactNaming, NoneOsFallsBackToHost) {
    tr::Triple none; none.arch = "aarch64"; none.os = "none";
    auto n = tr::artifact_naming(none, kBogusHost);
    EXPECT_EQ(n.exeSuffix, ".HOST");
    EXPECT_EQ(n.staticLibExt, ".HOST");
}

// ── wasm32-emscripten: the executable is the file a runner executes, and on
// this row that file is the JavaScript launcher, not a bare ELF-shaped name.
// See .agents/docs/2026-09-12-622-a-ui-framework-on-android-ios-and-web.md §2.5.

// A Linux host must not leak its own (empty-suffix) naming onto the row.
TEST(ArtifactNaming, EmscriptenOnLinuxHostNamesJs) {
    const tr::ArtifactNaming linuxHost{
        .exeSuffix = "", .libPrefix = "lib", .staticLibExt = ".a",
        .sharedLibExt = ".so", .sharedNeedsImportLib = false,
    };
    auto n = tr::artifact_naming(T("wasm32-emscripten"), linuxHost);
    EXPECT_EQ(n.exeSuffix, ".js");
    EXPECT_EQ(n.libPrefix, "lib");
    EXPECT_EQ(n.staticLibExt, ".a");
}

// A Windows host must not leak `.exe` onto the row either — the row names one
// file on every host.
TEST(ArtifactNaming, EmscriptenOnWindowsHostNamesJs) {
    const tr::ArtifactNaming windowsHost{
        .exeSuffix = ".exe", .libPrefix = "", .staticLibExt = ".lib",
        .sharedLibExt = ".dll", .sharedNeedsImportLib = true,
    };
    auto n = tr::artifact_naming(T("wasm32-emscripten"), windowsHost);
    EXPECT_EQ(n.exeSuffix, ".js");
    EXPECT_EQ(n.libPrefix, "lib");
    EXPECT_EQ(n.staticLibExt, ".a");
}

// A side module needs `-sSIDE_MODULE`, which mcpp does not render — the row
// marks shared libraries unsupported rather than naming a file emcc's default
// link would not actually produce as a working shared object.
TEST(ArtifactNaming, EmscriptenSharedIsUnsupported) {
    auto n = tr::artifact_naming(T("wasm32-emscripten"), kBogusHost);
    EXPECT_TRUE(n.sharedNeedsImportLib);
    EXPECT_EQ(n.sharedLibExt, "");
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

// ── #622 A3: the link form of `kind = "app"` is a property of the row ───────
//
// `application_form` answers the one question §2.3 of the #622 design record
// turns into a fourth `kind` rather than a manifest predicate: on Android an
// application IS the shared library the platform loads (there is no
// executable form of an app there at all); everywhere else it is identical to
// `bin`. The negative direction here is the positive direction of every OTHER
// row — a form function with only one answer would not be a function.

TEST(ArtifactNaming, ApplicationFormIsSharedObjectOnAndroid) {
    EXPECT_EQ(tr::application_form(T("aarch64-linux-android")),
              tr::ApplicationForm::SharedObject);
    EXPECT_EQ(tr::application_form(T("x86_64-linux-android")),
              tr::ApplicationForm::SharedObject);
}

TEST(ArtifactNaming, ApplicationFormIsExecutableEverywhereElse) {
    EXPECT_EQ(tr::application_form(T("x86_64-linux-gnu")),
              tr::ApplicationForm::Executable);
    EXPECT_EQ(tr::application_form(T("aarch64-ios-sim")),
              tr::ApplicationForm::Executable);
    EXPECT_EQ(tr::application_form(T("wasm32-emscripten")),
              tr::ApplicationForm::Executable);
    EXPECT_EQ(tr::application_form(T("x86_64-windows-msvc")),
              tr::ApplicationForm::Executable);
}

// An empty (host) triple is not Android on any machine this engine runs on
// today -- the negative direction that keeps a bare `kind = "app"` host build
// linking an ordinary executable, exactly as it does before this feature.
TEST(ArtifactNaming, ApplicationFormOnTheHostTripleIsExecutable) {
    EXPECT_EQ(tr::application_form(tr::Triple{}), tr::ApplicationForm::Executable);
}

} // namespace

// `platform_name` (#622 A7): the triple's `os`, except that an `env` naming a
// platform of its own wins. Every row the engine has maps into the closed
// vocabulary `[package] platforms` accepts, so a claim can be made for every
// row and for nothing else.
TEST(PlatformName, IsTheOsExceptForAndroid) {
    using mcpp::toolchain::triple::parse;
    using mcpp::toolchain::triple::platform_name;
    EXPECT_EQ(platform_name(*parse("x86_64-linux-gnu")), "linux");
    EXPECT_EQ(platform_name(*parse("aarch64-macos")), "macos");
    EXPECT_EQ(platform_name(*parse("x86_64-windows-msvc")), "windows");
    EXPECT_EQ(platform_name(*parse("aarch64-ios-sim")), "ios");
    EXPECT_EQ(platform_name(*parse("aarch64-linux-android")), "android");
    EXPECT_EQ(platform_name(*parse("wasm32-emscripten")), "emscripten");
}

TEST(PlatformName, EveryKnownRowIsInTheVocabulary) {
    using namespace mcpp::toolchain::triple;
    for (auto const& row : known_targets()) {
        auto t = parse(std::string(row.canonical));
        ASSERT_TRUE(t.has_value()) << row.canonical;
        if (t->os == "none") continue;   // bare-metal rows claim no platform
        EXPECT_TRUE(is_platform_name(platform_name(*t))) << row.canonical;
    }
    EXPECT_TRUE(is_platform_name("emscripten"));
    EXPECT_FALSE(is_platform_name("web"));
    EXPECT_EQ(platform_names_joined(), "linux | macos | windows | ios | android | emscripten");
}
