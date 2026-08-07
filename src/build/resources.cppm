// mcpp.build.resources — Windows resource scripts: synthesise one, read one,
// and find the tool that compiles it (mcpp#365).
//
// WHY THIS IS ITS OWN MODULE
//
// Everything here is knowledge about the `.rc` FORMAT and about the resource
// compilers, and none of it is knowledge about the build graph. prepare.cppm
// decides which units exist and ninja_backend.cppm spells the edge; this file
// answers "what goes in the file", "what does the file depend on", and "which
// binary can compile it".
//
// THE BUG THAT SHAPES ALL OF IT
//
// mcpp#365 reported that an embedded VERSIONINFO is invisible to Windows —
// `llvm-readobj --coff-resources` shows the resource, `GetFileVersionInfo`
// returns nothing — and attributed it to llvm-rc. It is not an llvm-rc bug.
// `VS_VERSION_INFO` is a macro from `verrsrc.h` (via `windows.h`) whose value is
// 1. Without that definition, rc grammar happily accepts the bare identifier in
// the resource-NAME position, so the resource is filed under the string name
// "VS_VERSION_INFO" instead of ordinal 1. The type is RT_VERSION(16) either way
// — which is exactly why every tool that prints the type says it looks right —
// but GetFileVersionInfo looks up MAKEINTRESOURCE(VS_VERSION_INFO), i.e. the
// ORDINAL, and finds nothing.
//
// Measured on llvm-rc 22.1.8. In the emitted `.res`, the type and name fields
// of the first real resource header sit at offset 40 (a 32-byte null header,
// then dataSize + headerSize); `ffff` introduces an ordinal:
//
//   1 VERSIONINFO                 ff ff 10 00  ff ff 01 00     (480 bytes)
//   VS_VERSION_INFO VERSIONINFO   ff ff 10 00  56 00 53 00 …   (508 bytes)
//              type = RT_VERSION ─┘            ^ UTF-16 "VS_VERSION_INFO"
//
// Consequences, all of them visible below:
//   * The script we synthesise writes the literal `1`. Correct by construction,
//     no toolchain workaround, nothing to keep in sync with a Windows SDK.
//   * An author-written `.rc` gets the target's include directories, so
//     `#include <windows.h>` resolves and the macro is real. We do NOT inject
//     `-DVS_VERSION_INFO=1`: partially re-implementing the SDK's macros is a
//     second source of truth that drifts (the reporter also needed
//     VOS_NT_WINDOWS32 and VFT_APP).
//   * `ScanResult::versionInfoNamedByString` catches the exact silent shape in
//     an author-written script anyway, because a user who hits it has no way to
//     diagnose it from the outside.

export module mcpp.build.resources;

import std;
import mcpp.manifest;
import mcpp.toolchain.detect;
import mcpp.toolchain.triple;
import mcpp.version_req;

