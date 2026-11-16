
import std;
import mcpplibs.mtest;
import mcpplibs.os.platform;
import mcpp.toolchain.platform;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

// How a built artifact is NAMED is a property of the TARGET, never of the
// machine doing the build.
//
// mcpp used to answer it with the host's {exe_suffix,lib_prefix,
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

UTest linux_target_ignores_host_answer{"ArtifactNaming/LinuxTargetIgnoresHostAnswer", [] {
    auto n = tr::artifact_naming(T("x86_64-linux-musl"), kBogusHost);
    expect(eq(n.exeSuffix, ""));
    expect(eq(n.libPrefix, "lib"));
    expect(eq(n.staticLibExt, ".a"));
    expect(eq(n.sharedLibExt, ".so"));
    expect(!n.sharedNeedsImportLib);
}};

UTest macos_target_ignores_host_answer{"ArtifactNaming/MacosTargetIgnoresHostAnswer", [] {
    auto n = tr::artifact_naming(T("aarch64-macos"), kBogusHost);
    expect(eq(n.exeSuffix, ""));
    expect(eq(n.libPrefix, "lib"));
    expect(eq(n.staticLibExt, ".a"));
    expect(eq(n.sharedLibExt, ".dylib"));
    expect(!n.sharedNeedsImportLib);
}};

// ── The (os, env) split — the part a single _WIN32 branch cannot express ─────
//
// windows-gnu uses the GNU convention. mcpp names this `foo.lib` today even on
// a Windows host, so mingw's `ar` emits a GNU archive wearing an MSVC name.
// That is a pre-existing defect, independent of cross compilation.
UTest windows_gnu_uses_gnu_convention{"ArtifactNaming/WindowsGnuUsesGnuConvention", [] {
    auto n = tr::artifact_naming(T("x86_64-windows-gnu"), kBogusHost);
    expect(eq(n.exeSuffix, ".exe"));
    expect(eq(n.libPrefix, "lib"));       // NOT ""
    expect(eq(n.staticLibExt, ".a"));     // NOT ".lib"
    expect(eq(n.sharedLibExt, ".dll"));
    expect(bool(n.sharedNeedsImportLib));
}};

UTest windows_msvc_uses_msvc_convention{"ArtifactNaming/WindowsMsvcUsesMsvcConvention", [] {
    auto n = tr::artifact_naming(T("x86_64-windows-msvc"), kBogusHost);
    expect(eq(n.exeSuffix, ".exe"));
    expect(eq(n.libPrefix, ""));
    expect(eq(n.staticLibExt, ".lib"));
    expect(eq(n.sharedLibExt, ".dll"));
    expect(bool(n.sharedNeedsImportLib));
}};

// The legacy mingw spelling must resolve identically to the canonical one —
// the triple parser is the single source of truth, not a substring match.
UTest legacy_mingw_spelling_matches_canonical{"ArtifactNaming/LegacyMingwSpellingMatchesCanonical", [] {
    auto legacy    = tr::artifact_naming(T("x86_64-w64-mingw32"), kBogusHost);
    auto canonical = tr::artifact_naming(T("x86_64-windows-gnu"), kBogusHost);
    expect(eq(legacy.exeSuffix, canonical.exeSuffix));
    expect(eq(legacy.libPrefix, canonical.libPrefix));
    expect(eq(legacy.staticLibExt, canonical.staticLibExt));
    expect(eq(legacy.sharedLibExt, canonical.sharedLibExt));
}};

// ── Host target: the one case where the host answer IS correct ──────────────

UTest empty_triple_falls_back_to_host{"ArtifactNaming/EmptyTripleFallsBackToHost", [] {
    auto n = tr::artifact_naming(tr::Triple{}, kBogusHost);
    expect(eq(n.exeSuffix, ".HOST"));
    expect(eq(n.libPrefix, "HOST"));
}};

// An unparseable triple must not be guessed at — a wrong guess here silently
// misnames every artifact of the build.
UTest unknown_os_falls_back_to_host{"ArtifactNaming/UnknownOsFallsBackToHost", [] {
    tr::Triple wasm; wasm.arch = "wasm32"; wasm.os = "unknown";
    auto n = tr::artifact_naming(wasm, kBogusHost);
    expect(eq(n.exeSuffix, ".HOST"));
    expect(eq(n.staticLibExt, ".HOST"));
}};

