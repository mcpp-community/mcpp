// mcpp.pack.binfmt — what a binary depends on, read from the FILE.
//
// WHY THIS MODULE EXISTS
//
// `pack` used to derive an artifact's dependency closure like this:
//
//     LD_TRACE_LOADED_OBJECTS=1 '<binary>'
//
// That is the dynamic linker being asked to resolve the closure, which is
// accurate and complete — and requires RUNNING THE BINARY. So it is not
// "pack has no Windows branch": it cannot cross an OS (a Linux box cannot
// execute a PE) and it cannot cross an ARCHITECTURE (an x86_64 box cannot
// execute an aarch64 ELF, same OS or not). The `#if defined(_WIN32)` refusal
// at the top of `pack::run()` was the symptom; this was the cause.
//
// Reading the file instead removes both limits at once, because nothing here
// executes anything. Cross-OS packaging then is not a feature that had to be
// added — it is what remains when the obstacle is gone.
//
// See .agents/docs/2026-08-16-windows-toolchain-three-axes-design.md §4.
//
// WHAT THIS MODULE DOES NOT DO. It answers "which names does this object
// ask for", not "which files will the loader hand it". Resolution is the
// caller's, because the search rules are the loader's policy — and on ELF
// they are genuinely intricate (DT_RPATH before LD_LIBRARY_PATH before
// DT_RUNPATH before ld.so.cache, `$ORIGIN` expansion, hwcaps subdirectories).
// mcpp does not reimplement that: on the host's own format it still asks the
// loader, and this reader serves the cases the loader cannot be asked about.

export module mcpp.pack.binfmt;

import std;

export namespace mcpp::pack::binfmt {

enum class Format { Unknown, Elf, Pe, MachO };

// The identity a packaging decision needs: can THIS machine run that file?
// Both halves matter and only one of them used to be considered.
struct Ident {
    Format      format = Format::Unknown;
    // The object's own machine, in mcpp's canonical arch spelling
    // ("x86_64", "aarch64", "i686", …). Empty when unrecognised — a new
    // machine type is not a corrupt file, and reporting it as one would be
    // worse than saying nothing.
    std::string arch;
    bool        is64 = false;
};

std::string_view format_name(Format f) {
    switch (f) {
        case Format::Elf:   return "ELF";
        case Format::Pe:    return "PE";
        case Format::MachO: return "Mach-O";
        case Format::Unknown: break;
    }
    return "unknown";
}

// Identify by magic. Never throws, never runs anything; an unreadable or
// truncated file simply comes back Unknown.
Ident identify(const std::filesystem::path& binary);

// The DIRECT dependency names an object declares:
//   ELF  DT_NEEDED entries          ("libstdc++.so.6")
//   PE   import + delay-import DLL names ("vcruntime140.dll")
//
// Names, in the file's own spelling and order, deduplicated. An object with
// no dynamic dependencies yields an empty list — which is a valid answer, not
// a failure, and the caller must not confuse the two (a fully static binary
// and an unparseable one had better not look alike).
std::expected<std::vector<std::string>, std::string>
needed_names(const std::filesystem::path& binary);

// Is `name` provided by the target OS itself — i.e. must NOT be bundled?
//
// On ELF this is the manylinux allow-list, which `pack` already had.
//
// On PE the list is short and the reason is different: shipping a copy of a
// Windows component is not "extra weight", it is a broken program. The OS
// loader resolves `kernel32.dll` from the system directory whatever sits
// beside the executable — except when a DLL that sits beside it is loaded
// FIRST by name, at which point the process has two of something that must be
// unique. Microsoft's redistribution terms say the same thing from the other
// side.
//
// Note what is NOT here: `vcruntime140.dll` and `msvcp140.dll`. Those belong
// to the TOOLSET, not to Windows (see mcpp.build.distribution's PE branch),
// and whether they travel is a contract decision — the one thing this
// predicate must not quietly make for it.
bool is_system_lib(Format f, std::string_view name);

} // namespace mcpp::pack::binfmt

