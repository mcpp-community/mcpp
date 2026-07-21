// mcpp.platform.axis — the host/target platform axes, as distinct types.
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

export module mcpp.platform.axis;

import std;
import mcpp.platform;

export namespace mcpp::platform {

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
        return HostPlatform(std::string(mcpp::platform::xpkg_platform));
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
        if (tripleOs == "macos" || tripleOs == "macosx") return TargetPlatform("macosx");
        if (tripleOs == "windows")                       return TargetPlatform("windows");
        if (tripleOs == "linux")                         return TargetPlatform("linux");
        // Unknown/absent os token: fall back to the host, which is what the
        // whole code base did unconditionally before #254.
        return TargetPlatform(std::string(mcpp::platform::xpkg_platform));
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

} // namespace mcpp::platform
