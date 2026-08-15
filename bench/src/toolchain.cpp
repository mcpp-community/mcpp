// bench.toolchain — implementation.
//
// `module bench.toolchain;` with no `export`: an implementation unit, so nothing below
// reaches an importer's BMI. The pinned version constants stay in the interface —
// they are `constexpr` and callers use them at compile time.
module bench.toolchain;

import std;
import bench.platform;

namespace bench::toolchain {

bool on_windows() { return platform::OS_NAME == "windows"; }

bool is_clang_request(std::string_view compiler) {
    return compiler.find("clang") != std::string_view::npos
        || compiler.find("llvm")  != std::string_view::npos;
}

bool resolves_to_clang(std::string_view compiler) {
    return is_clang_request(compiler) || platform::OS_NAME != "linux";
}

std::string mcpp_pin(std::string_view compiler) {
    if (resolves_to_clang(compiler))
        return std::format("llvm@{}", on_windows() ? kLlvmWindows : kLlvm);
    return std::format("gcc@{}", kGcc);
}

std::filesystem::path registry_xpkgs() {
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

Resolved payload_cxx(std::string_view compiler) {
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

PayloadFlags payload_flags(std::string_view compiler) {
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
