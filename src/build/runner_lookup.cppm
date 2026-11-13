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

export module mcpp.build.runner_lookup;

import std;
import mcpplibs.os.platform;

export namespace mcpp::build::runner_lookup {

struct Lookup {
    std::optional<std::filesystem::path> program;   // absolute, executable
    std::vector<std::filesystem::path>   searched;  // in order, for the message
};

namespace detail {
inline bool executable_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    if constexpr (mcpplibs::os::is_windows) return true;
    auto perms = std::filesystem::status(p, ec).permissions();
    using P = std::filesystem::perms;
    return (perms & (P::owner_exec | P::group_exec | P::others_exec)) != P::none;
}
} // namespace detail

// The directories one installed payload contributes to `locate`, in order:
// `<root>/bin`, then `<root>` itself.
//
// `bin/` is the convention and stays first. It is not universal: `xim:7zip`
// installs `7zz` straight into its payload directory, and so did the first
// version of `xim:apple-simulator-tools` -- which is how the second entry was
// measured, on a macOS runner with that package correctly installed:
//
//   error: runner 'simctl-run' for 'aarch64-ios-sim' was not found on any
//          search path.
//   Searched: .../xim-x-apple-simulator-tools/0.1.0/bin
//
// The directory searched was right and the program was one level up. Two
// directories per payload is cheaper than a layout rule every recipe has to
// know, and a recipe that uses `bin/` is unaffected because that entry is
// still tried first. A function rather than two `push_back`s at the call site
// so that the order is a stated rule a unit test can hold.
inline std::vector<std::filesystem::path>
payload_search_dirs(const std::filesystem::path& payloadRoot) {
    return { payloadRoot / "bin", payloadRoot };
}

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
    // A bare name on Windows is a program with the system's suffix, as the
    // shell finds it: `okrun` is `okrun.exe`.
    auto in_dir = [&](const std::filesystem::path& d) -> std::optional<std::filesystem::path> {
        if (auto c = d / a0; detail::executable_file(c)) return c;
        if constexpr (mcpplibs::os::is_windows) {
            if (!a0.has_extension())
                for (auto ext : { ".exe", ".cmd", ".bat" })
                    if (auto c = d / (a0.string() + ext); detail::executable_file(c)) return c;
        }
        return std::nullopt;
    };
    for (auto const& d : depBinDirs) {
        out.searched.push_back(d);
        if (auto c = in_dir(d)) { out.program = *c; return out; }
    }
    constexpr char sep = mcpplibs::os::is_windows ? ';' : ':';
    for (auto part : std::views::split(pathEnv, sep)) {
        std::string_view sv(part.begin(), part.end());
        if (sv.empty()) continue;
        std::filesystem::path d(sv);
        out.searched.push_back(d);
        if (auto c = in_dir(d)) { out.program = *c; return out; }
    }
    return out;
}

// What the kernel's refusal means. Only ENOEXEC says "this host cannot load
// the artifact": mcpp.run reports every refusal of that kind under it (a file
// that is no image, and an image built for another processor, EBADARCH on
// macOS); everything else — EACCES, ENOENT on a found program — is reported
// verbatim and never turned into advice about runners.
enum class SpawnClass { Unloadable, Other };

