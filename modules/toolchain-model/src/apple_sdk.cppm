// mcpp.toolchain.apple_sdk — Apple's SDKs, their layout and versions, and the
// deployment floor: what a toolchain for an Apple target is built against.
//
// Provides:
//   has_xcode_clt()       — detect Xcode Command Line Tools
//   sdk_path(sdk)         — discover an Apple SDK via xcrun ("macosx" default)
//   sdk_layout(sdk)       — the directory names that SDK uses inside Xcode
//   sdk_name_of_root(p)   — which SDK a path IS, for the SDKROOT override
//   supports_full_static  — macOS cannot fully static-link (libSystem)

export module mcpp.toolchain.apple_sdk;

import std;
import mcpplibs.os.platform;

export namespace mcpp::toolchain::apple {

// Whether macOS supports full static linking (it does not — libSystem
// must be dynamically linked).
constexpr bool supports_full_static = false;

// Check whether Xcode Command Line Tools are installed.
// Returns true if `xcode-select -p` succeeds.
bool has_xcode_clt();

// THREE SDKS, ONE MACHINE. Apple ships the macOS, iPhoneOS and
// iPhoneSimulator SDKs from one developer directory, and mcpp locates
// rather than installs all three: they are not redistributable, which
// is why the iOS rows carry a located `sysroot` instead of a package.
// `sdk` is the name `xcrun --sdk` takes.
inline constexpr std::string_view sdk_macos     = "macosx";
inline constexpr std::string_view sdk_iphoneos  = "iphoneos";
inline constexpr std::string_view sdk_iphonesim = "iphonesimulator";

// Where an SDK lives inside a developer directory: the `.platform`
// directory that holds it, and its own `.sdk` directory name. Pure, and
// nullopt for a name no Apple SDK answers to.
struct SdkLayout {
    std::string_view platformDir;   // e.g. "iPhoneOS.platform"
    std::string_view sdkDir;        // e.g. "iPhoneOS.sdk"
    bool             inCommandLineTools;  // the CLT-only install ships it
};
std::optional<SdkLayout> sdk_layout(std::string_view sdk);

// Which SDK a path IS, derived from its own directory name:
// `MacOSX15.4.sdk` -> "macosx", `iPhoneSimulator18.4.sdk` ->
// "iphonesimulator". Empty when the path does not name an SDK.
//
// THIS EXISTS BECAUSE `SDKROOT` NAMES ONE SDK AND THERE ARE THREE
// QUESTIONS. Honouring it for every request would answer an iOS query
// with a macOS SDK whenever a shell had it set -- a wrong sysroot, which
// fails later as missing headers rather than as a bad override.
std::string sdk_name_of_root(const std::filesystem::path& p);

// Whether an `SDKROOT` override answers a request for `sdk`.
//
// A path that names an SDK answers for that SDK and no other. A path that
// names NO SDK -- a hand-rolled sysroot, which is a spelling clang accepts --
// answers the DEFAULT question only: it is an answer to "the SDK", not to
// "the iPhoneOS SDK". That asymmetry is what keeps today's callers unchanged
// while making a specific request require positive identification.
bool sdkroot_answers(const std::filesystem::path& candidate,
                     std::string_view sdk);

// The version an SDK directory says it is, from its own `SDKSettings.json`
// (`"Version": "18.5"`). Empty when the file is absent or says nothing. Pure
// file reading, so it answers on every host.
std::string sdk_settings_version(const std::filesystem::path& sdkRoot);

// Whether an SDK carries a slice for `target` (`x86_64-ios-simulator`,
// `arm64-macos`), read from the `targets:` list of its `libSystem.tbd`. The
// slice is what decides whether a row can be built against this SDK; a
// vendor's hardware roadmap does not (D16). `unknown` when the stub cannot be
// read. Three named answers rather than an optional bool; measured with GCC
// 16.1, the optional form here left the compiled interface of a module that
// imports this one beside mcpp.modgraph.graph unreadable to its importers.
enum class Slice { unknown, carried, absent };
Slice sdk_has_target(const std::filesystem::path& sdkRoot, std::string_view target);

// Discover an Apple SDK path via `xcrun --show-sdk-path`.
// Returns the SDK path if found, or nullopt. The default argument is
// today's behaviour for today's callers.
std::optional<std::filesystem::path> sdk_path(std::string_view sdk = sdk_macos);

// The version the located SDK was made for, as `xcrun --sdk <name>
// --show-sdk-version` reports it ("18.5"). Empty off macOS and when xcrun
// cannot answer. Read for an iOS row whose manifest states no deployment
// floor, since clang's own default for an unversioned iOS triple is older
// than any SDK on the machine (it refused thread-local storage; measured).
std::optional<std::string> sdk_version(std::string_view sdk);

// Built-in default deployment floor (rustc-style: every target has a
// baseline). 14.0 = the floor of the official LLVM static libc++
// archives; with the default-static stdlib this makes `mcpp run`
// binaries portable to any macOS ≥ 14 out of the box (no declaration
// needed — a fresh user's std::println hello on macOS 14 used to die
// at dyld against the system libc++). Lower floors need a custom
// libc++ build (tracked; data-only swap via xlings-res).
inline constexpr std::string_view default_deployment_target = "14.0";

// Resolve the effective macOS deployment target: the
// MACOSX_DEPLOYMENT_TARGET env var (explicit per-invocation override,
// the convention cargo/rustc/cc honor) wins over `manifestValue` (the
// [build] macos_deployment_target project default), which wins over
// the built-in default floor.
//
// WHETHER IT APPLIES IS A TARGET QUESTION, NOT A HOST QUESTION, so
// `targetIsMacos` is a parameter rather than a `#if defined(__APPLE__)`
// read inside this function. Every reader that folds this value into a
// compiler flag, a triple or the build fingerprint builds for a
// particular target (native or `--target`-crossed), and that target's
// OS -- not the machine running mcpp -- decides whether a macOS floor
// makes sense. The resolution itself (env > manifest > 14.0) never
// consults the host either, which is what makes `mcpp build --target
// aarch64-macos` on Linux honour `macos_deployment_target` at all: the
// old `#if defined(__APPLE__)` guard answered empty on every non-Apple
// host regardless of what the target was.
//
// Each caller answers the "is the target macOS" question itself --
// typically `parsedTriple.os == "macos"` -- and passes the bool.
//
// Result is never empty when `targetIsMacos` is true; always empty
// when it is false. THE single source of truth — flags.cppm, the BMI
// fingerprint rule and the std-module prebuild must all consume this
// same resolution, or cached std.pcm modules drift from the TUs
// (config-mismatch / unstaged-module failures observed on macos CI).
std::string deployment_target(bool targetIsMacos, std::string_view manifestValue);



} // namespace mcpp::toolchain::apple
