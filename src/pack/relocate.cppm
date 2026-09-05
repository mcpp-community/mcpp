// mcpp.pack.relocate — removing the BUILD MACHINE from a distributable image.
//
// THE RULE IS "THE TAG IS GONE", NOT "THE PATH IS RELATIVE"
//
// A dev build bakes the toolchain's own directories into every artifact:
//
//   DT_RUNPATH = <home>/registry/data/xpkgs/xim-x-glibc/2.44/lib64
//              : <home>/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64
//              : <home>/registry/subos/default/lib
//
// That is correct for a dev build and wrong for a package, and the obvious fix
// — rewrite it to `$ORIGIN` — DOES NOT WORK. Measured on a real `mcpp pack`
// product consumed by a real mcpp-built program, with the build machine's
// store made unreachable (issue #460):
//
//   state on the shipped .so         consumer's DT_RPATH inherited?   result
//   ------------------------------   -----------------------------   ------
//   DT_RUNPATH = <stale absolute>    no                              127
//   no tag at all                    YES                             ok
//   DT_RUNPATH = $ORIGIN             no                              127
//   DT_RUNPATH = "" (empty string)   no                              127
//   DT_RPATH  = <stale absolute>     yes                             ok
//
// The ELF loader drops the whole inherited DT_RPATH chain for any object that
// carries a DT_RUNPATH — whatever that RUNPATH says. So a non-empty
// replacement is not "less wrong", it is exactly as broken as the original,
// and an EMPTY one (which is what `patchelf --set-rpath ''` writes, and what
// any "clear the string" tool leaves behind) is broken too.
//
// Removing the tag is not merely the least-bad option, it is the RIGHT one:
// the consumer's own DT_RPATH is the same closure — payload, package dir,
// SubOS farm — resolved on the machine that will actually run it.
//
// WHY THIS EDITS BYTES INSTEAD OF SHELLING OUT TO patchelf
//
// A library package is cross-target by construction (`--target` is repeatable,
// and `run_library_pack` has no host gate because it never runs the artifact).
// `sandbox_patchelf` resolves only under a Linux xlings store, so a macOS or
// Windows host producing an ELF leg would find nothing — and the application
// packer's shape for that case is `if (!patchelf.empty())`, i.e. SILENTLY DO
// NOTHING. Copying that shape here would hand half of the hosts the very
// defect this module exists to remove.
//
// The edit needed is also far smaller than what patchelf is built for: delete
// one entry from the `Elf*_Dyn` array by moving the tail up one slot and
// padding the end with DT_NULL. Same file length, no segment moves, no offset
// fixups.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// The path STRING stays in `.dynstr`. Nothing points at it any more, so it is
// not reachable by the loader — but `strings` still finds it. That is not
// sloppiness and it is not a regression against the reference tool: measured,
// `patchelf --remove-rpath` leaves the identical residue at the identical file
// size. `.dynstr` is TAIL-MERGED by the linker, so a shorter live string may
// begin inside the dead one, and removing those bytes cannot be shown safe
// without a full reference analysis of `.dynstr`'s three consumers (DT_*
// strings, `.dynsym` names, `.gnu.version_r`). Silently truncating a symbol
// name is a worse outcome than a dead string.
//
// CONSEQUENCE FOR ANY GUARD: "this artifact carries no build-machine path"
// must be asked of the DYNAMIC ENTRIES, never of the file's bytes. A
// byte-pattern check reports a correctly relocated artifact as dirty.

export module mcpp.pack.relocate;

import std;
import mcpp.pack.binfmt;