inline SpawnClass classify(int e) {
    if (e == static_cast<int>(std::errc::executable_format_error)) return SpawnClass::Unloadable;
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

// `detail` is the system's own account (`RunResult::spawn_detail`), which
// names what the classification `e` does not.
inline std::string spawn_failed_message(std::string_view program, int e,
                                        std::string_view detail = {}) {
    return std::format("'{}' could not be started: {} (error {}){}{}",
                       program, errno_text(e), e, detail.empty() ? "" : "; ", detail);
}

// The hosted sibling of mcpp::freestanding::no_runner_message. It reports what
// the kernel answered rather than asserting why: on a hosted triple mcpp does
// not know whether the refusal is a foreign ISA or a file that is not an
// executable at all. The declaration it prints is the key's shape; which
// program runs the artifact is the statement of a provider, a platform or the
// project (SPEC-016 §6), and no program is named here.
inline std::string unrunnable_message(std::string_view triple,
                                      const std::filesystem::path& artifact, int e) {
    return std::format(
        "this host cannot execute '{}': {} (error {}).\n"
        "       The artifact was built for '{}'. Declare how to run it here:\n"
        "\n"
        "           [target.{}]\n"
        "           runner = [\"<program>\", \"{{}}\"]\n"
        "\n"
        "       The artifact path is appended, or substituted for `{{}}` if the "
        "template contains it.\n"
        "       A host that can execute it directly may pass --no-runner.",
        artifact.string(), errno_text(e), e, triple, triple);
}

// THE PROGRAM A SCRIPT'S `#!` LINE HANDS IT TO (#621).
//
// An artefact the run slot executes directly can be a script: emsdk's
// JavaScript launcher begins `#!/usr/bin/env node`. When that program is not
// there, the refusal comes after mcpp has handed the process over -- from the
// kernel, or from `env` once it has started (`/usr/bin/env: 'node': No such
// file or directory`) -- so none of the messages above applies. The line is
// read as the kernel reads it: the interpreter is the first word after `#!`
// and the rest is its argument; through `env`, that argument names the program
// `env` looks up on PATH. A form this does not read with certainty (an
// interpreter that is not an absolute path, an `env` option other than `-S`,
// `env` with no program) names no program, so a note built on it never names
// the wrong one.
struct ScriptInterpreter {
    std::string           line;      // the `#!` line, without its line end
    std::filesystem::path program;   // what the kernel executes
    std::string           viaEnv;    // what `program`, an `env`, looks up on PATH; empty otherwise
};

inline std::optional<ScriptInterpreter> parse_script_interpreter(std::string_view head) {
    if (!head.starts_with("#!")) return std::nullopt;
    head = head.substr(0, head.find_first_of("\r\n"));
    ScriptInterpreter out;
    out.line = std::string(head);
    constexpr std::string_view blanks = " \t";
    auto next_word = [&](std::string_view& rest) {
        const auto from = std::min(rest.find_first_not_of(blanks), rest.size());
        rest.remove_prefix(from);
        const auto to = std::min(rest.find_first_of(blanks), rest.size());
        auto word = rest.substr(0, to);
        rest.remove_prefix(to);
        return word;
    };
    std::string_view rest = head.substr(2);
    const auto program = next_word(rest);
    if (!program.starts_with('/')) return std::nullopt;
    out.program = std::filesystem::path(program);
    if (out.program.filename() != "env") return out;
    // `env [-S] [NAME=value]... <program> [<argument>]...`
    for (auto word = next_word(rest); !word.empty(); word = next_word(rest)) {
        if (word == "-S" || word == "--split-string") continue;
        if (word.starts_with('-')) return std::nullopt;
        if (word.find('=') != std::string_view::npos) continue;
        out.viaEnv = std::string(word);
        return out;
    }
    return std::nullopt;
}

// The artefact's own `#!` line; none on Windows, which does not read one.
inline std::optional<ScriptInterpreter> script_interpreter(const std::filesystem::path& artifact) {
    if constexpr (mcpplibs::os::is_windows) {
        return std::nullopt;
    } else {
        std::ifstream in(artifact, std::ios::binary);
        if (!in) return std::nullopt;
        // The kernel reads no more of the line than this (BINPRM_BUF_SIZE).
        std::string head(256, '\0');
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(in.gcount()));
        return parse_script_interpreter(head);
    }
}

// What `mcpp run` and `mcpp test` say before they execute `artifact` directly
// when the program its `#!` line names is not there; empty when it is, and
// when the artefact is no script this reads. `pathEnv` is the PATH the
// artefact receives, the one `env` searches.
//
// The remedy is the one an emsdk payload installed before its recipe wrote
// `.mcpp-toolchain.json` needs (docs/20, "Running what they produce"). The
// note names no toolchain: it reads a file's first line, and which payload
// built the file is not a fact that line holds.
inline std::string missing_interpreter_note(const std::filesystem::path& artifact,
                                            std::string_view pathEnv) {
    auto script = script_interpreter(artifact);
    if (!script) return {};
    std::string missing;
    if (!detail::executable_file(script->program))
        missing = std::format("'{}' is not an executable file", script->program.string());
    else if (!script->viaEnv.empty() && !locate(script->viaEnv, {}, pathEnv).program)
        missing = std::format("'{}' is not on PATH", script->viaEnv);
    if (missing.empty()) return {};
    return std::format(
        "'{}' runs through its first line, `{}`, and {}.\n"
        "      A toolchain payload names the program that runs its artefacts in\n"
        "      `.mcpp-toolchain.json`; one installed before its recipe wrote that file\n"
        "      names none, and its artefacts run through their `#!` line.\n"
        "      fix: `mcpp index update`, then `mcpp toolchain remove <spec>` and\n"
        "      `mcpp toolchain install <spec>` for the toolchain that built it.",
        artifact.string(), script->line, missing);
}

} // namespace mcpp::build::runner_lookup