export namespace mcpp::build::resources {

// ─── The resource compiler ────────────────────────────────────────────────

struct RcTool {
    std::filesystem::path path;
    // "gnu"  → windres: emits a COFF object, because GNU ld cannot consume a
    //          `.res`. Also preprocesses through its own matching `<triple>-gcc`,
    //          so it inherits the target's default include path.
    // "msvc" → rc.exe / llvm-rc: emits a `.res`, which link.exe and lld-link
    //          take directly. llvm-rc preprocesses by default and accepts
    //          /I and /D (measured; there is no depfile option).
    std::string           style;
    std::string           name() const { return path.filename().string(); }
};

// Find the resource compiler for `tc`, searching PAYLOAD-RELATIVE locations
// only — never the host PATH.
//
// PATH is not an option here, and not for tidiness: xlings' shim mechanism
// gives the last installer of a bare name ownership of it, and
// `…/registry/subos/default/bin/x86_64-w64-mingw32-windres` is today a symlink
// to the xlings dispatcher. Resolving a build tool through a mutable global
// namespace is how a cross toolchain once silently produced ARM objects while
// every version probe answered correctly. `clang::find_scan_deps` sets the
// precedent: look next to the compiler that is actually being used.
std::optional<RcTool> find_rc_tool(const mcpp::toolchain::Toolchain& tc,
                                   std::string_view dialectId);

// Split a Windows environment list (PATH, INCLUDE, LIB) into its entries.
//
// `;` is the ONLY separator, and that is not a simplification. Every value that
// reaches here was synthesised by the MSVC backend for a Windows host, where an
// entry routinely begins `C:\`. Splitting on `find_first_of(";:")` cuts at the
// DRIVE COLON: `C:\Windows Kits\10\bin\...;C:\...` becomes `C` plus a
// current-drive-relative tail, which resolves by accident when the build sits
// on the same drive and silently finds nothing otherwise. One function because
// two callers (the rc-tool search here, the llvm-rc include list in
// prepare.cppm) were deriving the same rule independently and had already
// disagreed about it.
std::vector<std::string_view> split_env_list(std::string_view value);

// ─── Reading an author-written .rc ────────────────────────────────────────

struct ScanResult {
    // Quoted `#include`s and the data files named by resource statements,
    // resolved against the .rc's directory. Angled includes are deliberately
    // absent: `<windows.h>` belongs to the toolchain, which is immutable for
    // the life of a build directory and already folded into the fingerprint.
    std::vector<std::filesystem::path> inputs;
    // Operands the scan could not turn into a path — almost always a file name
    // reached through a macro (`1 ICON APP_ICON`). Named rather than dropped:
    // bounding coverage silently is how a stale resource survives in a shipped
    // binary. The remedy is `[resources].extra-inputs`.
    std::vector<std::string>           gaps;
    // The mcpp#365 shape: a VERSIONINFO whose name is an identifier that is not
    // the literal `1`, in a file that defines no such macro and includes
    // nothing that could. See the module header.
    bool                               versionInfoNamedByString = false;
    std::string                        versionInfoName;
};

ScanResult scan_rc(const std::filesystem::path& rc);

// ─── Synthesising the common case ─────────────────────────────────────────

// Build the `.rc` text for `[resources]`: an icon at ordinal 1 and/or a
// VERSIONINFO at ordinal 1, with every string defaulted from `[package]`.
//
// `iconAbs` is empty when no icon was declared; `outputFileName` is the produced
// artifact's file name (VERSIONINFO's OriginalFilename). Fails only when the
// package version cannot be expressed as FILEVERSION's four 16-bit fields —
// clamping silently would put a version in the binary that is not the version
// that was built.
std::expected<std::string, std::string>
synthesize_rc(const mcpp::manifest::Package&   pkg,
              const mcpp::manifest::Resources& res,
              std::string_view                 outputFileName,
              const std::filesystem::path&     iconAbs);

} // namespace mcpp::build::resources