// A bare-metal `none` OS is not the emscripten row and must keep falling back
// to the host answer — the emscripten branch below must not widen the
// fallback it sits next to.
UTest none_os_falls_back_to_host{"ArtifactNaming/NoneOsFallsBackToHost", [] {
    tr::Triple none; none.arch = "aarch64"; none.os = "none";
    auto n = tr::artifact_naming(none, kBogusHost);
    expect(eq(n.exeSuffix, ".HOST"));
    expect(eq(n.staticLibExt, ".HOST"));
}};

// ── wasm32-emscripten: the executable is the file a runner executes, and on
// this row that file is the JavaScript launcher, not a bare ELF-shaped name.
// See .agents/docs/2026-09-12-622-a-ui-framework-on-android-ios-and-web.md §2.5.

// A Linux host must not leak its own (empty-suffix) naming onto the row.
UTest emscripten_on_linux_host_names_js{"ArtifactNaming/EmscriptenOnLinuxHostNamesJs", [] {
    const tr::ArtifactNaming linuxHost{
        .exeSuffix = "", .libPrefix = "lib", .staticLibExt = ".a",
        .sharedLibExt = ".so", .sharedNeedsImportLib = false,
    };
    auto n = tr::artifact_naming(T("wasm32-emscripten"), linuxHost);
    expect(eq(n.exeSuffix, ".js"));
    expect(eq(n.libPrefix, "lib"));
    expect(eq(n.staticLibExt, ".a"));
}};

// A Windows host must not leak `.exe` onto the row either — the row names one
// file on every host.
UTest emscripten_on_windows_host_names_js{"ArtifactNaming/EmscriptenOnWindowsHostNamesJs", [] {
    const tr::ArtifactNaming windowsHost{
        .exeSuffix = ".exe", .libPrefix = "", .staticLibExt = ".lib",
        .sharedLibExt = ".dll", .sharedNeedsImportLib = true,
    };
    auto n = tr::artifact_naming(T("wasm32-emscripten"), windowsHost);
    expect(eq(n.exeSuffix, ".js"));
    expect(eq(n.libPrefix, "lib"));
    expect(eq(n.staticLibExt, ".a"));
}};

// A side module needs `-sSIDE_MODULE`, which mcpp does not render — the row
// marks shared libraries unsupported rather than naming a file emcc's default
// link would not actually produce as a working shared object.
UTest emscripten_shared_is_unsupported{"ArtifactNaming/EmscriptenSharedIsUnsupported", [] {
    auto n = tr::artifact_naming(T("wasm32-emscripten"), kBogusHost);
    expect(bool(n.sharedNeedsImportLib));
    expect(eq(n.sharedLibExt, ""));
}};

// ── Host builds must be bit-for-bit unchanged ───────────────────────────────
// Passing the real host constants for the host target has to reproduce exactly
// what the old code produced on this machine.
UTest real_host_constants_round_trip{"ArtifactNaming/RealHostConstantsRoundTrip", [] {
    const tr::ArtifactNaming host{
        .exeSuffix = mcpplibs::os::exe_suffix,
        .libPrefix = mcpp::toolchain::platform::lib_prefix,
        .staticLibExt = mcpp::toolchain::platform::static_lib_ext,
        .sharedLibExt = mcpp::toolchain::platform::shared_lib_ext,
        .sharedNeedsImportLib = mcpplibs::os::is_windows,
    };
    auto n = tr::artifact_naming(tr::Triple{}, host);
    expect(eq(n.exeSuffix, mcpplibs::os::exe_suffix));
    expect(eq(n.libPrefix, mcpp::toolchain::platform::lib_prefix));
    expect(eq(n.staticLibExt, mcpp::toolchain::platform::static_lib_ext));
    expect(eq(n.sharedLibExt, mcpp::toolchain::platform::shared_lib_ext));
}};

