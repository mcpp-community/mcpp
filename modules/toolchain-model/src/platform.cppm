// mcpp.toolchain.platform — the host/target platform axes, as distinct types,
// and the facts of the host in the toolchain model's vocabulary.
//
// mcpp resolves "which platform?" along TWO axes that are equal only for a
// native build:
//
//   host   — the machine mcpp is running on. Correct for host TOOLS: the
//            toolchain payload, ninja, patchelf.
//   target — the platform the produced binaries will run on. Correct for
//            anything compiled INTO the user's build: a dependency's
//            sources, flags, deps, and the prebuilt asset chosen for it.
//
// Both are spelled in the xpkg descriptor vocabulary (linux | macosx |
// windows) and both were plain std::string_view, so mixing them cost
// nothing and read identically. #254: every xpkg per-OS section and every
// xpm version table was keyed on a compile-time HOST constant, while
// `[target.'cfg(...)']` was evaluated against the RESOLVED TARGET — the same
// decision derived two different ways. Cross-compiling therefore spliced the
// host's section into a dependency built for the target. Native builds hide
// it, because there the two axes coincide, which is why three-platform CI
// never caught it.
//
// Making them separate types means an API states which axis it wants, and a
// call site must name the axis it is supplying. There is deliberately no
// conversion between them and no constructor from a bare string: the
// interesting bugs here are all "the wrong axis was passed", and a string
// parameter cannot express the difference.

export module mcpp.toolchain.platform;

import std;
import mcpplibs.os.platform;

export namespace mcpp::toolchain::platform {

// THE MACHINE mcpp RUNS ON, in the toolchain model's vocabulary. Each is a fact
// of the host, read where a HOST answer is the right one (a host tool, a
// payload for this machine); an artifact for a target asks the target's triple.
inline constexpr std::string_view name      = mcpplibs::os::name(mcpplibs::os::PLATFORM);
inline constexpr std::string_view host_arch = mcpplibs::os::name(mcpplibs::os::ARCH);
inline constexpr std::string_view xpkg_platform = mcpplibs::os::is_windows ? "windows"
                                                : mcpplibs::os::is_macos   ? "macosx" : "linux";
inline constexpr std::string_view static_lib_ext = mcpplibs::os::is_windows ? ".lib" : ".a";
inline constexpr std::string_view shared_lib_ext = mcpplibs::os::is_windows ? ".dll"
                                                 : mcpplibs::os::is_macos   ? ".dylib" : ".so";
inline constexpr std::string_view lib_prefix     = mcpplibs::os::is_windows ? "" : "lib";
inline constexpr bool supports_full_static  = mcpplibs::os::is_linux;     // macOS and Windows cannot
inline constexpr bool supports_rpath        = !mcpplibs::os::is_windows;  // ELF and Mach-O
inline constexpr bool needs_explicit_libcxx = mcpplibs::os::is_macos;     // macOS: -lc++

// Is this Linux an Android (D21)? `ANDROID_ROOT` and `ANDROID_DATA` are set in
// every Android process environment (Termux included), and
// `/system/bin/linker64` is bionic's dynamic loader. Answers which shell exists
// and how a failure is explained; it does not choose a toolchain: Android takes
// the same default as GNU/Linux on its architecture.
bool is_android_host();

}  // namespace mcpp::toolchain::platform

export namespace mcpp::toolchain::platform {

// THE spelling rule, and the only one.
//
// One platform has two names: the triple vocabulary says `macos`, an xpkg
// descriptor and xlings' own project file say `macosx`. Every place that has
// to accept both — a target's `os` token, a `[xlings.workspace]` platform
// table, a descriptor's per-OS section — asks here, so the alias cannot be
// half-known. `nullopt` means the token names no platform at all, which the
// caller decides how to treat: `for_os` falls back to the host, a manifest
// parser reports it.
inline std::optional<std::string_view> xpkg_platform_key_for(std::string_view os) {
    if (os == "macos" || os == "macosx") return "macosx";
    if (os == "windows")                 return "windows";
    if (os == "linux")                   return "linux";
    return std::nullopt;
}

// The three keys a descriptor has a block for, in that vocabulary.
inline constexpr std::string_view xpkg_platforms[] = {"linux", "macosx", "windows"};

// A resolved xpkg platform key. Only obtainable through one of the axis
// types below, so possession of one implies somebody decided which axis it
// came from.
class PlatformKey {
public:
    std::string_view key() const { return key_; }

    bool operator==(const PlatformKey& other) const { return key_ == other.key_; }

protected:
    explicit PlatformKey(std::string key) : key_(std::move(key)) {}

private:
    std::string key_;
};

// The machine mcpp itself is running on.
class HostPlatform : public PlatformKey {
public:
    static HostPlatform current() {
        return HostPlatform(std::string(xpkg_platform));
    }

private:
    explicit HostPlatform(std::string key) : PlatformKey(std::move(key)) {}
};

// The platform the artifacts being produced will run on.
class TargetPlatform : public PlatformKey {
public:
    // From a resolved triple's `os` token. The triple vocabulary says
    // "macos" where xpkg descriptors say "macosx"; that translation lives
    // here so no caller has to remember it.
    static TargetPlatform for_os(std::string_view tripleOs) {
        // Unknown/absent os token: fall back to the host, which is what the
        // whole code base did unconditionally before #254.
        return TargetPlatform(std::string(
            xpkg_platform_key_for(tripleOs)
                .value_or(xpkg_platform)));
    }

    // For tooling that walks platforms it is not running on and is not
    // building for — `mcpp xpkg parse --all-os` lints every per-OS section.
    // Named awkwardly on purpose: it should be obvious at the call site that
    // this is neither axis.
    static TargetPlatform for_lint_of(std::string_view xpkgKey) {
        return TargetPlatform(std::string(xpkgKey));
    }

private:
    explicit TargetPlatform(std::string key) : PlatformKey(std::move(key)) {}
};

} // namespace mcpp::toolchain::platform