export namespace mcpp::pack::relocate {

// What happened to one image.
//
// FIVE-VALUED, and the last two are the point: an image this module could not
// analyse has NOT been shown to be clean, and reporting that as `NothingToDo`
// would make "nothing was wrong" and "nothing was checked" the same answer.
enum class Outcome {
    Removed,        // an ELF search-path tag was present and is now gone
    NothingToDo,    // ELF, analysed, carried no DT_RPATH / DT_RUNPATH
    NotApplicable,  // PE, an archive, or not an image — no such concept
    Reported,       // Mach-O: entries were read and are reported, bytes untouched
    Unanalysed,     // recognised as an image, but this module cannot read it
};

struct Report {
    Outcome                  outcome = Outcome::NotApplicable;
    // What the removed (ELF) or found (Mach-O) entries said, for the log.
    std::vector<std::string> paths;
    // Why, when `outcome == Unanalysed`. Empty otherwise.
    std::string              note;
};

std::string_view describe(Outcome o);

// Remove every loader search-path entry from `image`, in place.
//
// Errors are reserved for "the file is there and something is wrong with it"
// (unreadable, truncated, not writable). A file that simply has no such
// concept — a `.a`, a `.dll`, a `.lib` — is `NotApplicable`, not an error:
// the caller packs every leg through the same step and must not have to know
// the format first.
std::expected<Report, std::string> strip_search_paths(const std::filesystem::path& image);

} // namespace mcpp::pack::relocate

namespace mcpp::pack::relocate {

namespace {

// ── endian- and class-aware byte access ─────────────────────────────────
//
// ELF32 and big-endian are not hypothetical for mcpp: a `--target` list can
// name a 32-bit or big-endian triple, and the packer is the same code for all
// of them. Getting this wrong would corrupt an artifact rather than refuse it,
// so the accessors are parameterised rather than assumed.
struct Bytes {
    std::string* d = nullptr;
    bool         little = true;

    std::uint64_t read(std::size_t off, std::size_t width) const {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < width; ++i) {
            auto byte = static_cast<std::uint8_t>((*d)[off + (little ? i : width - 1 - i)]);
            v |= static_cast<std::uint64_t>(byte) << (8 * i);
        }
        return v;
    }
    void write(std::size_t off, std::size_t width, std::uint64_t v) {
        for (std::size_t i = 0; i < width; ++i) {
            auto byte = static_cast<char>((v >> (8 * i)) & 0xFF);
            (*d)[off + (little ? i : width - 1 - i)] = byte;
        }
    }
    bool has(std::size_t off, std::size_t len) const {
        return d && off <= d->size() && d->size() - off >= len;
    }
};

std::optional<std::string> slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof()) return std::nullopt;
    return ss.str();
}