// ── #622 A3: the link form of `kind = "app"` is a property of the row ───────
//
// `application_form` answers the one question §2.3 of the #622 design record
// turns into a fourth `kind` rather than a manifest predicate: on Android an
// application IS the shared library the platform loads (there is no
// executable form of an app there at all); everywhere else it is identical to
// `bin`. The negative direction here is the positive direction of every OTHER
// row — a form function with only one answer would not be a function.

UTest application_form_is_shared_object_on_android{"ArtifactNaming/ApplicationFormIsSharedObjectOnAndroid", [] {
    expect(eq(tr::application_form(T("aarch64-linux-android")), tr::ApplicationForm::SharedObject));
    expect(eq(tr::application_form(T("x86_64-linux-android")), tr::ApplicationForm::SharedObject));
}};

UTest application_form_is_executable_everywhere_else{"ArtifactNaming/ApplicationFormIsExecutableEverywhereElse", [] {
    expect(eq(tr::application_form(T("x86_64-linux-gnu")), tr::ApplicationForm::Executable));
    expect(eq(tr::application_form(T("aarch64-ios-sim")), tr::ApplicationForm::Executable));
    expect(eq(tr::application_form(T("wasm32-emscripten")), tr::ApplicationForm::Executable));
    expect(eq(tr::application_form(T("x86_64-windows-msvc")), tr::ApplicationForm::Executable));
}};

// An empty (host) triple is not Android on any machine this engine runs on
// today -- the negative direction that keeps a bare `kind = "app"` host build
// linking an ordinary executable, exactly as it does before this feature.
UTest application_form_on_the_host_triple_is_executable{"ArtifactNaming/ApplicationFormOnTheHostTripleIsExecutable", [] {
    expect(eq(tr::application_form(tr::Triple{}), tr::ApplicationForm::Executable));
}};

// ── #630 A9: the Android ABI name of a row's architecture ───────────────────
//
// The two rows mcpp currently produces a shared object for; a third
// architecture keeps its GNU spelling rather than a guessed Android name.

UTest translates_the_two_known_architectures{"AndroidAbi/TranslatesTheTwoKnownArchitectures", [] {
    expect(eq(tr::android_abi(T("aarch64-linux-android")), "arm64-v8a"));
    expect(eq(tr::android_abi(T("x86_64-linux-android")), "x86_64"));
}};

UTest an_untranslated_architecture_keeps_its_gnu_spelling{"AndroidAbi/AnUntranslatedArchitectureKeepsItsGnuSpelling", [] {
    expect(eq(tr::android_abi(T("riscv64-linux-android")), "riscv64"));
}};

} // namespace

// `platform_name` (#622 A7): the triple's `os`, except that an `env` naming a
// platform of its own wins. Every row the engine has maps into the closed
// vocabulary `[package] platforms` accepts, so a claim can be made for every
// row and for nothing else.
UTest is_the_os_except_for_android{"PlatformName/IsTheOsExceptForAndroid", [] {
    using mcpp::toolchain::triple::parse;
    using mcpp::toolchain::triple::platform_name;
    expect(eq(platform_name(*parse("x86_64-linux-gnu")), "linux"));
    expect(eq(platform_name(*parse("aarch64-macos")), "macos"));
    expect(eq(platform_name(*parse("x86_64-windows-msvc")), "windows"));
    expect(eq(platform_name(*parse("aarch64-ios-sim")), "ios"));
    expect(eq(platform_name(*parse("aarch64-linux-android")), "android"));
    expect(eq(platform_name(*parse("wasm32-emscripten")), "emscripten"));
}};

UTest every_known_row_is_in_the_vocabulary{"PlatformName/EveryKnownRowIsInTheVocabulary", [] {
    using namespace mcpp::toolchain::triple;
    for (auto const& row : known_targets()) {
        auto t = parse(std::string(row.canonical));
        expect(fatal(t.has_value())) << row.canonical;
        if (t->os == "none") continue;   // bare-metal rows claim no platform
        expect(is_platform_name(platform_name(*t))) << row.canonical;
    }
    expect(is_platform_name("emscripten"));
    expect(!is_platform_name("web"));
    expect(is_platform_name("openkal"));
    expect(eq(platform_names_joined(), "linux | macos | windows | ios | android | emscripten | openkal | freebsd"));
}};
