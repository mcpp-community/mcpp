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

// Does this object ALREADY declare exports?
//
// `__declspec(dllexport)` makes the compiler write `/EXPORT:name` directives
// into the object's `.drectve` section, which the linker reads. When an author
// has annotated their surface, adding a generated `.def` on top would export
// the same names twice — `LNK4197: export specified multiple times` — and,
// worse, would export everything else as well, quietly replacing a chosen
// public surface with all of it.
//
// So the annotation wins and mcpp stays out of the way. Detected rather than
// configured: a manifest key for "I annotated my exports" would be a second
// place to say something the objects already say, and the two could disagree.
bool declares_exports(std::span<const std::byte> bytes);

// Shared by the COFF and LLVM symbol readers so LTO does not change the
// filtering policy or i386 calling-convention spelling.
std::optional<std::string> export_name(std::string_view name, bool i386);

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

// AN ANONYMOUS OBJECT: machine 0 and 0xFFFF where an ordinary header has its
// section count, then a version and, from version 1 on, a class GUID at
// offset 12 that says what the rest is. Three kinds reach a link (2026.10.5.2;
// headers measured from cl 19.51 output, GUIDs as LLVM's `identify_magic`
// spells them):
//   version 0           a short import object, a member of an import library;
//   kBigObjClassId      `/bigobj`, an ordinary object with 32-bit counts;
//   kClGlClassId        `/GL`, cl's intermediate code, with no symbol table.
// `/GL` together with `/bigobj` yields the `/GL` header.
constexpr std::size_t kBigObjHeaderSize  = 56;
constexpr std::size_t kBigObjSymbolSize  = 20;
constexpr std::array<std::uint8_t, 16> kBigObjClassId{
    0xc7, 0xa1, 0xba, 0xd1, 0xee, 0xba, 0xa9, 0x4b,
    0xaf, 0x20, 0xfa, 0xf6, 0x6a, 0xa4, 0xdc, 0xb8};
constexpr std::array<std::uint8_t, 16> kClGlClassId{
    0x38, 0xfe, 0xb3, 0x0c, 0xa5, 0xd9, 0xab, 0x4d,
    0xac, 0x9b, 0xd6, 0xb6, 0x22, 0x26, 0x53, 0xc2};

// The subset of IMAGE_FILE_MACHINE_* that mcpp targets or can be handed.
constexpr std::uint16_t kMachineI386   = 0x014c;
constexpr std::uint16_t kMachineAmd64  = 0x8664;
constexpr std::uint16_t kMachineArm    = 0x01c0;
constexpr std::uint16_t kMachineArmNT  = 0x01c4;
constexpr std::uint16_t kMachineArm64  = 0xaa64;

constexpr std::uint8_t  kSymClassExternal = 2;    // IMAGE_SYM_CLASS_EXTERNAL
constexpr std::uint16_t kSymTypeFunction  = 0x20; // DTYPE_FUNCTION << 4
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

// Where an object keeps its sections and symbols. An ordinary header and a
// `/bigobj` one differ in the width of the counts and of a symbol record and in
// where the section headers start; the records themselves are the same apart
// from the section number, which `/bigobj` widens to 32 bits.
struct Layout {
    std::uint16_t machine      = 0;
    std::uint32_t numSections  = 0;
    std::size_t   sectionsOff  = 0;
    std::uint32_t symTableOff  = 0;
    std::uint32_t numSymbols   = 0;
    std::size_t   symbolSize   = kSymbolRecordSize;
    bool          big          = false;
};

bool class_id_is(std::span<const std::byte> b, const std::array<std::uint8_t, 16>& id) {
    if (b.size() < 28) return false;
    for (std::size_t i = 0; i < id.size(); ++i)
        if (std::to_integer<std::uint8_t>(b[12 + i]) != id[i]) return false;
    return true;
}

