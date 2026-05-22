// mcpp.fallback.sysroot_complete — ensure sysroot has complete headers.
//
// When GCC's probed sysroot exists but may be missing linux kernel
// headers or glibc headers, symlink them from the payload xpkgs.

export module mcpp.fallback.sysroot_complete;

import std;
import mcpp.toolchain.model;

export namespace mcpp::fallback {

// Ensure sysroot directory has complete headers by symlinking from
// payload xpkgs. Called when GCC's probed sysroot exists but may
// be missing linux kernel headers or glibc headers.
void ensure_sysroot_complete(const std::filesystem::path& sysroot,
                             const mcpp::toolchain::PayloadPaths& pp) {
    if (sysroot.empty()) return;

    auto sysrootInclude = sysroot / "usr" / "include";
    if (!std::filesystem::exists(sysrootInclude)) return;

    std::error_code ec;

    // Ensure linux kernel headers are present in sysroot.
    // If missing, symlink from linux-headers payload.
    if (!pp.linuxInclude.empty()) {
        for (auto dir : {"linux", "asm", "asm-generic"}) {
            auto target = sysrootInclude / dir;
            auto source = pp.linuxInclude / dir;
            if (!std::filesystem::exists(target, ec) && std::filesystem::exists(source, ec)) {
                std::filesystem::create_directory_symlink(source, target, ec);
            }
        }
    }

    // Ensure glibc headers are present if sysroot is bare.
    if (!std::filesystem::exists(sysrootInclude / "features.h", ec)) {
        // Symlink individual glibc dirs/files into sysroot.
        for (auto& entry : std::filesystem::directory_iterator(pp.glibcInclude, ec)) {
            auto target = sysrootInclude / entry.path().filename();
            if (!std::filesystem::exists(target, ec)) {
                if (entry.is_directory(ec))
                    std::filesystem::create_directory_symlink(entry.path(), target, ec);
                else
                    std::filesystem::create_symlink(entry.path(), target, ec);
            }
        }
    }
}

} // namespace mcpp::fallback
