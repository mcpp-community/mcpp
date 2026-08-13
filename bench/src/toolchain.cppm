// bench.toolchain — WHICH compiler every engine is handed, and where it lives.
//
// This module exists because the same decision was being made in two places.
// The fixture's generated `mcpp.toml` pinned `gcc@16.1.0`, and the CI workflow
// separately resolved `command -v g++` for cmake/xmake/bazel. Those two are not
// the same compiler, and nothing anywhere said so:
//
//   * mcpp built the fixture with the registry's gcc 16.1.0 and passed;
//   * cmake and xmake were handed the runner's gcc 13.3.0, which cannot build
//     C++23 modules at all — cmake failed to configure, xmake crashed gcc with
//     an internal compiler error, and both were recorded as `failed`.
//
// Forty-eight of the seventy-two cells in a "passing" matrix job failed that
// way. The suite's own fairness rule (see `resolve_cxx`) says every engine that
// can be told which compiler to use MUST be told the same one; this module is
// what makes that rule reachable, by naming ONE payload and handing it to
// everybody including mcpp.
//
// The versions are pinned rather than "whatever is newest" for the reason every
// other pin in this repository exists: a benchmark whose toolchain moves under
// it reports the toolchain's change as the engine's.
export module bench.toolchain;

import std;
import bench.platform;

