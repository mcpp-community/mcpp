module;
#include <string>
export module device;

// A named C++23 module, compiled by mcpp's own clang for aarch64-linux-ohos.
// Deliberately NOT `import std`: the OpenHarmony SDK's bundled libc++ is 15.0.4
// and ships no `std` module, so this is the shape that works against a stock
// SDK. Point $MCPP_OHOS_LIBCXX at a libc++ built for the target and `import std`
// becomes available too (docs/03-toolchains.md#harmonyos).
export std::string device_banner() {
    return "mcpp on HarmonyOS — C++23 named modules, cross-built and really run";
}