bool spit(const std::filesystem::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// Read a NUL-terminated string at `off`, bounded by the file.
std::string cstr_at(const std::string& d, std::size_t off) {
    if (off >= d.size()) return {};
    auto end = d.find('\0', off);
    return d.substr(off, end == std::string::npos ? std::string::npos : end - off);
}

constexpr std::uint64_t kDtNull    = 0;
constexpr std::uint64_t kDtStrtab  = 5;
constexpr std::uint64_t kDtRpath   = 15;
constexpr std::uint64_t kDtRunpath = 29;
constexpr std::uint32_t kPtLoad    = 1;
constexpr std::uint32_t kPtDynamic = 2;

std::expected<Report, std::string> strip_elf(const std::filesystem::path& image,
                                             std::string data)
{
    const bool elf64 = static_cast<std::uint8_t>(data[4]) == 2;
    const std::uint8_t dataEnc = static_cast<std::uint8_t>(data[5]);
    if (dataEnc != 1 && dataEnc != 2)
        return std::unexpected(std::format(
            "'{}' declares an unknown ELF data encoding ({})", image.string(), dataEnc));

    Bytes b{ &data, dataEnc == 1 };
    const std::size_t ptrW = elf64 ? 8 : 4;

    // e_phoff / e_phentsize / e_phnum — the two layouts differ only in offsets.
    const std::size_t phoffOff = elf64 ? 0x20 : 0x1C;
    const std::size_t phentOff = elf64 ? 0x36 : 0x2A;
    const std::size_t phnumOff = elf64 ? 0x38 : 0x2C;
    if (!b.has(phnumOff, 2)) return std::unexpected(std::format(
        "'{}' is truncated before its ELF header ends", image.string()));

    const auto phoff = b.read(phoffOff, ptrW);
    const auto phent = b.read(phentOff, 2);
    const auto phnum = b.read(phnumOff, 2);
    if (phoff == 0 || phent == 0 || phnum == 0)
        return Report{ Outcome::NothingToDo, {}, {} };   // no program headers: no PT_DYNAMIC

    // Program-header field offsets. p_offset/p_vaddr/p_filesz sit in different
    // places in the two classes because ELF64 moved p_flags forward.
    const std::size_t poOff = elf64 ? 0x08 : 0x04;
    const std::size_t pvOff = elf64 ? 0x10 : 0x08;
    const std::size_t pfOff = elf64 ? 0x20 : 0x10;

    std::optional<std::uint64_t> dynOff, dynSize;
    struct Load { std::uint64_t off, vaddr, filesz; };
    std::vector<Load> loads;
    for (std::uint64_t i = 0; i < phnum; ++i) {
        const auto ph = phoff + i * phent;
        if (!b.has(static_cast<std::size_t>(ph), static_cast<std::size_t>(phent)))
            return std::unexpected(std::format(
                "'{}' has a program-header table that runs past the file",
                image.string()));
        const auto type = static_cast<std::uint32_t>(b.read(static_cast<std::size_t>(ph), 4));
        if (type == kPtDynamic) {
            dynOff  = b.read(static_cast<std::size_t>(ph + poOff), ptrW);
            dynSize = b.read(static_cast<std::size_t>(ph + pfOff), ptrW);
        } else if (type == kPtLoad) {
            loads.push_back({ b.read(static_cast<std::size_t>(ph + poOff), ptrW),
                              b.read(static_cast<std::size_t>(ph + pvOff), ptrW),
                              b.read(static_cast<std::size_t>(ph + pfOff), ptrW) });
        }
    }
    if (!dynOff || !dynSize) return Report{ Outcome::NothingToDo, {}, {} };

    // Walk the Elf*_Dyn array up to and including its DT_NULL terminator.
    const std::size_t slotW = ptrW * 2;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> slots;   // (tag, value)
    for (std::uint64_t at = *dynOff; at + slotW <= *dynOff + *dynSize; at += slotW) {
        if (!b.has(static_cast<std::size_t>(at), slotW))
            return std::unexpected(std::format(
                "'{}' has a PT_DYNAMIC that runs past the file", image.string()));
        auto tag = b.read(static_cast<std::size_t>(at), ptrW);
        auto val = b.read(static_cast<std::size_t>(at + ptrW), ptrW);
        slots.push_back({ tag, val });
        if (tag == kDtNull) break;
    }
    if (slots.empty()) return Report{ Outcome::NothingToDo, {}, {} };

    // DT_STRTAB is a VIRTUAL address; map it through PT_LOAD so the removed
    // paths can be named in the log. Purely for reporting — a failure to map
    // it must not stop the removal.
    std::optional<std::uint64_t> strtabFileOff;
    for (auto const& [tag, val] : slots) {
        if (tag != kDtStrtab) continue;
        for (auto const& l : loads) {
            if (val >= l.vaddr && val - l.vaddr < l.filesz) {
                strtabFileOff = l.off + (val - l.vaddr);
                break;
            }
        }
    }

    Report report;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kept;
    for (auto const& slot : slots) {
        if (slot.first == kDtRpath || slot.first == kDtRunpath) {
            if (strtabFileOff)
                report.paths.push_back(cstr_at(data, static_cast<std::size_t>(*strtabFileOff + slot.second)));
            continue;
        }
        kept.push_back(slot);
    }
    if (kept.size() == slots.size()) return Report{ Outcome::NothingToDo, {}, {} };

    // Same byte length: the freed slots become DT_NULL padding, which is what
    // a linker writes there anyway. Nothing after PT_DYNAMIC moves, so no
    // section header, segment offset or relocation needs touching.
    kept.resize(slots.size(), { kDtNull, 0 });
    for (std::size_t i = 0; i < kept.size(); ++i) {
        const auto at = *dynOff + i * slotW;
        b.write(static_cast<std::size_t>(at), ptrW, kept[i].first);
        b.write(static_cast<std::size_t>(at + ptrW), ptrW, kept[i].second);
    }

    if (!spit(image, data))
        return std::unexpected(std::format("cannot write '{}'", image.string()));
    report.outcome = Outcome::Removed;
    return report;
}

// Mach-O: READ ONLY.
//
// The same defect can exist here as LC_RPATH, and the same edit is possible in
// principle — but mcpp has no macOS artifact to measure it on in the suite
// that gates this code, and shipping an untested byte-editor for a format
// whose load commands carry their own sizes is how a package becomes
// unloadable instead of unportable. So this reads them and hands them back;
// the caller decides what to say. `install_name_tool -delete_rpath` is the
// documented manual step until a macOS-gated test exists.
std::expected<Report, std::string> report_macho(const std::filesystem::path& image,
                                                const std::string& d)
{
    constexpr std::uint32_t kMagic64 = 0xFEEDFACFu, kMagic32 = 0xFEEDFACEu;
    constexpr std::uint32_t kCigam64 = 0xCFFAEDFEu, kCigam32 = 0xCEFAEDFEu;
    constexpr std::uint32_t kLcRpath = 0x1Cu;
    if (d.size() < 32) return std::unexpected(std::format(
        "'{}' is too short to be a Mach-O image", image.string()));

    auto raw32 = [&](std::size_t off, bool little) {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[off + (little ? i : 3 - i)])) << (8 * i);
        return v;
    };
    const auto magicLE = raw32(0, true);
    bool little = true, wide = true;
    if      (magicLE == kMagic64) { little = true;  wide = true;  }
    else if (magicLE == kMagic32) { little = true;  wide = false; }
    else if (magicLE == kCigam64) { little = false; wide = true;  }
    else if (magicLE == kCigam32) { little = false; wide = false; }
    else return Report{ Outcome::Unanalysed, {},
        "a universal (fat) Mach-O; its per-architecture images are not walked" };

    const auto ncmds      = raw32(16, little);
    const auto sizeofcmds = raw32(20, little);
    std::size_t at = wide ? 32 : 28;
    if (at + sizeofcmds > d.size()) return std::unexpected(std::format(
        "'{}' has a Mach-O load-command table that runs past the file", image.string()));

    Report report;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (at + 8 > d.size()) break;
        const auto cmd     = raw32(at, little);
        const auto cmdsize = raw32(at + 4, little);
        if (cmdsize < 8 || at + cmdsize > d.size()) break;
        if (cmd == kLcRpath) {
            const auto strOff = raw32(at + 8, little);
            if (strOff < cmdsize) report.paths.push_back(cstr_at(d, at + strOff));
        }
        at += cmdsize;
    }
    report.outcome = Outcome::Reported;
    return report;
}

} // namespace

