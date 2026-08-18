// mcpp.freestanding.linkline — the compile prefix and link line for a target
// that has no operating system under it.
//
// PURE STRING BUILDERS, ON PURPOSE
//
// Nothing here knows about `mcpp::build::CompileFlags`, the build plan, or the
// manifest. `mcpp.build.flags` imports this module and applies what it
// returns; this module importing that one back would be a cycle, and the
// cheapest way to guarantee it never becomes one is for the freestanding side
// to speak only in strings.
//
// WHY THE LINK LINE IS REPLACED RATHER THAN EXTENDED
//
// Every hosted link flag is wrong here, not merely unnecessary: the C runtime
// startup files, the dynamic linker, `-lstdc++`, the loader search paths.
// Appending `-nostdlib` to a line that already carries them relies on the
// driver dropping them in the right order. Building the line from nothing is
// the only form where "what ends up on the command" equals "what this module
// decided".
//
// ⚠️ THE LINKER IS ADDRESSED BY ABSOLUTE PATH
//
// `-fuse-ld=lld` resolves by NAME, and on a machine with binutils earlier on
// PATH it finds GNU ld, which then fails with
//
//     unrecognised emulation mode: elf64lriscv
//
// Reproduced 2026-08-19 on this very toolchain while building picolibc. A name
// is a request; a path is an instruction.

export module mcpp.freestanding.linkline;

import std;
import mcpp.freestanding.target;

export namespace mcpp::freestanding {

// Flags every translation unit of a freestanding build carries.
//
// `--target` is the load-bearing one and the reason this exists at all: clang
// is ONE binary that emits every target it was built with, so unlike the
// hosted cross toolchains (where a distinct `<triple>-g++` reports its own
// `-dumpmachine`) nothing about the driver's identity says which target is
// wanted. Omit it and the build silently produces host objects — measured, and
// the exact shape of the E1 defect this work exists to close.
inline std::string compile_prefix(const Spec& s) {
    std::string out;
    out += " --target=" + std::string(s.triple);
    for (auto const& f : compile_flags(s)) { out += ' '; out += f; }
    return out;
}

// Assembly goes through the C driver, which needs the same target selection
// but none of the C++ dialect flags.
inline std::string assemble_prefix(const Spec& s) { return compile_prefix(s); }

// What the link line must NOT do, spelled as flags:
//   -nostdlib      no C runtime, no default libraries
//   -nostartfiles  no crt0/crt1 — startup comes from a BSP, or nowhere
//   -static        there is no loader, so there is no other option
//
// The linker script and startup objects are NOT here: they are board facts, a
// BSP supplies them, and the engine only carries their ORDER (which is the one
// thing a `build.mcpp` cannot express — `link-search`/`link-lib` are unordered
// and there is no verbatim ldflag directive).
struct LinkInputs {
    std::filesystem::path lld;          // absolute path to ld.lld
    std::filesystem::path linkerScript; // "" = none (BSP-supplied when present)
    std::vector<std::filesystem::path> prologue;  // before the user's objects
    std::vector<std::filesystem::path> epilogue;  // after them
};

inline std::string link_flags(const Spec& s, const LinkInputs& in,
                              const std::function<std::string(
                                  const std::filesystem::path&)>& esc)
{
    std::string out;
    out += " --target=" + std::string(s.triple);
    for (auto const& f : compile_flags(s)) { out += ' '; out += f; }
    out += " -nostdlib -nostartfiles -static";
    if (!in.lld.empty())
        out += " -fuse-ld=" + esc(in.lld);
    if (!in.linkerScript.empty())
        out += " -T " + esc(in.linkerScript);
    return out;
}

// LLD inside the same payload as the driver.
//
// Derived from the driver's own path rather than searched for, because a
// search is exactly how the wrong linker gets picked. Returns "" when the
// payload has no LLD, and the caller must treat that as a hard error rather
// than falling back to `-fuse-ld=lld` — falling back is the failure.
inline std::filesystem::path resolve_lld(const std::filesystem::path& driver) {
    if (driver.empty()) return {};
    const auto bin = driver.parent_path();
    std::error_code ec;
    for (const char* name : { "ld.lld", "ld.lld.exe", "lld", "lld.exe" }) {
        auto p = bin / name;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

// Does this look like LLD? Checked against the linker's own `--version`
// output rather than its filename, because the filename is exactly what was
// wrong in the failure this guards.
inline bool version_output_is_lld(std::string_view versionText) {
    return versionText.find("LLD") != std::string_view::npos;
}

// The diagnostic for the case above. Its own function so the wording is
// asserted in one place and cannot drift between call sites.
inline std::string wrong_linker_message(const std::filesystem::path& linker,
                                        std::string_view versionText)
{
    std::string firstLine(versionText.substr(0, versionText.find('\n')));
    return std::format(
        "the linker at {} is not LLD (it reports: {})\n"
        "       a freestanding target needs LLD: GNU ld in an LLVM payload "
        "cannot emit\n"
        "       this target's object format and fails with "
        "'unrecognised emulation mode'.",
        linker.string(), firstLine.empty() ? "(no output)" : firstLine);
}

} // namespace mcpp::freestanding