std::expected<Layout, std::string> layout_of(std::span<const std::byte> b) {
    if (b.size() < kFileHeaderSize)
        return std::unexpected("not a COFF object: shorter than a file header");
    Layout l;
    if (rd16(b, 0) == 0 && rd16(b, 2) == 0xFFFF) {
        const auto version = b.size() >= 6 ? rd16(b, 4) : 0;
        if (version == 0)
            return std::unexpected(
                "this is a short import object (a member of an import library), "
                "not a compiled object; it declares no symbols to export");
        if (class_id_is(b, kClGlClassId))
            return std::unexpected(
                "this object was compiled by cl.exe with /GL: it holds the "
                "compiler's intermediate code and no symbol table, so its exports "
                "cannot be discovered.\n"
                "  Set `windows_auto_export = false` on the shared library and mark "
                "its public surface with __declspec(dllexport), or compile it "
                "without /GL.");
        if (!class_id_is(b, kBigObjClassId) || b.size() < kBigObjHeaderSize)
            return std::unexpected(std::format(
                "this is an anonymous COFF object of a kind mcpp does not read "
                "(version {}); reading it would be a guess at its layout", version));
        l.big         = true;
        l.machine     = rd16(b, 6);
        l.numSections = rd32(b, 44);
        l.symTableOff = rd32(b, 48);
        l.numSymbols  = rd32(b, 52);
        l.sectionsOff = kBigObjHeaderSize;
        l.symbolSize  = kBigObjSymbolSize;
        return l;
    }
    l.machine     = rd16(b, 0);
    l.numSections = rd16(b, 2);
    l.symTableOff = rd32(b, 8);
    l.numSymbols  = rd32(b, 12);
    l.sectionsOff = kFileHeaderSize + rd16(b, 16);   // + optional header
    return l;
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

std::optional<std::string> export_name(std::string_view name, bool i386) {
    if (name.empty() || is_skipped_name(name)) return std::nullopt;
    std::string out(name);
    if (i386 && out.starts_with('_') && out.find('@') == std::string::npos)
        out.erase(0, 1);
    if (out.empty()) return std::nullopt;
    return out;
}

bool declares_exports(std::span<const std::byte> bytes) {
    // An object whose layout cannot be read declares nothing here; reading its
    // symbols reports why (`read_exports`).
    const auto layout = layout_of(bytes);
    if (!layout) return false;

    for (std::uint32_t i = 0; i < layout->numSections; ++i) {
        const auto hdr = layout->sectionsOff + std::size_t(i) * kSectionHeaderSize;
        if (hdr + kSectionHeaderSize > bytes.size()) return false;

        std::string name;
        for (std::size_t c = 0; c < 8; ++c) {
            auto ch = std::to_integer<char>(bytes[hdr + c]);
            if (ch == '\0') break;
            name.push_back(ch);
        }
        if (name != ".drectve") continue;

        const auto size = rd32(bytes, hdr + 16);
        const auto off  = rd32(bytes, hdr + 20);
        if (off == 0 || std::size_t(off) + size > bytes.size()) return false;

        // The section is plain text: a run of linker directives. Both spellings
        // occur — cl emits `/EXPORT:`, clang-cl `-export:` — and a linker
        // accepts either, so looking for one of them only would be a coin flip
        // on which compiler produced the object.
        std::string text;
        text.reserve(size);
        for (std::uint32_t k = 0; k < size; ++k)
            text.push_back(std::to_integer<char>(bytes[std::size_t(off) + k]));
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (text.find("/export:") != std::string::npos) return true;
        if (text.find("-export:") != std::string::npos) return true;
    }
    return false;
}

bool is_supported_machine(std::uint16_t machine) {
    return machine == kMachineI386  || machine == kMachineAmd64
        || machine == kMachineArm   || machine == kMachineArmNT
        || machine == kMachineArm64;
}

std::expected<std::vector<Export>, std::string>
read_exports(std::span<const std::byte> bytes)
{
    // An anonymous object is read when it is `/bigobj`, and named when it is
    // anything else: "unsupported machine 0x0000" would be true and useless.
    const auto layout = layout_of(bytes);
    if (!layout) return std::unexpected(layout.error());
    const auto machine = layout->machine;

    if (!is_supported_machine(machine)) {
        return std::unexpected(std::format(
            "unsupported COFF machine 0x{:04x}. mcpp reads i386, amd64, arm, "
            "armnt and arm64; anything else would be a guess at a layout this "
            "has never been run against.", machine));
    }

    const auto numSections   = layout->numSections;
    const auto symTableOff   = layout->symTableOff;
    const auto numSymbols    = layout->numSymbols;
    const auto symbolSize    = layout->symbolSize;
    if (symTableOff == 0 || numSymbols == 0) return std::vector<Export>{};

    // Section characteristics, indexed 1-based the way symbols address them.
    // A count no file of this size can hold is refused before it sizes a vector.
    const std::size_t sectionsOff = layout->sectionsOff;
    if (sectionsOff + std::size_t(numSections) * kSectionHeaderSize > bytes.size())
        return std::unexpected("COFF section headers run past the end of the file");
    std::vector<std::uint32_t> sectionFlags(std::size_t(numSections) + 1, 0);
    for (std::uint32_t i = 0; i < numSections; ++i) {
        const auto off = sectionsOff + std::size_t(i) * kSectionHeaderSize;
        if (off + kSectionHeaderSize > bytes.size())
            return std::unexpected("COFF section headers run past the end of the file");
        sectionFlags[i + 1] = rd32(bytes, off + 36);
    }

    const std::size_t symbolsEnd = std::size_t(symTableOff)
                                 + std::size_t(numSymbols) * symbolSize;
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
                              + std::size_t(i) * symbolSize;
        // `/bigobj` widens the section number to 32 bits, which moves the
        // three fields after it by two bytes.
        const std::size_t wide = layout->big ? 2 : 0;
        const auto sectionNum = layout->big
            ? static_cast<std::int32_t>(rd32(bytes, rec + 12))
            : static_cast<std::int32_t>(static_cast<std::int16_t>(rd16(bytes, rec + 12)));
        const auto type       = rd16(bytes, rec + 14 + wide);
        const auto storage    = std::to_integer<std::uint8_t>(bytes[rec + 16 + wide]);
        const auto numAux     = std::to_integer<std::uint8_t>(bytes[rec + 17 + wide]);

        // Advance past auxiliary records regardless of whether this symbol is
        // taken — an aux record is not a symbol and reading it as one produces
        // names out of raw section data.
        i += 1u + numAux;

        if (storage != kSymClassExternal) continue;   // not visible outside the TU
        if (sectionNum <= 0)              continue;   // undefined, absolute or debug
        if (type != kSymTypeFunction && type != 0) continue;

        auto nm = name_at(rec);
        if (!nm) return std::unexpected(nm.error());
        auto name = export_name(*nm, machine == kMachineI386);
        if (!name) continue;

        const auto flags = std::size_t(sectionNum) < sectionFlags.size()
                         ? sectionFlags[sectionNum] : 0u;
        // DATA when it is not code. Both halves matter: a function symbol in a
        // writable section is still a function, and a variable in a read-only
        // section (a `const`) is still data.
        const bool isData = type != kSymTypeFunction
                         && (flags & kScnMemExecute) == 0;
        out.push_back(Export{ std::move(*name), isData });
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