namespace mcpp::build::resources {

namespace {

bool exists_file(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// Candidate tool names, most specific first. The triple-prefixed windres is the
// one that preprocesses with the matching cross gcc, so it must win over a bare
// `windres` that might belong to the host.
std::vector<std::string> gnu_candidates(std::string_view triple) {
    std::vector<std::string> out;
    if (!triple.empty()) {
        out.push_back(std::string(triple) + "-windres");
        // GCC cross payloads spell the triple with a vendor field
        // (x86_64-w64-mingw32) that mcpp's canonical form drops.
        auto t = mcpp::toolchain::triple::parse(triple);
        if (t && t->os == "windows" && t->env == "gnu")
            out.push_back(t->arch + "-w64-mingw32-windres");
    }
    out.push_back("windres");
    out.push_back("llvm-windres");
    return out;
}

std::optional<std::filesystem::path>
probe_dir(const std::filesystem::path& dir, const std::vector<std::string>& names) {
    if (dir.empty()) return std::nullopt;
    for (auto const& n : names) {
        for (auto const& ext : {"", ".exe"}) {
            auto p = dir / (n + ext);
            if (exists_file(p)) return p;
        }
    }
    return std::nullopt;
}

// A run of `[0-9A-Za-z_]` starting at `i`.
std::string_view word_at(std::string_view s, std::size_t i) {
    std::size_t j = i;
    while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
    return s.substr(i, j - i);
}

// Resource statements whose operand is a FILE. Anything else (STRINGTABLE,
// DIALOG, MENU, ACCELERATORS) carries its data inline.
bool is_file_resource_keyword(std::string_view w) {
    return w == "ICON" || w == "BITMAP" || w == "CURSOR" || w == "FONT"
        || w == "RCDATA" || w == "MESSAGETABLE" || w == "TYPELIB"
        || w == "MANIFEST" || w == "HTML" || w == "ANICURSOR" || w == "ANIICON";
}

std::string escape_rc_string(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n' || c == '\r') { out += ' '; continue; }
        out += c;
    }
    return out;
}

} // namespace

std::vector<std::string_view> split_env_list(std::string_view value) {
    std::vector<std::string_view> out;
    while (!value.empty()) {
        const auto sep = value.find(';');
        if (auto entry = value.substr(0, sep); !entry.empty())
            out.push_back(entry);
        if (sep == std::string_view::npos) break;
        value = value.substr(sep + 1);
    }
    return out;
}

std::optional<RcTool> find_rc_tool(const mcpp::toolchain::Toolchain& tc,
                                   std::string_view dialectId) {
    const auto compilerDir = tc.binaryPath.parent_path();

    if (dialectId == "msvc") {
        // rc.exe comes from the Windows SDK, which the MSVC backend surfaces on
        // the toolchain's own PATH override (never the host's). llvm-rc ships
        // beside clang and is the fallback for clang + lld-link.
        //
        // The PATH walk is the PRIMARY route, not a fallback: rc.exe lives in
        // the SDK's own bin directory, never next to cl.exe, so the probe above
        // answers only for llvm-rc.
        const std::vector<std::string> names = {"rc", "llvm-rc"};
        if (auto p = probe_dir(compilerDir, names)) return RcTool{*p, "msvc"};
        for (auto const& ev : tc.envOverrides) {
            if (ev.key != "PATH" && ev.key != "Path") continue;
            for (auto dir : split_env_list(ev.value))
                if (auto p = probe_dir(std::filesystem::path(dir), names))
                    return RcTool{*p, "msvc"};
        }
        return std::nullopt;
    }

    const auto names = gnu_candidates(tc.targetTriple);
    if (auto p = probe_dir(compilerDir, names)) return RcTool{*p, "gnu"};
    // Cross payloads keep binutils in a sibling <triple>/bin.
    if (!tc.targetTriple.empty()) {
        auto root = compilerDir.parent_path();
        if (auto p = probe_dir(root / tc.targetTriple / "bin", names))
            return RcTool{*p, "gnu"};
    }
    return std::nullopt;
}

ScanResult scan_rc(const std::filesystem::path& rc) {
    ScanResult out;
    std::ifstream is(rc, std::ios::binary);
    if (!is) return out;
    std::string body{std::istreambuf_iterator<char>(is), {}};
    const auto dir = rc.parent_path();

    bool sawInclude = false, definesVersionInfoMacro = false;
    // First pass: does anything in this file make VS_VERSION_INFO real?
    // `#include <windows.h>` (or any include — we cannot follow it, and a file
    // that includes something has plausibly included the right thing) or an
    // explicit `#define`.
    for (std::size_t i = 0; i + 1 < body.size(); ++i) {
        if (body[i] != '#') continue;
        auto w = word_at(body, i + 1);
        if (w == "include") sawInclude = true;
        if (w == "define") {
            auto j = i + 1 + w.size();
            while (j < body.size() && (body[j] == ' ' || body[j] == '\t')) ++j;
            if (word_at(body, j) == "VS_VERSION_INFO") definesVersionInfoMacro = true;
        }
    }

    auto add_input = [&](std::string_view raw) {
        std::filesystem::path p{std::string(raw)};
        auto abs = p.is_absolute() ? p : dir / p;
        if (std::find(out.inputs.begin(), out.inputs.end(), abs) == out.inputs.end())
            out.inputs.push_back(std::move(abs));
    };

    // Line-oriented: rc statements do not span lines in any form that matters
    // here, and a line-based reader keeps the "what did I fail to understand"
    // reporting precise.
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const auto nl = body.find('\n', pos);
        std::string_view line{body.data() + pos,
                              (nl == std::string::npos ? body.size() : nl) - pos};
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;

        // Strip a trailing `//` comment; `/* */` is rare in .rc and a partial
        // strip would be worse than none.
        if (auto c = line.find("//"); c != std::string_view::npos) line = line.substr(0, c);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.empty()) continue;

