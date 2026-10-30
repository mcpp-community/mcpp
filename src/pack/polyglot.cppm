// mcpp.pack.polyglot — one file holding the single files of several systems
// (SPEC-014 §5.3; design 2026-10-30 part 2 §19.6).
//
// THE FILE IS THE WINDOWS PROGRAM, AND A POSIX SHELL SCRIPT. A PE begins with
// a DOS header whose first bytes are free but for `MZ` and whose word at 0x3C
// names the PE header. Written as `MZqFpD='` the header is also the start of
// a shell assignment whose quoted value runs to a `'` after that word; the
// lines after it are the script, and the PE header and sections follow, moved
// down by a multiple of the file alignment. Windows starts the PE as it is.
// A POSIX shell reads the script, which reads the rest of itself from the end
// of the file, copies this system's program to the single file's cache (the
// rules of SPEC-014 §5.2) and executes it there.
//
// THE PROGRAMS ARE THE ROW'S SINGLE FILES, UNCHANGED. Each is a native
// program the platform's carrier made and signed (a Mach-O keeps its
// signature, since the bytes the cache receives are the bytes that were
// signed); the file adds no runtime of its own. What it cannot do is be
// executed directly by `execve` on a POSIX system (ENOEXEC): it is started by
// `sh <file>`, or by a shell that runs a file the kernel refused as a script.
//
// NOT COSMOPOLITAN'S APE. Measured against it (part 2 §19.1, §19.6.1): the PE
// here is a standard linker's, so Authenticode signs it and Windows starts it
// as fast as the row's file; macOS needs no compiler; the programs are the
// conformance-tested native implementations.

export module mcpp.pack.polyglot;

import std;

export namespace mcpp::pack::polyglot {

// One system's program: `system` is `linux`, `macos` or `windows`, `arch`
// `x86_64` or `aarch64`.
struct Program {
    std::string system;
    std::string arch;
    std::string bytes;
    std::string sha256;   // of `bytes`: the cache directory's name
};

// The file: the Windows program (at most one) leads, the others follow it.
// `name` is the program's name in the cache and in messages.
std::expected<std::string, std::string> assemble(std::string_view name, const std::vector<Program>& programs);

// Whether `head`, the first bytes of a file, are a file `assemble` wrote.
bool is_polyglot(std::string_view head);

} // namespace mcpp::pack::polyglot

namespace mcpp::pack::polyglot {

namespace {

std::uint16_t u16(std::string_view b, std::size_t o) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[o])
                                      | static_cast<unsigned char>(b[o + 1]) << 8);
}
std::uint32_t u32(std::string_view b, std::size_t o) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | static_cast<unsigned char>(b[o + static_cast<std::size_t>(i)]);
    return v;
}
void put32(std::string& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + static_cast<std::size_t>(i)] = static_cast<char>(v >> (8 * i) & 0xFF);
}

// `uname -s`-`uname -m` spellings of a system and architecture.
std::string uname_pattern(std::string_view system, std::string_view arch) {
    const std::string s = system == "macos" ? "Darwin" : "Linux";
    if (arch == "x86_64") return std::format("{0}-x86_64|{0}-amd64", s);
    return std::format("{0}-aarch64|{0}-arm64", s);
}

constexpr std::string_view kMarker = "# mcpp polyglot";

