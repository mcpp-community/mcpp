// mcpp.build.coff_exports — which symbols of a COFF object a DLL should export.
//
// WHY THIS EXISTS
//
// MSVC exports nothing from a DLL unless the source says `__declspec(dllexport)`
// or a `.def` file lists the symbols. Without either, the import library comes
// out empty and every consumer fails with unresolved externals naming symbols
// that are plainly in the object files — a diagnostic pointing nowhere near its
// cause. MinGW's linker auto-exports and hides the whole problem; lld-link's
// MSVC flavour does not, deliberately, because PE caps exports at 65535.
//
// So mcpp generates the `.def`. This is not novel: CMake has shipped
// `WINDOWS_EXPORT_ALL_SYMBOLS` since 3.4 and its `bindexplib` does exactly this.
// The filter below follows that one's semantics, which are the de-facto standard
// for "what does auto-export mean on Windows".
//
// WHY IT PARSES COFF ITSELF
//
// `dumpbin` lives in a Visual Studio developer environment, and mcpp's default
// Windows toolchain is clang — a plain `mcpp build` is not inside that
// environment. `llvm-nm` would be a second external dependency with the same
// class of failure ("not on this machine"). COFF's symbol table is a fixed-size
// record array with a string table after it; reading it needs no library.
//
// WHY IT IS A PURE FUNCTION OVER BYTES
//
// So it can be tested on any host. Windows CI can only tell whether the linker
// accepted the result; the filtering rules are where mistakes hide, and those
// are decidable from a byte buffer. Linux CI produces real COFF objects through
// mingw-cross, so the parser is exercised against genuine input off-Windows too.
//
// Design: .agents/docs/2026-08-18-windows-shared-library-and-module-extensions.md §2.

export module mcpp.build.coff_exports;

import std;

export namespace mcpp::build::coff {

// One exportable symbol.
struct Export {
    std::string name;
    // `.def` needs `name DATA` for anything that is not code: the linker
    // otherwise generates a call thunk for a variable, and the consumer reads
    // the thunk instead of the value.
    bool        data = false;

    bool operator==(const Export&) const = default;
    auto operator<=>(const Export&) const = default;
};

// PE's export directory addresses exports by 16-bit ordinal, so a DLL cannot
// have more than this many. Reaching it is refused rather than truncated: a
// truncated export table links and then fails at the consumer, naming whichever
// symbol happened to fall off the end.
inline constexpr std::size_t kMaxExports = 65535;

// Machine types this reader accepts. An unknown one is not an error to guess
// at: the symbol-table layout is shared, but a machine it has never been tried
// against is exactly where an untested assumption would hide.
bool is_supported_machine(std::uint16_t machine);

// Read one object file's exportable symbols. `bytes` is a whole `.obj`.
//
// Returns an error string for input this reader cannot honestly interpret —
// truncated, an unsupported machine, or a symbol table that runs off the end.
// It never returns a partial list: half a symbol table is indistinguishable
// from a small one, and the caller would ship a DLL missing exports.
std::expected<std::vector<Export>, std::string>
read_exports(std::span<const std::byte> bytes);

// The `.def` text for a set of exports, sorted and de-duplicated.
//
// `libraryName` goes in the `LIBRARY` statement — link.exe accepts a `.def`
// without one, but naming it makes the file self-describing when a human opens
// it while working out why a symbol is missing.
std::string write_def(std::string_view libraryName, std::vector<Export> exports);

} // namespace mcpp::build::coff

