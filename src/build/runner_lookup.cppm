// mcpp.build.runner_lookup — where the runner's program is, and what to say
// when it is not.
//
// The lookup is mcpp's own rather than posix_spawnp's for one measured reason:
// a bare name on PATH resolves to an xvm shim, and the shim answers for the
// current subos rather than for the package (e2e 130 in CI: `mcpp run` exec'ing
// the bare `qemu-system-riscv64` answered "not installed" while
// `qemu-system-riscv64 --version` in the same job succeeded; `python3` on the
// development host answers "not installed in this subos (_) — installed
// elsewhere"). The payload's bin/ is the binary itself, so it is searched
// first. That is what lets a runner name a program the project declared under
// `[xlings] deps` without writing the payload's home-and-version path into the
// manifest — the thing BuildConfig::runner's comment says a static manifest
// cannot do.
//
// Doing the lookup here has a second effect: "not found anywhere" is decided
// before any spawn, so a spawn-time ENOENT can only mean the program was found
// and its interpreter or loader was not. The two messages differ, and neither
// guesses.
//
// Design: .agents/docs/2026-09-02-runner-beyond-baremetal-design.md §4.3-4.4.

module;
#include <cerrno>

export module mcpp.build.runner_lookup;

import std;
import mcpp.platform;

export namespace mcpp::build::runner_lookup {

struct Lookup {
    std::optional<std::filesystem::path> program;   // absolute, executable
    std::vector<std::filesystem::path>   searched;  // in order, for the message
};

namespace detail {
inline bool executable_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    if constexpr (mcpp::platform::is_windows) return true;
    auto perms = std::filesystem::status(p, ec).permissions();
    using P = std::filesystem::perms;
    return (perms & (P::owner_exec | P::group_exec | P::others_exec)) != P::none;
}
} // namespace detail

// `argv0` absolute, or containing a directory separator: taken as-is when it
// is an executable file. Otherwise `<each depBinDir>/argv0`, then each `PATH`
// entry (`pathEnv` split on the platform's list separator); the first
// executable regular file wins. Every directory looked in is recorded so the
// not-found message can list them.
inline Lookup locate(std::string_view argv0,
                     std::span<const std::filesystem::path> depBinDirs,
                     std::string_view pathEnv)
{
    Lookup out;
    std::filesystem::path a0(argv0);
    const bool hasDir = a0.is_absolute()
                     || argv0.find('/') != std::string_view::npos
                     || argv0.find('\\') != std::string_view::npos;
    if (hasDir) {
        std::error_code ec;
        if (detail::executable_file(a0)) out.program = std::filesystem::absolute(a0, ec);
        out.searched.push_back(a0.parent_path());
        return out;
    }
    for (auto const& d : depBinDirs) {
        out.searched.push_back(d);
        if (auto c = d / a0; detail::executable_file(c)) { out.program = c; return out; }
    }
    constexpr char sep = mcpp::platform::is_windows ? ';' : ':';
    for (auto part : std::views::split(pathEnv, sep)) {
        std::string_view sv(part.begin(), part.end());
        if (sv.empty()) continue;
        std::filesystem::path d(sv);
        out.searched.push_back(d);
        if (auto c = d / a0; detail::executable_file(c)) { out.program = c; return out; }
    }
    return out;
}

// What the kernel's refusal means. Only ENOEXEC (and EBADARCH where the
// platform defines it) says "this host cannot load the artifact"; everything
// else — EACCES, ENOENT on a found program, E2BIG — is reported verbatim and
// never turned into advice about runners.
enum class SpawnClass { Unloadable, Other };

inline SpawnClass classify(int e) {
    if (e == ENOEXEC) return SpawnClass::Unloadable;
#if defined(EBADARCH)
    if (e == EBADARCH) return SpawnClass::Unloadable;
#endif
    return SpawnClass::Other;
}

inline std::string errno_text(int e) {
    return std::generic_category().message(e);
}

inline std::string not_found_message(std::string_view triple, std::string_view argv0,
                                     std::span<const std::filesystem::path> searched) {
    // The first line stands on its own: `mcpp test` repeats it in its summary.
    std::string dirs;
    for (auto const& d : searched) dirs += "\n           " + d.string();
    return std::format(
        "runner '{}' for '{}' was not found on any search path.\n"
        "       Searched:{}\n"
        "       Declare the package that provides it under [xlings.workspace], or "
        "install it on PATH.\n"
        "       Pass --no-runner to execute the artifact directly on this host.",
        argv0, triple, dirs);
}

inline std::string spawn_failed_message(std::string_view program, int e) {
    return std::format("'{}' could not be started: {} (error {})",
                       program, errno_text(e), e);
}

// The hosted sibling of mcpp::freestanding::no_runner_message. It reports what
// the kernel answered rather than asserting why: on a hosted triple mcpp does
// not know whether the refusal is a foreign ISA or a file that is not an
// executable at all, and the example it prints is a user-mode emulator.
inline std::string unrunnable_message(std::string_view triple,
                                      const std::filesystem::path& artifact, int e) {
    return std::format(
        "this host cannot execute '{}': {} (error {}).\n"
        "       The artifact was built for '{}'. Declare how to run it here:\n"
        "\n"
        "           [target.{}]\n"
        "           runner = [\"qemu-aarch64-static\"]\n"
        "\n"
        "       The artifact path is appended, or substituted for `{{}}` if the "
        "template contains it.\n"
        "       A host that can execute it directly may pass --no-runner.",
        artifact.string(), errno_text(e), e, triple, triple);
}

} // namespace mcpp::build::runner_lookup