// The script the file's head evaluates: the program of this system, copied
// to the single file's cache by SPEC-014 §5.2's rules, executed with the
// arguments. `$o` is the file, set by the head.
std::string stage_two(std::string_view name, const std::vector<Program>& posix,
                      const std::vector<std::uint64_t>& offsets, bool leadsWindows) {
    std::string held;
    for (auto const& p : posix) held += (held.empty() ? "" : ", ") + std::format("{}-{}", p.system, p.arch);
    if (leadsWindows) held += (held.empty() ? "" : ", ") + std::string("windows (run directly)");
    std::string cases;
    for (std::size_t i = 0; i < posix.size(); ++i)
        cases += std::format("{}) k={}-{} a={} b={} h={};;\n", uname_pattern(posix[i].system, posix[i].arch),
                             posix[i].system, posix[i].arch, offsets[i], posix[i].bytes.size(), posix[i].sha256);
    if (leadsWindows) cases += "MINGW*|MSYS*|CYGWIN*) exec \"$o\" \"$@\";;\n";
    return std::format(R"SH({0}: the programs of several systems in one file (SPEC-014 §5.3)
n='{1}'
f() {{ echo "$n: $*" >&2; exit 127; }}
m() {{ [ ! -L "$1" ] && [ -d "$1" ] && [ "$(ls -ldn "$1" | awk '{{print $3}}')" = "$(id -u)" ]; }}
case $(uname -s)-$(uname -m) in
{2}*) echo "$n: this file holds no program for $(uname -s)-$(uname -m); it holds {3}" >&2; exit 126;;
esac
[ -n "$HOME" ] || f "this machine names no cache directory for its user"
case $(uname -s) in
Darwin) r=$HOME/Library/Caches;;
*) case $XDG_CACHE_HOME in /*) r=$XDG_CACHE_HOME;; *) r=$HOME/.cache;; esac;;
esac
u=$r/mcpp; s=$u/single-file; d=$s/$h
mkdir -p "$s" 2>/dev/null
for x in "$u" "$s"; do m "$x" || f "$x is not a directory of this user's; nothing is run from it"; done
if [ ! -f "$d/.mcpp-complete" ]; then
  chmod 700 "$s" || f "cannot restrict $s to this user"
  t=$s/.$h-$$
  rm -rf "$t"; mkdir "$t" || f "cannot write under $s"
  tail -c +$((a+1)) "$o" | head -c "$b" > "$t/$n" || {{ rm -rf "$t"; f "cannot write $t/$n"; }}
  if command -v sha256sum >/dev/null 2>&1; then g=$(sha256sum "$t/$n")
  elif command -v shasum >/dev/null 2>&1; then g=$(shasum -a 256 "$t/$n")
  elif command -v openssl >/dev/null 2>&1; then g=$(openssl dgst -sha256 -r "$t/$n")
  else rm -rf "$t"; f "no sha256sum, shasum or openssl here to check the program for $k with"; fi
  case $g in "$h"*) ;; *) rm -rf "$t"; f "the program for $k does not match its digest: the file is damaged";; esac
  chmod 755 "$t/$n" && echo "$h" > "$t/.mcpp-complete" || {{ rm -rf "$t"; f "cannot write under $t"; }}
  mv "$t" "$d" 2>/dev/null
  rm -rf "$t" "$d/.$h-$$"
  [ -f "$d/.mcpp-complete" ] || f "cannot place the program under $d"
fi
m "$d" || f "$d is not a directory of this user's; nothing is run from it"
exec "$d/$n" "$@"
)SH", kMarker, name, cases, held);
}

// The head's script: the file's own path, and the rest read from its end.
std::string stage_one(std::uint64_t at, std::uint64_t length) {
    return std::format("o=$0;case $o in */*);;*)if [ -f \"$o\" ];then o=./$o;else o=$(command -v \"$0\");fi;;esac\n"
                       "eval \"$(tail -c +{} \"$o\"|head -c {})\";exit 127\n", at + 1, length);
}

}  // namespace

bool is_polyglot(std::string_view head) {
    return head.starts_with("MZqFpD='\n") || head.starts_with("#!/bin/sh\n" "o=$0;");
}

