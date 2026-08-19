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
    // Where the TARGET's C library lives. Supplied by the engine from the
    // target's own row, so a board package selects libraries by bare name
    // (`-lc`, `-lcrt0-semihost`) without knowing which libc it is or where it
    // landed. "" = this target has no sysroot of its own.
    std::filesystem::path sysrootLib;
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
    // ⚠️ BEFORE the libraries the board selects, and it has to be on THIS line
    // rather than in the generic ld flags: a freestanding link line is
    // REPLACED wholesale (the payload cfg would otherwise inject a host
    // dynamic linker), so anything appended to the ordinary ldflags earlier is
    // discarded. Measured — the -L was built, and then silently was not there.
    if (!in.sysrootLib.empty())
        out += " -L" + esc(in.sysrootLib);
    if (!in.linkerScript.empty())
        out += " -T " + esc(in.linkerScript);
    return out;
}

// The link map, for the question only a map can answer on a bare-metal target:
// why a section is where it is, and why something did or did not get pulled in.
//
// A flag on the link rather than a separate edge, because the linker is the
// only thing that can produce it and it does so as a side effect of the link
// it is already doing.
inline std::string map_flag(const std::filesystem::path& artifact,
                            const std::function<std::string(
                                const std::filesystem::path&)>& esc)
{
    auto m = artifact; m += ".map";
    return " -Wl,-Map=" + esc(m);
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

// llvm-objcopy inside the same payload as the driver, for the flat image a
// flasher takes. Derived from the driver's path for the same reason
// `resolve_lld` is: a search is how the wrong tool gets picked. Returns "" for
// a hosted target — a loader wants the ELF, so there is nothing to convert.
inline std::filesystem::path
resolve_objcopy(const std::filesystem::path& driver, std::string_view triple) {
    if (driver.empty()) return {};
    if (!resolve(triple)) return {};      // not a bare-metal target we know
    const auto bin = driver.parent_path();
    std::error_code ec;
    for (const char* name : { "llvm-objcopy", "llvm-objcopy.exe",
                              "objcopy", "objcopy.exe" }) {
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

// ── Size summary ───────────────────────────────────────────────────────────
//
// The core constraint on a bare-metal target is CAPACITY, and mcpp already
// knows the number the moment the link finishes. Not printing it means every
// user runs `size` themselves — and the ones who do not find out the image no
// longer fits when the flasher refuses it.
//
// Parsed rather than passed through so the shape is mcpp's, not the tool's:
// `llvm-size` and GNU `size` differ in their headers, and a build that printed
// one tool's table would change appearance with the toolchain.
struct SizeSummary { long long text = 0, data = 0, bss = 0; };

inline long long size_total(const SizeSummary& s) { return s.text + s.data + s.bss; }

// Parse the Berkeley-format second line: "   text	   data	    bss	    dec …".
inline std::optional<SizeSummary> parse_size_output(std::string_view out) {
    std::size_t nl = out.find('\n');
    if (nl == std::string_view::npos) return std::nullopt;
    auto row = out.substr(nl + 1);
    SizeSummary s;
    int got = 0;
    long long cur = 0; bool in = false;
    for (char c : row) {
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); in = true; continue; }
        if (in) {
            if (got == 0) s.text = cur;
            else if (got == 1) s.data = cur;
            else if (got == 2) { s.bss = cur; return s; }
            ++got; cur = 0; in = false;
        }
        if (c == '\n') break;
    }
    if (in && got == 2) { s.bss = cur; return s; }
    return std::nullopt;
}

// `llvm-size` beside the driver, same derivation as the linker and objcopy.
inline std::filesystem::path resolve_size_tool(const std::filesystem::path& driver) {
    if (driver.empty()) return {};
    const auto bin = driver.parent_path();
    std::error_code ec;
    for (const char* name : { "llvm-size", "llvm-size.exe", "size", "size.exe" }) {
        auto p = bin / name;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

} // namespace mcpp::freestanding