namespace mcpp::pack::binfmt {

namespace detail {

// A bounded, whole-file read. Executables are tens of megabytes at worst and
// the alternative is a seek-heavy parser that has to re-validate every offset
// against the file length anyway; with the bytes in hand, one bounds-checked
// accessor covers every read in this file.
std::optional<std::string> slurp(const std::filesystem::path& p) {
    std::error_code ec;
    auto size = std::filesystem::file_size(p, ec);
    if (ec) return std::nullopt;
    // 512 MiB. A larger "executable" is not one, and refusing is better than
    // allocating whatever a caller was handed.
    if (size > (512ull << 20)) return std::nullopt;
    std::ifstream is(p, std::ios::binary);
    if (!is) return std::nullopt;
    std::string buf(static_cast<std::size_t>(size), '\0');
    is.read(buf.data(), static_cast<std::streamsize>(size));
    buf.resize(static_cast<std::size_t>(is.gcount()));
    return buf;
}

// Little-endian integer at `off`, or nullopt if it would run off the end.
// EVERY read in this module goes through here: a malformed file is ordinary
// input (a truncated download, a text file named `.exe`), and the parser has
// to be total over it rather than trusting a length field it just read.
template <typename T>
std::optional<T> le(std::string_view b, std::size_t off) {
    if (off + sizeof(T) > b.size()) return std::nullopt;
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v |= static_cast<T>(static_cast<unsigned char>(b[off + i]))
             << (8 * i);
    return v;
}

// NUL-terminated string at `off`, bounded by the file end.
std::optional<std::string> cstr(std::string_view b, std::size_t off) {
    if (off >= b.size()) return std::nullopt;
    auto end = b.find('\0', off);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(b.substr(off, end - off));
}

std::string elf_arch(std::uint16_t machine) {
    switch (machine) {
        case 0x03: return "i686";      // EM_386
        case 0x28: return "arm";       // EM_ARM
        case 0x3E: return "x86_64";    // EM_X86_64
        case 0xB7: return "aarch64";   // EM_AARCH64
        case 0xF3: return "riscv64";   // EM_RISCV
        case 0x15: return "ppc64";     // EM_PPC64
        case 0x08: return "mips";      // EM_MIPS
        case 0x16: return "s390x";     // EM_S390
        case 0x102: return "loongarch64";
        default:   return {};
    }
}

std::string pe_arch(std::uint16_t machine) {
    switch (machine) {
        case 0x014c: return "i686";     // IMAGE_FILE_MACHINE_I386
        case 0x8664: return "x86_64";   // AMD64
        case 0xAA64: return "aarch64";  // ARM64
        case 0x01c4: return "arm";      // ARMNT
        default:     return {};
    }
}

// ─── ELF ────────────────────────────────────────────────────────────────
//
// DT_NEEDED lives in the .dynamic section, whose entries are (tag, value)
// pairs; the NEEDED value is a byte offset into the string table named by
// DT_STRTAB. DT_STRTAB is a VIRTUAL ADDRESS, not a file offset, so the PT_LOAD
// segments have to be walked to translate it — which is the one step that
// makes this more than "read a list".
std::expected<std::vector<std::string>, std::string>
elf_needed(std::string_view b) {
    auto cls = le<std::uint8_t>(b, 4);
    auto dat = le<std::uint8_t>(b, 5);
    if (!cls || !dat) return std::unexpected("ELF header is truncated");
    if (*dat != 1)
        return std::unexpected("big-endian ELF objects are not supported");
    const bool is64 = (*cls == 2);
    if (*cls != 1 && *cls != 2)
        return std::unexpected("ELF header declares an unknown class");

    const std::size_t phoffAt = is64 ? 0x20 : 0x1C;
    std::uint64_t phoff = 0;
    if (is64) {
        auto v = le<std::uint64_t>(b, phoffAt);
        if (!v) return std::unexpected("ELF program header offset is truncated");
        phoff = *v;
    } else {
        auto v = le<std::uint32_t>(b, phoffAt);
        if (!v) return std::unexpected("ELF program header offset is truncated");
        phoff = *v;
    }
    auto phentsize = le<std::uint16_t>(b, is64 ? 0x36 : 0x2A);
    auto phnum     = le<std::uint16_t>(b, is64 ? 0x38 : 0x2C);
    if (!phentsize || !phnum)
        return std::unexpected("ELF program header table is truncated");

    struct Load { std::uint64_t vaddr, offset, filesz; };
    std::vector<Load> loads;
    std::uint64_t dynOff = 0, dynSize = 0;
    for (std::uint16_t i = 0; i < *phnum; ++i) {
        const std::size_t ph = static_cast<std::size_t>(phoff)
                             + static_cast<std::size_t>(i) * *phentsize;
        auto type = le<std::uint32_t>(b, ph);
        if (!type) break;
        auto rd = [&](std::size_t off64, std::size_t off32)
            -> std::optional<std::uint64_t> {
            if (is64) return le<std::uint64_t>(b, ph + off64);
            if (auto v = le<std::uint32_t>(b, ph + off32)) return *v;
            return std::nullopt;
        };
        auto offset = rd(0x08, 0x04);
        auto vaddr  = rd(0x10, 0x08);
        auto filesz = rd(0x20, 0x10);
        if (!offset || !vaddr || !filesz) continue;
        if (*type == 1) loads.push_back({*vaddr, *offset, *filesz});   // PT_LOAD
        if (*type == 2) { dynOff = *offset; dynSize = *filesz; }       // PT_DYNAMIC
    }
    // No PT_DYNAMIC = a static executable. Zero dependencies is the answer,
    // and it is a different answer from "could not be read".
    if (dynSize == 0) return std::vector<std::string>{};

    auto vaddr_to_off = [&](std::uint64_t va) -> std::optional<std::uint64_t> {
        for (auto const& l : loads)
            if (va >= l.vaddr && va - l.vaddr < l.filesz)
                return l.offset + (va - l.vaddr);
        return std::nullopt;
    };

    const std::size_t entSize = is64 ? 16 : 8;
    std::vector<std::uint64_t> neededOffsets;
    std::optional<std::uint64_t> strtabVa;
    for (std::uint64_t at = dynOff; at + entSize <= dynOff + dynSize;
         at += entSize) {
        std::uint64_t tag = 0, val = 0;
        if (is64) {
            auto t = le<std::uint64_t>(b, static_cast<std::size_t>(at));
            auto v = le<std::uint64_t>(b, static_cast<std::size_t>(at) + 8);
            if (!t || !v) break;
            tag = *t; val = *v;
        } else {
            auto t = le<std::uint32_t>(b, static_cast<std::size_t>(at));
            auto v = le<std::uint32_t>(b, static_cast<std::size_t>(at) + 4);
            if (!t || !v) break;
            tag = *t; val = *v;
        }
        if (tag == 0) break;                       // DT_NULL
        if (tag == 1) neededOffsets.push_back(val); // DT_NEEDED
        if (tag == 5) strtabVa = val;               // DT_STRTAB
    }
    if (neededOffsets.empty()) return std::vector<std::string>{};
    if (!strtabVa)
        return std::unexpected("ELF .dynamic has DT_NEEDED but no DT_STRTAB");
    auto strOff = vaddr_to_off(*strtabVa);
    if (!strOff)
        return std::unexpected("ELF DT_STRTAB address is in no PT_LOAD segment");

    std::vector<std::string> out;
    for (auto n : neededOffsets) {
        if (auto s = cstr(b, static_cast<std::size_t>(*strOff + n));
            s && !s->empty())
            out.push_back(std::move(*s));
    }
    return out;
}

// ─── PE ─────────────────────────────────────────────────────────────────
//
// Two directories carry DLL names and BOTH are dependencies: the ordinary
// import table (directory 1), resolved by the loader before the process
// starts, and the delay-load table (directory 13), resolved at the first call
// through it. A delay-loaded DLL that is missing does not fail at startup —
// it fails later, somewhere in the program, which is strictly worse to debug.
// Leaving it out of the closure would produce exactly that.
std::expected<std::vector<std::string>, std::string>
pe_needed(std::string_view b) {
    auto lfanew = le<std::uint32_t>(b, 0x3C);
    if (!lfanew) return std::unexpected("PE: no e_lfanew");
    const std::size_t nt = *lfanew;
    if (b.substr(nt, 4) != std::string_view("PE\0\0", 4))
        return std::unexpected("PE: no PE\\0\\0 signature at e_lfanew");

    auto numSections = le<std::uint16_t>(b, nt + 6);
    auto optSize     = le<std::uint16_t>(b, nt + 20);
    auto magic       = le<std::uint16_t>(b, nt + 24);
    if (!numSections || !optSize || !magic)
        return std::unexpected("PE: headers are truncated");
    // 0x10b PE32, 0x20b PE32+. They differ only in where the data directories
    // start — the extra 16 bytes are the 64-bit ImageBase and friends.
    std::size_t dirsAt = 0;
    if      (*magic == 0x10b) dirsAt = nt + 24 + 96;
    else if (*magic == 0x20b) dirsAt = nt + 24 + 112;
    else return std::unexpected("PE: optional header magic is neither PE32 nor PE32+");
    auto numDirs = le<std::uint32_t>(b, dirsAt - 4);
    if (!numDirs) return std::unexpected("PE: data directory count is truncated");

    struct Section { std::uint32_t va, vsize, raw, rawSize; };
    std::vector<Section> sections;
    const std::size_t secAt = nt + 24 + *optSize;
    for (std::uint16_t i = 0; i < *numSections; ++i) {
        const std::size_t s = secAt + static_cast<std::size_t>(i) * 40;
        auto vsize  = le<std::uint32_t>(b, s + 8);
        auto va     = le<std::uint32_t>(b, s + 12);
        auto rawSz  = le<std::uint32_t>(b, s + 16);
        auto raw    = le<std::uint32_t>(b, s + 20);
        if (!vsize || !va || !rawSz || !raw) break;
        sections.push_back({*va, *vsize, *raw, *rawSz});
    }

    auto rva_to_off = [&](std::uint32_t rva) -> std::optional<std::size_t> {
        for (auto const& s : sections) {
            // A section's mapped size is VirtualSize, but a section whose
            // VirtualSize is 0 (some linkers) still maps SizeOfRawData.
            auto span = s.vsize ? s.vsize : s.rawSize;
            if (rva >= s.va && rva - s.va < span) {
                std::size_t off = s.raw + (rva - s.va);
                if (off < b.size()) return off;
                return std::nullopt;
            }
        }
        return std::nullopt;
    };

    std::vector<std::string> out;
    auto push = [&](std::optional<std::string> name) {
        if (!name || name->empty()) return;
        if (std::ranges::find(out, *name) == out.end())
            out.push_back(std::move(*name));
    };

    // Directory 1 — imports. 20-byte descriptors, terminated by an all-zero
    // one; `Name` (offset 12) is an RVA to the DLL's ASCIIZ name.
    if (*numDirs > 1) {
        auto rva = le<std::uint32_t>(b, dirsAt + 1 * 8);
        if (rva && *rva) {
            if (auto at = rva_to_off(*rva)) {
                for (std::size_t d = *at; ; d += 20) {
                    auto nameRva = le<std::uint32_t>(b, d + 12);
                    auto oft     = le<std::uint32_t>(b, d);
                    auto ft      = le<std::uint32_t>(b, d + 16);
                    if (!nameRva || !oft || !ft) break;
                    if (*nameRva == 0 && *oft == 0 && *ft == 0) break;
                    if (auto o = rva_to_off(*nameRva)) push(cstr(b, *o));
                }
            }
        }
    }

    // Directory 13 — delay imports. 32-byte descriptors. `grAttrs` bit 0
    // (`dlattrRva`) says the fields are RVAs; the pre-VC7 form stored virtual
    // ADDRESSES instead, and nothing produced in this century does that, so a
    // descriptor without the bit is skipped rather than guessed at.
    if (*numDirs > 13) {
        auto rva = le<std::uint32_t>(b, dirsAt + 13 * 8);
        if (rva && *rva) {
            if (auto at = rva_to_off(*rva)) {
                for (std::size_t d = *at; ; d += 32) {
                    auto attrs   = le<std::uint32_t>(b, d);
                    auto nameRva = le<std::uint32_t>(b, d + 4);
                    if (!attrs || !nameRva) break;
                    if (*attrs == 0 && *nameRva == 0) break;
                    if ((*attrs & 1u) == 0) continue;
                    if (auto o = rva_to_off(*nameRva)) push(cstr(b, *o));
                }
            }
        }
    }
    return out;
}

// PEP 600 / manylinux2014: assumed present on any target Linux glibc system.
constexpr std::array kElfSystem = std::to_array<std::string_view>({
    "libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so",
    "libutil.so", "libnsl.so", "libresolv.so", "libcrypt.so",
    "libstdc++.so", "libgcc_s.so", "linux-vdso.so", "ld-linux", "libld-linux",
});

// Windows' own. Deliberately NOT including vcruntime140/msvcp140: those
// belong to the toolset, and whether they travel is `cxx_runtime`'s decision.
constexpr std::array kPeSystem = std::to_array<std::string_view>({
    "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll",
    "gdi32full.dll", "advapi32.dll", "shell32.dll", "shlwapi.dll",
    "ole32.dll", "oleaut32.dll", "combase.dll", "comdlg32.dll",
    "comctl32.dll", "rpcrt4.dll", "sechost.dll", "setupapi.dll",
    "cfgmgr32.dll", "version.dll", "winmm.dll", "imm32.dll", "uxtheme.dll",
    "dwmapi.dll", "userenv.dll", "psapi.dll", "iphlpapi.dll", "netapi32.dll",
    "ws2_32.dll", "wsock32.dll", "mswsock.dll", "crypt32.dll", "bcrypt.dll",
    "bcryptprimitives.dll", "ncrypt.dll", "secur32.dll", "wintrust.dll",
    "powrprof.dll", "winspool.drv", "msimg32.dll", "opengl32.dll",
    "glu32.dll", "dxgi.dll", "d3d9.dll", "d3d11.dll", "d3d12.dll",
    "dbghelp.dll", "hid.dll", "avrt.dll", "mfplat.dll", "dnsapi.dll",
    "profapi.dll", "ucrtbase.dll", "msvcrt.dll", "win32u.dll",
});

} // namespace detail

Ident identify(const std::filesystem::path& binary) {
    Ident id;
    auto buf = detail::slurp(binary);
    if (!buf || buf->size() < 8) return id;
    std::string_view b{*buf};

    if (b.starts_with(std::string_view("\x7f" "ELF", 4))) {
        id.format = Format::Elf;
        auto cls = detail::le<std::uint8_t>(b, 4);
        id.is64 = cls && *cls == 2;
        if (auto m = detail::le<std::uint16_t>(b, 18)) id.arch = detail::elf_arch(*m);
        return id;
    }
    if (b.starts_with("MZ")) {
        // MZ alone is a DOS stub; a PE needs the signature e_lfanew points at.
        // Saying "PE" for a file that has none would send the caller into a
        // parser that cannot succeed.
        if (auto lfanew = detail::le<std::uint32_t>(b, 0x3C)) {
            if (b.substr(*lfanew, 4) == std::string_view("PE\0\0", 4)) {
                id.format = Format::Pe;
                if (auto m = detail::le<std::uint16_t>(b, *lfanew + 4))
                    id.arch = detail::pe_arch(*m);
                if (auto magic = detail::le<std::uint16_t>(b, *lfanew + 24))
                    id.is64 = (*magic == 0x20b);
                return id;
            }
        }
        return id;
    }
    // Mach-O, both endiannesses and the fat wrapper. Recognised but not
    // parsed: macOS packaging still asks the loader, and a caller that lands
    // here deserves a message naming the format rather than "unknown".
    for (auto magic : {std::string_view("\xcf\xfa\xed\xfe", 4),
                       std::string_view("\xce\xfa\xed\xfe", 4),
                       std::string_view("\xfe\xed\xfa\xcf", 4),
                       std::string_view("\xfe\xed\xfa\xce", 4),
                       std::string_view("\xca\xfe\xba\xbe", 4)}) {
        if (b.starts_with(magic)) { id.format = Format::MachO; return id; }
    }
    return id;
}

std::expected<std::vector<std::string>, std::string>
needed_names(const std::filesystem::path& binary) {
    auto buf = detail::slurp(binary);
    if (!buf)
        return std::unexpected(std::format("cannot read '{}'", binary.string()));
    std::string_view b{*buf};
    switch (identify(binary).format) {
        case Format::Elf: return detail::elf_needed(b);
        case Format::Pe:  return detail::pe_needed(b);
        case Format::MachO:
            return std::unexpected(
                "Mach-O dependency reading is not implemented; macOS packaging "
                "resolves the closure through the loader instead");
        case Format::Unknown: break;
    }
    return std::unexpected(std::format(
        "'{}' is not an ELF, PE or Mach-O object", binary.string()));
}

bool is_system_lib(Format f, std::string_view name) {
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    if (f == Format::Pe) {
        // API sets: `api-ms-win-crt-runtime-l1-1-0.dll` and friends are
        // forwarders resolved by the loader against the OS. They have no file
        // to copy on most systems and copying one would be wrong anyway.
        if (lower.starts_with("api-ms-") || lower.starts_with("ext-ms-"))
            return true;
        return std::ranges::find(detail::kPeSystem, lower)
            != detail::kPeSystem.end();
    }
    if (f == Format::Elf) {
        for (auto prefix : detail::kElfSystem)
            if (lower.starts_with(prefix)) return true;
        return false;
    }
    return false;
}

} // namespace mcpp::pack::binfmt