std::expected<std::string, std::string> assemble(std::string_view name, const std::vector<Program>& programs) {
    if (name.empty() || name.find_first_of("'/\\\n") != std::string_view::npos)
        return std::unexpected(std::format("'{}' cannot name a program in a polyglot file", name));
    const Program* windows = nullptr;
    std::vector<Program> posix;
    for (auto const& p : programs) {
        if (p.system == "windows") {
            if (windows)
                return std::unexpected(std::format(
                    "a polyglot file leads with one Windows program, and this pack has two ({}, {})",
                    windows->arch, p.arch));
            windows = &p;
        } else if (p.system == "linux" || p.system == "macos") {
            posix.push_back(p);
        } else {
            return std::unexpected(std::format("a polyglot file holds no program of {}", p.system));
        }
    }
    if (programs.empty()) return std::unexpected(std::string("a polyglot file holds at least one program"));

    // Without a Windows program the head is a plain script.
    if (!windows) {
        std::uint64_t headSize = 0;
        std::string out;
        for (int pass = 0; pass < 8; ++pass) {
            std::vector<std::uint64_t> offsets;
            std::uint64_t at = headSize;
            for (auto const& p : posix) { offsets.push_back(at); at += p.bytes.size(); }
            const auto two = stage_two(name, posix, offsets, false);
            const auto head = "#!/bin/sh\n" + stage_one(at, two.size());
            if (head.size() == headSize) {
                out = head;
                for (auto const& p : posix) out += p.bytes;
                out += two;
                return out;
            }
            headSize = head.size();
        }
        return std::unexpected(std::string("the polyglot head did not settle"));
    }

    // THE PE (PE32+, unsigned): its header moves down by a multiple of the
    // file alignment, and every file offset it holds moves with it.
    std::string_view pe = windows->bytes;
    if (pe.size() < 0x40 || pe[0] != 'M' || pe[1] != 'Z') return std::unexpected(std::string("the Windows program is no PE"));
    const std::uint32_t lfanew = u32(pe, 0x3C);
    if (lfanew + 24 > pe.size() || pe.substr(lfanew, 4) != std::string_view("PE\0\0", 4))
        return std::unexpected(std::string("the Windows program is no PE"));
    const std::size_t coff = lfanew + 4;
    const std::uint16_t nsec = u16(pe, coff + 2);
    const std::uint16_t optSize = u16(pe, coff + 16);
    const std::size_t opt = coff + 20;
    if (opt + optSize > pe.size() || u16(pe, opt) != 0x20B)
        return std::unexpected(std::string("the Windows program is not PE32+"));
    const std::uint32_t fileAlign = u32(pe, opt + 36);
    const std::size_t sizeHeadersAt = opt + 60;
    const std::uint32_t sizeHeaders = u32(pe, sizeHeadersAt);
    const std::size_t dirs = opt + 112;
    const std::size_t sec = opt + optSize;
    if (sec + 40ull * nsec > pe.size() || fileAlign == 0)
        return std::unexpected(std::string("the Windows program's section table is truncated"));
    if (u32(pe, dirs + 8 * 4) != 0)
        return std::unexpected(std::string(
            "the Windows program is already signed; a polyglot file is signed after it is assembled"));
    std::uint32_t firstVa = 0xFFFFFFFF;
    for (std::uint16_t i = 0; i < nsec; ++i) firstVa = std::min(firstVa, u32(pe, sec + 40u * i + 12));

    auto build = [&](std::uint64_t twoAt, std::uint64_t twoSize) {
        std::string head = "MZqFpD='\n";
        head.resize(0x3C, ' ');
        head += std::string(4, '\0');   // e_lfanew, written below
        head += "'\n" + stage_one(twoAt, twoSize);
        return head;
    };
    std::string head;
    std::uint32_t shift = 0;
    std::uint64_t twoAt = 0, twoSize = 0;
    std::string two;
    for (int pass = 0;; ++pass) {
        if (pass == 16) return std::unexpected(std::string("the polyglot head did not settle"));
        head = build(twoAt, twoSize);
        const std::uint64_t want = (head.size() + 7) & ~std::uint64_t{7};
        shift = want > lfanew
            ? static_cast<std::uint32_t>((want - lfanew + fileAlign - 1) / fileAlign * fileAlign) : 0;
        // The quoted header must hold no `'`, and e_lfanew is in it.
        while (((lfanew + shift) & 0xFF) == '\'' || ((lfanew + shift) >> 8 & 0xFF) == '\''
               || ((lfanew + shift) >> 16 & 0xFF) == '\'')
            shift += fileAlign;
        std::vector<std::uint64_t> offsets;
        std::uint64_t at = pe.size() + shift;
        for (auto const& p : posix) { offsets.push_back(at); at += p.bytes.size(); }
        two = stage_two(name, posix, offsets, true);
        if (at == twoAt && two.size() == twoSize) break;
        twoAt = at;
        twoSize = two.size();
    }
    if (sizeHeaders + shift > firstVa)
        return std::unexpected(std::format(
            "the polyglot head ({} bytes) does not fit before the Windows program's first section", head.size()));

    std::string body(pe.substr(lfanew));
    auto patch = [&](std::size_t fileOffset) {
        const std::size_t o = fileOffset - lfanew;
        if (const auto v = u32(body, o); v != 0) put32(body, o, v + shift);
    };
    patch(sizeHeadersAt);
    patch(coff + 8);   // PointerToSymbolTable
    for (std::uint16_t i = 0; i < nsec; ++i) {
        const std::size_t s = sec + 40u * i;
        patch(s + 20);   // PointerToRawData
        patch(s + 24);   // PointerToRelocations
        patch(s + 28);   // PointerToLinenumbers
    }
    // The debug directory's entries name file offsets (PointerToRawData).
    const std::uint32_t dbgRva = u32(pe, dirs + 8 * 6), dbgSize = u32(pe, dirs + 8 * 6 + 4);
    if (dbgRva && dbgSize)
        for (std::uint16_t i = 0; i < nsec; ++i) {
            const std::size_t s = sec + 40u * i;
            const std::uint32_t va = u32(pe, s + 12), raw = u32(pe, s + 20);
            const std::uint32_t vsz = std::max(u32(pe, s + 8), u32(pe, s + 16));
            if (va <= dbgRva && dbgRva < va + vsz)
                for (std::uint32_t e = 0; e < dbgSize / 28; ++e)
                    if (const std::size_t ptr = raw + (dbgRva - va) + 28u * e + 24; ptr + 4 <= pe.size())
                        patch(ptr);
        }

    std::string out = head;
    out.resize(lfanew + shift, '\n');
    put32(out, 0x3C, lfanew + shift);
    out += body;
    for (auto const& p : posix) out += p.bytes;
    if (out.size() != twoAt) return std::unexpected(std::string("the polyglot layout is inconsistent"));
    out += two;
    return out;
}

} // namespace mcpp::pack::polyglot