namespace mcpp::build::coff {

namespace {

// ── COFF, only the parts needed ──────────────────────────────────────────
//
// Offsets rather than structs: a packed-struct cast would depend on the host's
// alignment rules to read a file format, and this code runs on hosts that never
// produce COFF.

constexpr std::size_t kFileHeaderSize   = 20;
constexpr std::size_t kSymbolRecordSize = 18;
constexpr std::size_t kSectionHeaderSize = 40;

// The subset of IMAGE_FILE_MACHINE_* that mcpp targets or can be handed.
constexpr std::uint16_t kMachineI386   = 0x014c;
constexpr std::uint16_t kMachineAmd64  = 0x8664;
constexpr std::uint16_t kMachineArm    = 0x01c0;
constexpr std::uint16_t kMachineArmNT  = 0x01c4;
constexpr std::uint16_t kMachineArm64  = 0xaa64;

constexpr std::uint8_t  kSymClassExternal = 2;    // IMAGE_SYM_CLASS_EXTERNAL
constexpr std::uint16_t kSymTypeFunction  = 0x20; // DTYPE_FUNCTION << 4
constexpr std::uint32_t kScnMemWrite      = 0x80000000u; // IMAGE_SCN_MEM_WRITE
constexpr std::uint32_t kScnMemExecute    = 0x20000000u; // IMAGE_SCN_MEM_EXECUTE

std::uint16_t rd16(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[off])
         | (std::to_integer<unsigned>(b[off + 1]) << 8));
}
std::uint32_t rd32(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off])
         | (std::to_integer<unsigned>(b[off + 1]) << 8)
         | (std::to_integer<unsigned>(b[off + 2]) << 16)
         | (std::to_integer<unsigned>(b[off + 3]) << 24));
}

// Symbols bindexplib skips, and why each one would be wrong to export.
bool is_skipped_name(std::string_view n) {
    // Scalar-deleting and vector-deleting destructor thunks. They are emitted
    // per-TU and exporting them makes the DLL's surface depend on which
    // translation unit happened to instantiate one.
    if (n.starts_with("??_G") || n.starts_with("??_E")) return true;
    // Managed (C++/CLI) artefacts: `.` cannot appear in a C++ mangled name, and
    // these forms name IL constructs that mean nothing to a native consumer.
    if (n.find('.') != std::string_view::npos) return true;
    if (n.find("__t2m") != std::string_view::npos) return true;
    if (n.find("$$F")   != std::string_view::npos) return true;
    if (n.find("$$J")   != std::string_view::npos) return true;
    // ARM64EC thunks: linker-generated bridges between the ARM64 and x64 views
    // of the same function. Exporting a bridge exports neither side.
    if (n.find("$ientry_thunk") != std::string_view::npos) return true;
    if (n.find("$entry_thunk")  != std::string_view::npos) return true;
    if (n.find("$iexit_thunk")  != std::string_view::npos) return true;
    if (n.find("$exit_thunk")   != std::string_view::npos) return true;
    return false;
}

} // namespace

bool is_supported_machine(std::uint16_t machine) {
    return machine == kMachineI386  || machine == kMachineAmd64
        || machine == kMachineArm   || machine == kMachineArmNT
        || machine == kMachineArm64;
}