export namespace bench::toolchain {

// The payload every arm of the benchmark compiles against.
//
// Windows is on llvm 20.1.7 rather than 22.1.8 because that is the version
// mcpp's registry actually ships for the PE target; pinning a version that is
// not there does not produce a slower number, it produces `unavailable`.
inline constexpr std::string_view kGcc          = "16.1.0";
inline constexpr std::string_view kLlvm         = "22.1.8";
inline constexpr std::string_view kLlvmWindows  = "20.1.7";

inline bool on_windows() { return platform::OS_NAME == "windows"; }

// Is this compiler request a clang one? The single spelling of that test, used
// by both the manifest emitter and the payload lookup.
inline bool is_clang_request(std::string_view compiler) {
    return compiler.find("clang") != std::string_view::npos
        || compiler.find("llvm")  != std::string_view::npos;
}

// Which FAMILY a compiler request resolves to on this host — the single
// decision `mcpp_pin` and `payload_cxx` both read, so the toolchain mcpp is told
// to use and the driver every other engine is handed cannot disagree.
//
// ⚠️ THE HOST IS PART OF THE ANSWER. There is no gcc payload for macOS in mcpp's
// registry (bench/matrix.json excludes the macos/gcc cell for exactly that
// reason), and the Windows payload is llvm. A pin that reads `gcc@16.1.0`
// everywhere fails on those hosts with
//
//     error: toolchain 'gcc@16.1.0': package 'xim:gcc@16.1.0' not found
//
// which is what happened the moment this replaced the old emitter's explicit
// `macos = "llvm@..."` / `windows = "llvm@..."` overrides with one `default`.
inline bool resolves_to_clang(std::string_view compiler) {
    return is_clang_request(compiler) || platform::OS_NAME != "linux";
}

// What the fixture's `mcpp.toml` must say so that mcpp uses the same compiler
// every other engine was handed.
inline std::string mcpp_pin(std::string_view compiler) {
    if (resolves_to_clang(compiler))
        return std::format("llvm@{}", on_windows() ? kLlvmWindows : kLlvm);
    return std::format("gcc@{}", kGcc);
}

// Where mcpp keeps its packages. MCPP_HOME first, matching mcpp's own
// resolution order and the CMake helper in projects/common/.
inline std::filesystem::path registry_xpkgs() {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    if (const char* home = std::getenv("MCPP_HOME"))
        return std::filesystem::path(home) / "registry" / "data" / "xpkgs";
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    const char* user = std::getenv(platform::OS_NAME == "windows" ? "USERPROFILE" : "HOME");
    if (!user) return {};
    return std::filesystem::path(user) / ".mcpp" / "registry" / "data" / "xpkgs";
}

// The C++ driver for `compiler` inside that payload, or nullopt with a reason.
//
// Returning the REASON rather than a bare nullopt matters: "the payload is not
// unpacked" and "this machine has no mcpp" lead to different fixes, and a
// benchmark that silently falls back to the host compiler when it cannot find
// the payload is the exact failure this module was written to end.
struct Resolved {
    std::filesystem::path driver;
    std::string           why;      // set when `driver` is empty
};

inline Resolved payload_cxx(std::string_view compiler) {
    const auto xpkgs = registry_xpkgs();
    if (xpkgs.empty())
        return {{}, "neither MCPP_HOME nor HOME/USERPROFILE is set"};

    const bool clang = resolves_to_clang(compiler);
    const std::string pkg = clang ? "xim-x-llvm" : "xim-x-gcc";
    const std::string ver{clang ? (on_windows() ? kLlvmWindows : kLlvm) : kGcc};
    const std::string exe = std::string(clang ? "clang++" : "g++")
                          + (on_windows() ? ".exe" : "");

    const auto driver = xpkgs / pkg / ver / "bin" / exe;
    std::error_code ec;
    if (std::filesystem::exists(driver, ec)) return {driver, {}};

    return {{}, std::format("{} is not unpacked (run `mcpp toolchain install {}@{}`)",
                            driver.string(), clang ? "llvm" : "gcc", ver)};
}

// The flags a FOREIGN engine needs so that a payload compiler can actually
// build and link — the generated fixture's counterpart of
// bench/projects/common/cmake/hermetic_payload.cmake.
//
// ⚠️ WHY THIS EXISTS AT ALL, given that file exists. The checked-in project
// descriptions `include()` it; the fixture is GENERATED into a scratch
// directory by a binary that may live anywhere, so it has no path to include.
// The two are the same decision in two places and must be kept in step — the
// alternative considered (emit an `include()` of an absolute path) makes every
// generated fixture depend on this checkout still being where it was.
//
// It is needed because `--compiler payload:gcc` hands cmake and xmake a
// compiler out of mcpp's registry, and a bare registry gcc has no idea where
// its assembler, linker or libc are:
//
//     /usr/bin/ld: cannot find crt1.o: No such file or directory
//     /usr/bin/ld: cannot find -lm: No such file or directory
//
// which cmake reports as "the C++ compiler is not able to compile a simple
// test program", i.e. as a configure failure with no mention of a sysroot.
struct PayloadFlags {
    std::string compile;
    std::string link;
};

inline PayloadFlags payload_flags(std::string_view compiler) {
    PayloadFlags f;
    // Only a compiler FROM the registry gets these; a host compiler already
    // knows where its own runtime is, and adding a registry sysroot to it
    // produces a mixed build that fails somewhere unrelated.
    if (compiler.find("xpkgs") == std::string_view::npos) return f;

    const auto xpkgs = registry_xpkgs();
    if (xpkgs.empty()) return f;
    std::error_code ec;

    if (!is_clang_request(compiler)) {
        // gcc: -B for `as`/`ld`, --sysroot for headers and startup files, and
        // BOTH must reach compile and link — the driver spawns `as` at compile
        // time and `ld` at link time, so one side alone silently falls back.
        for (const auto& e : std::filesystem::directory_iterator(
                 xpkgs / "xim-x-binutils", ec)) {
            f.compile += " -B" + (e.path() / "bin").string();
            f.link    += " -B" + (e.path() / "bin").string();
            break;
        }
        auto sysroot = registry_xpkgs().parent_path().parent_path()
                     / "subos" / "default";
        if (std::filesystem::is_directory(sysroot, ec)) {
            f.compile += " --sysroot=" + sysroot.string();
            f.link    += " --sysroot=" + sysroot.string();
        }
        return f;
    }

    // ⚠️ macOS GETS NOTHING, AND THAT IS THE CORRECT ANSWER.
    //
    // Pointing `-L`/`-rpath` at the registry's lib directory puts a second
    // libc++ where the platform toolchain can find it, and Apple's own linker
    // links against libc++ — so `ld` itself was resolved against the payload's
    // copy and died before it linked anything:
    //
    //     dyld: Symbol not found: __ZdaPv
    //       Referenced from: /Applications/Xcode_*.app/.../usr/bin/ld
    //       Expected in:     …/registry/…/lib/libc++.1.0.dylib
    //
    // cmake, bazel and the reference mcpp all failed identically while the mcpp
    // UNDER TEST passed on the same runner — which is the proof that mcpp does
    // not pass these flags on macOS either. The payload clang already knows
    // where its own libc++ is, and cmake supplies `-isysroot` itself.
    if (platform::OS_NAME == "macos") return f;

    // clang elsewhere: an explicit libc++ chain rather than --sysroot, which is
    // what mcpp itself drives clang with. Handing clang gcc's sysroot is the
    // mirror of the gcc branch's bug — one arm on the payload libc, the other
    // on the host's.
    const std::string ver{on_windows() ? kLlvmWindows : kLlvm};
    const auto root = xpkgs / "xim-x-llvm" / ver;
    if (std::filesystem::is_directory(root / "include" / "c++" / "v1", ec)) {
        f.compile += " --no-default-config -nostdinc++"
                     " -isystem" + (root / "include" / "c++" / "v1").string();
        // ⚠️ AND THE PER-TRIPLE DIRECTORY, which is where `__config_site` lives.
        // libc++'s `__config` includes it, so without this every TU dies with
        //
        //     __config:13:10: fatal error: '__config_site' file not found
        //
        // pointing inside the standard library rather than at a missing flag.
        // hermetic_payload.cmake globs for it; this port dropped that line.
        for (const auto& d : std::filesystem::directory_iterator(root / "include", ec)) {
            const auto cand = d.path() / "c++" / "v1";
            if (std::filesystem::is_directory(cand, ec))
                f.compile += " -isystem" + cand.string();
        }
        // ⚠️ AND THE PER-TRIPLE lib DIRECTORY. In this payload libc++ lives in
        // `lib/x86_64-unknown-linux-gnu/`, not `lib/`. A clang DRIVER finds it
        // by itself, which is why the cmake arm worked with `-L…/lib` alone —
        // but an engine that links through a different driver does not, and
        // xmake failed with
        //
        //     ld: cannot find -lc++: No such file or directory
        //
        // Naming both directories makes the flags independent of who links.
        // ⚠️ -L TELLS THE LINKER; -rpath TELLS THE LOADER. They are different
        // questions and this payload needs both answered: libc++ lives inside
        // the registry, nowhere the dynamic loader looks by default. With only
        // -L, every engine on macOS produced a binary that linked cleanly and
        // then died the moment it ran:
        //
        //     dyld: Symbol not found: __ZdaPv        (operator delete[])
        //
        // cmake, mcpp and bazel all failed identically, which is the tell that
        // it was the flags rather than any one engine.
        const auto add_libdir = [&](const std::filesystem::path& d) {
            f.link += " -L" + d.string() + " -Wl,-rpath," + d.string();
        };
        f.link += " -nostdlib++";
        add_libdir(root / "lib");
        for (const auto& d : std::filesystem::directory_iterator(root / "lib", ec)) {
            if (!d.is_directory()) continue;
            if (std::filesystem::exists(d.path() / "libc++.so", ec)   ||
                std::filesystem::exists(d.path() / "libc++.dylib", ec) ||
                std::filesystem::exists(d.path() / "libc++.a", ec))
                add_libdir(d.path());
        }
        f.link += " -lc++ -lc++abi";
    }
    return f;
}

}  // namespace bench::toolchain