        if (line.starts_with("#include")) {
            auto q = line.find('"');
            if (q != std::string_view::npos) {
                auto e = line.find('"', q + 1);
                if (e != std::string_view::npos) add_input(line.substr(q + 1, e - q - 1));
            }
            continue;   // angled includes are the toolchain's, on purpose
        }
        if (line.front() == '#') continue;

        // `<name> <TYPE> <operand>` — find the type keyword by scanning words.
        std::size_t i = 0;
        std::string_view prevWord;
        while (i < line.size()) {
            if (!(std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) { ++i; continue; }
            auto w = word_at(line, i);
            if (w.empty()) { ++i; continue; }

            if (w == "VERSIONINFO" && !prevWord.empty()) {
                // The mcpp#365 shape: an identifier name that is not `1`.
                const bool numeric = std::all_of(prevWord.begin(), prevWord.end(),
                    [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
                if (!numeric && !sawInclude && !definesVersionInfoMacro) {
                    out.versionInfoNamedByString = true;
                    out.versionInfoName = std::string(prevWord);
                }
            }
            if (is_file_resource_keyword(w)) {
                auto rest = line.substr(i + w.size());
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                    rest.remove_prefix(1);
                if (rest.starts_with('"')) {
                    auto e = rest.find('"', 1);
                    if (e != std::string_view::npos) add_input(rest.substr(1, e - 1));
                } else if (!rest.empty() && rest.front() != '{'
                           && !rest.starts_with("BEGIN")) {
                    // A macro, or a form the scanner does not model. Say so.
                    auto tok = word_at(rest, 0);
                    if (!tok.empty())
                        out.gaps.push_back(std::format("{} {}", w, tok));
                }
            }
            prevWord = w;
            i += w.size();
        }
    }
    return out;
}

std::expected<std::string, std::string>
synthesize_rc(const mcpp::manifest::Package&   pkg,
              const mcpp::manifest::Resources& res,
              std::string_view                 outputFileName,
              const std::filesystem::path&     iconAbs) {
    std::string out;
    // ASCII throughout, including this banner: see the LegalCopyright note
    // below for why generated text must not lean on the codepage flag.
    out += "// Generated by mcpp from [package] and [resources]. Do not edit.\n";
    out += "// Copy this file into your project and list it in\n";
    out += "// [resources] files = [...] to take it over; the result is\n";
    out += "// byte-identical.\n\n";

    if (!iconAbs.empty()) {
        // Ordinal 1: Explorer and the shell show the LOWEST-numbered icon group.
        out += std::format("1 ICON \"{}\"\n\n",
                           escape_rc_string(iconAbs.generic_string()));
    }

    if (res.synthesize_version_info()) {
        // A version with no numeric form (an upstream build number like
        // `b10069`) leaves the four fields at zero and keeps the real text in
        // the string fields — the properties dialog is then partly right rather
        // than wrong. prepare.cppm reports that as a degradation; this function
        // is pure and has no channel to say it on.
        std::array<std::int64_t, 4> f{0, 0, 0, 0};
        if (auto parsed = mcpp::version_req::parse_version(pkg.version); parsed)
            for (std::size_t i = 0; i < 4; ++i) f[i] = parsed->seg(i);
        for (std::size_t i = 0; i < 4; ++i) {
            if (f[i] < 0 || f[i] > 0xFFFF)
                return std::unexpected(std::format(
                    "[resources] cannot build a Windows FILEVERSION from "
                    "[package].version = \"{}\": field {} is {}, and each of the "
                    "four fields must fit in 16 bits (0-65535). Set "
                    "[resources.version-info] explicitly, or use a version whose "
                    "numeric parts fit.",
                    pkg.version, i + 1, f[i]));
        }

        const std::string company = !res.info.company.empty() ? res.info.company
            : (pkg.authors.empty() ? std::string{} : pkg.authors.front());
        const std::string product = !res.info.product.empty() ? res.info.product : pkg.name;
        const std::string descr   = !res.info.description.empty() ? res.info.description
                                                                  : pkg.description;
        const std::string internalName = !res.info.internalName.empty()
            ? res.info.internalName : pkg.name;
        const std::string origName = !res.info.originalFilename.empty()
            ? res.info.originalFilename : std::string(outputFileName);
        std::string copyright = res.info.copyright;
        if (copyright.empty() && !company.empty()) {
            // ASCII on purpose. A user's own metadata may be anything (the
            // build passes the rc tool a UTF-8 codepage for exactly that
            // reason), but text mcpp generates itself should not depend on
            // that flag being right — an em dash here failed llvm-rc outright
            // with "Non-ASCII 8-bit codepoint can't be interpreted in the
            // current codepage".
            copyright = std::format("(C) {}", company);
            if (!pkg.license.empty()) copyright += std::format(" - {}", pkg.license);
        }

        // `1`, not `VS_VERSION_INFO`. See the module header: the macro is only
        // real when windows.h has been included, and without it the resource is
        // filed under a string name that GetFileVersionInfo never looks up.
        out += "1 VERSIONINFO\n";
        out += std::format(" FILEVERSION    {},{},{},{}\n", f[0], f[1], f[2], f[3]);
        out += std::format(" PRODUCTVERSION {},{},{},{}\n", f[0], f[1], f[2], f[3]);
        out += " FILEFLAGSMASK  0x3fL\n";
        out += " FILEFLAGS      0x0L\n";
        out += " FILEOS         0x40004L\n";   // VOS_NT_WINDOWS32
        out += " FILETYPE       0x1L\n";       // VFT_APP
        out += " FILESUBTYPE    0x0L\n";
        out += "BEGIN\n";
        out += "  BLOCK \"StringFileInfo\"\n";
        out += "  BEGIN\n";
        // 040904b0 = US English, Unicode. Paired with the Translation entry
        // below; the two must agree or Windows reads neither.
        out += "    BLOCK \"040904b0\"\n";
        out += "    BEGIN\n";
        auto value = [&](std::string_view k, std::string_view v) {
            if (v.empty()) return;
            out += std::format("      VALUE \"{}\", \"{}\"\n", k, escape_rc_string(v));
        };
        value("CompanyName",      company);
        value("FileDescription",  descr);
        value("FileVersion",      pkg.version);
        value("InternalName",     internalName);
        value("LegalCopyright",   copyright);
        value("OriginalFilename", origName);
        value("ProductName",      product);
        value("ProductVersion",   pkg.version);
        out += "    END\n";
        out += "  END\n";
        out += "  BLOCK \"VarFileInfo\"\n";
        out += "  BEGIN\n";
        out += "    VALUE \"Translation\", 0x409, 1200\n";
        out += "  END\n";
        out += "END\n";
    }
    return out;
}

} // namespace mcpp::build::resources