std::expected<std::vector<Export>, std::string>
read_exports(std::span<const std::byte> bytes)
{
    if (bytes.size() < kFileHeaderSize)
        return std::unexpected("not a COFF object: shorter than a file header");

    const auto machine = rd16(bytes, 0);
    if (!is_supported_machine(machine)) {
        return std::unexpected(std::format(
            "unsupported COFF machine 0x{:04x}. mcpp reads i386, amd64, arm, "
            "armnt and arm64; anything else would be a guess at a layout this "
            "has never been run against.", machine));
    }

    const auto numSections   = rd16(bytes, 2);
    const auto symTableOff   = rd32(bytes, 8);
    const auto numSymbols    = rd32(bytes, 12);
    if (symTableOff == 0 || numSymbols == 0) return std::vector<Export>{};

    // Section characteristics, indexed 1-based the way symbols address them.
    const std::size_t sectionsOff = kFileHeaderSize + rd16(bytes, 16); // + optional header
    std::vector<std::uint32_t> sectionFlags(numSections + 1, 0);
    for (std::uint16_t i = 0; i < numSections; ++i) {
        const auto off = sectionsOff + std::size_t(i) * kSectionHeaderSize;
        if (off + kSectionHeaderSize > bytes.size())
            return std::unexpected("COFF section headers run past the end of the file");
        sectionFlags[i + 1] = rd32(bytes, off + 36);
    }

    const std::size_t symbolsEnd = std::size_t(symTableOff)
                                 + std::size_t(numSymbols) * kSymbolRecordSize;
    if (symbolsEnd > bytes.size())
        return std::unexpected("COFF symbol table runs past the end of the file");

    // The string table follows the symbol table; its first 4 bytes are its own
    // size. Long names are `/<offset>` into it.
    const std::size_t stringsOff = symbolsEnd;
    const bool hasStrings = stringsOff + 4 <= bytes.size();

    auto name_at = [&](std::size_t rec) -> std::expected<std::string, std::string> {
        // A name is 8 bytes: either inline (NUL-padded) or a zero word followed
        // by an offset into the string table.
        if (rd32(bytes, rec) != 0) {
            std::string s;
            for (std::size_t i = 0; i < 8; ++i) {
                auto c = std::to_integer<char>(bytes[rec + i]);
                if (c == '\0') break;
                s.push_back(c);
            }
            return s;
        }
        const auto off = rd32(bytes, rec + 4);
        if (!hasStrings || stringsOff + off >= bytes.size())
            return std::unexpected("COFF long symbol name points outside the string table");
        std::string s;
        for (std::size_t i = stringsOff + off; i < bytes.size(); ++i) {
            auto c = std::to_integer<char>(bytes[i]);
            if (c == '\0') break;
            s.push_back(c);
        }
        return s;
    };

    std::vector<Export> out;
    for (std::uint32_t i = 0; i < numSymbols; ) {
        const std::size_t rec = std::size_t(symTableOff)
                              + std::size_t(i) * kSymbolRecordSize;
        const auto sectionNum = static_cast<std::int16_t>(rd16(bytes, rec + 12));
        const auto type       = rd16(bytes, rec + 14);
        const auto storage    = std::to_integer<std::uint8_t>(bytes[rec + 16]);
        const auto numAux     = std::to_integer<std::uint8_t>(bytes[rec + 17]);

        // Advance past auxiliary records regardless of whether this symbol is
        // taken — an aux record is not a symbol and reading it as one produces
        // names out of raw section data.
        i += 1u + numAux;

        if (storage != kSymClassExternal) continue;   // not visible outside the TU
        if (sectionNum <= 0)              continue;   // undefined, absolute or debug
        if (type != kSymTypeFunction && type != 0) continue;

        auto nm = name_at(rec);
        if (!nm) return std::unexpected(nm.error());
        if (nm->empty() || is_skipped_name(*nm)) continue;

        // i386 (and `__cdecl` on it) carries a leading underscore that is part
        // of the calling convention rather than of the name.
        std::string name = *nm;
        if (machine == kMachineI386 && name.starts_with('_') && name.find('@') == std::string::npos)
            name.erase(0, 1);

        const auto flags = std::size_t(sectionNum) < sectionFlags.size()
                         ? sectionFlags[sectionNum] : 0u;
        // DATA when it is not code. Both halves matter: a function symbol in a
        // writable section is still a function, and a variable in a read-only
        // section (a `const`) is still data.
        const bool isData = type != kSymTypeFunction
                         && (flags & kScnMemExecute) == 0;
        out.push_back(Export{ std::move(name), isData });
    }

    return out;
}

std::string write_def(std::string_view libraryName, std::vector<Export> exports) {
    std::ranges::sort(exports);
    exports.erase(std::ranges::unique(exports).begin(), exports.end());

    std::string out;
    if (!libraryName.empty()) out += std::format("LIBRARY {}\n", libraryName);
    out += "EXPORTS\n";
    for (auto const& e : exports)
        out += std::format("    {}{}\n", e.name, e.data ? " DATA" : "");
    return out;
}

} // namespace mcpp::build::coff