std::string_view describe(Outcome o) {
    switch (o) {
        case Outcome::Removed:       return "removed";
        case Outcome::NothingToDo:   return "already clean";
        case Outcome::NotApplicable: return "not applicable";
        case Outcome::Reported:      return "reported";
        case Outcome::Unanalysed:    return "not analysed";
    }
    return "not analysed";
}

std::expected<Report, std::string> strip_search_paths(const std::filesystem::path& image)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(image, ec))
        return std::unexpected(std::format("'{}' is not a file", image.string()));

    auto data = slurp(image);
    if (!data) return std::unexpected(std::format("cannot read '{}'", image.string()));
    if (data->size() < 8) return Report{ Outcome::NotApplicable, {}, {} };

    // The format is asked of the BYTES, never of the host or of the file
    // extension: a Linux host packing a `--target x86_64-windows-gnu` leg holds
    // a PE, and a `.so` built for another architecture is still an ELF.
    switch (mcpp::pack::binfmt::identify(image).format) {
        case mcpp::pack::binfmt::Format::Elf:
            return strip_elf(image, std::move(*data));
        case mcpp::pack::binfmt::Format::MachO:
            return report_macho(image, *data);
        case mcpp::pack::binfmt::Format::Pe:
            // PE has no loader search path baked into the image at all — the
            // DLL search order is the process's, not the file's. Nothing to
            // remove, and that is an answer rather than a gap.
            return Report{ Outcome::NotApplicable, {}, {} };
        case mcpp::pack::binfmt::Format::Unknown:
            break;
    }
    // A static archive lands here, and so does anything else that is not an
    // image. Both are `NotApplicable`: an archive's members are relocatable
    // objects, which carry no dynamic section to begin with.
    return Report{ Outcome::NotApplicable, {}, {} };
}

} // namespace mcpp::pack::relocate
