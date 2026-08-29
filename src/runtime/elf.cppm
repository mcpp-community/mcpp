// mcpp.runtime.elf — Linux ELF runtime-closure facts and physics.
//
// This module reads the bytes the loader will read.  It does not invoke
// readelf/patchelf/ldd and it does not infer a runtime from whichever payload
// happens to appear first in a directory.  Non-Linux callers get the same
// typed API, but validation is a no-op because ELF/glibc rules do not apply.

export module mcpp.runtime.elf;

import std;
import mcpp.platform;
import mcpp.runtime.binding;

export namespace mcpp::platform::elf {

// WHICH dynamic tag carried the search path, kept separately from the path
// list itself.
//
// `runpaths` below answers "where will the loader look"; this answers "how far
// does that reach". They are different questions and only the first one used to
// be recorded: both tags were folded into `runpaths` and the tag was dropped.
//
// DT_RUNPATH is consulted only for the object that carries it and for the
// dlopen() that object performs ITSELF. DT_RPATH is consulted for every dlopen
// anywhere in the process, at any depth. A GL program reaches its driver
// through three to four dlopen() calls that IT does not make -- libGLX.so.0
// makes them -- so with DT_RUNPATH the path is present and unreachable.
// Measured: same paths, tag flipped, egl/gles2/egl-surfaceless move from
// llvmpipe to the GPU.
//
// `Both` is a real state and must not silently read as `Rpath`: glibc ignores
// DT_RPATH whenever DT_RUNPATH is also present, and DT_RPATH-first is the
// common layout, so a reader that stops at the first hit reports the opposite
// of what the loader will do.
enum class SearchPathTag { None, Rpath, Runpath, Both };

std::string_view to_string(SearchPathTag tag) {
    switch (tag) {
        case SearchPathTag::None:    return "none";
        case SearchPathTag::Rpath:   return "DT_RPATH";
        case SearchPathTag::Runpath: return "DT_RUNPATH";
        case SearchPathTag::Both:    return "DT_RPATH+DT_RUNPATH";
    }
    return "none";
}

struct ElfRuntimeFacts {
    std::filesystem::path artifact;
    std::uint16_t elfType = 0;
    std::string interp;
    std::string soname;
    std::vector<std::string> runpaths;
    SearchPathTag searchPathTag = SearchPathTag::None;
    std::vector<std::string> needed;
    std::vector<std::string> requiredGlibcVersions;
    std::vector<std::string> definedGlibcVersions;
    std::filesystem::path resolvedLibc;
    std::vector<std::filesystem::path> resolvedObjects;

    // "Is this an executable" is PT_INTERP, not ET_EXEC: a PIE executable is
    // ET_DYN and therefore indistinguishable from a shared library by type
    // alone. The loader-tag contract splits exactly along this line, so the
    // predicate lives with the facts rather than in each caller.
    bool is_executable() const { return !interp.empty(); }
};

struct RuntimeResolution {
    ElfRuntimeFacts artifact;
    std::vector<ElfRuntimeFacts> objects;
    std::vector<std::filesystem::path> resolvedLibcs;

    // Everything that stopped the walk, as human-readable text. A mixed bag on
    // purpose: an object that could not be parsed, a closure that hit the size
    // cap, and a SONAME nothing provides all belong in the report.
    std::vector<std::string> unresolved;

    // The strict subset that means "a DT_NEEDED nothing on the search path
    // provides". SEPARATE because only this one is PROVABLE.
    //
    // `unresolved` also collects "I could not read this file" and "I stopped
    // after 512 objects", which are statements about the CHECK, not about the
    // artifact. Treating the whole bag as proof made a cross-built PE fail its
    // build: `crosswin.exe` is not ELF, that fact landed in `unresolved`, and a
    // "you are missing a library" verdict was issued for a file with no
    // DT_NEEDED at all. Caught by CI, not by reading.
    std::vector<std::string> unresolvedSonames;

    // Did the artifact itself parse as ELF? False ⇒ the ELF rules do not apply
    // to it, whatever the binding says. The binding describes the HOST; a cross
    // build's artifact is a different format entirely.
    bool artifactIsElf = false;
};

struct RuntimeVerdict {
    // FOUR-VALUED, and the third one is this round's addition.
    //
    //   Pass            every rule that applies was checked and held
    //   ProvenMismatch  two runtime payloads are being mixed
    //   Unresolvable    a DT_NEEDED cannot be found anywhere the artifact's
    //                   loader will look — the artifact provably cannot start
    //   Inconclusive    a rule that applies could not be evaluated
    //
    // `Unresolvable` used to be folded into `Inconclusive`, which reports a
    // PROVEN failure as "not checked". Under a hermetic binding the artifact's
    // PT_INTERP names a private loader whose search path mcpp computes in
    // full, so "not found" is a measurement, not an absence of one.
    enum class Status { Pass, ProvenMismatch, Unresolvable, Inconclusive };
    Status status = Status::Pass;
    std::vector<std::string> diagnostics;

    // Does this verdict mean the artifact is known-bad? Both blocking states
    // spelled once, so a caller cannot check for one and silently accept the
    // other.
    bool blocking() const {
        return status == Status::ProvenMismatch || status == Status::Unresolvable;
    }

    std::string explain() const {
        std::string out;
        for (auto const& diagnostic : diagnostics) {
            if (!out.empty()) out.push_back('\n');
            out += diagnostic;
        }
        return out;
    }
};

std::expected<ElfRuntimeFacts, std::string>
inspect_elf_runtime(const std::filesystem::path& artifact);

// One entry of a dynamic symbol table that this object DEFINES.
struct DynamicSymbol {
    std::string   name;
    // STT_FUNC (or STT_GNU_IFUNC). Load-bearing rather than informational: a
    // copy relocation moves DATA out of a shared library into the image that
    // links it, so it is never a function. A defined FUNC in an executable's
    // dynamic symbol table therefore has exactly one cause — the linker
    // exported it so that some shared object's reference would bind to it.
    bool          isFunc = false;
    std::uint64_t value = 0;   // st_value; the key a copy relocation matches
};

// What an ELF object's DYNAMIC symbol table says it provides.
//
// Deliberately not folded into ElfRuntimeFacts: every caller of
// inspect_elf_runtime would then pay for a symbol-table walk, and the runtime
// closure check — which runs on every changed artifact — does not need one.
struct DynamicSymbols {
    // GLOBAL/WEAK entries with st_shndx != SHN_UNDEF.
    std::vector<DynamicSymbol> defined;
    // The whole table, including undefined imports. This is the DENOMINATOR a
    // report quotes: "0 of 217" and "not measured" must not read the same.
    std::size_t                total = 0;
    // Addresses named by an R_<machine>_COPY relocation.
    //
    // ADDRESS, not name, and that distinction is measured rather than
    // reasoned: glibc's `environ` is a WEAK alias of `__environ` at the same
    // address, and only `__environ` appears in the relocation table. Matching
    // by name reports `environ` as a hijacked symbol in every single
    // dynamically linked executable.
    std::set<std::uint64_t>    copyRelocations;
    // Did we recognise this object's machine well enough to know its COPY
    // relocation type? False means the copy-relocation set above is EMPTY
    // BECAUSE IT WAS NOT COMPUTED, not because there are none — a caller must
    // report "not evaluated" rather than "clean".
    bool                       copyRelocationsKnown = false;
    // Does this object have a dynamic symbol table at all? A fully static
    // link has none, and "nothing to check" is a different answer from
    // "checked and found nothing".
    bool                       present = false;
};

// Read the dynamic symbol table and the copy relocations of one ELF object.
std::expected<DynamicSymbols, std::string>
inspect_dynamic_symbols(const std::filesystem::path& object);

RuntimeResolution resolve_runtime_closure(
    const std::filesystem::path& artifact,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    std::span<const std::filesystem::path> additionalSearchDirs = {});

// `hostLibsAllowed` mirrors `[build] allow_host_libs` (and
// `MCPP_ALLOW_HOST_LIBS`). It is the user's explicit statement that this build
// reaches outside the sandbox on purpose, and it already switches off the
// link-time hermeticity check. It has to switch off the RUN-time proof for the
// same reason: once resolution is the user's responsibility, mcpp can no longer
// claim the artifact is unstartable — they may run it under LD_LIBRARY_PATH, or
// on a machine where the library is installed where the private loader looks.
// One declaration, one meaning, both phases.
RuntimeVerdict validate_runtime_artifact(
    const std::filesystem::path& artifact,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    const RuntimeResolution& resolution,
    bool hostLibsAllowed = false);

} // namespace mcpp::platform::elf

namespace mcpp::platform::elf {
namespace detail {

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kPtInterp = 3;

constexpr std::uint64_t kDtNull = 0;
constexpr std::uint64_t kDtNeeded = 1;
constexpr std::uint64_t kDtStrtab = 5;
constexpr std::uint64_t kDtStrsz = 10;
constexpr std::uint64_t kDtRpath = 15;
constexpr std::uint64_t kDtSoname = 14;
constexpr std::uint64_t kDtRunpath = 29;
constexpr std::uint64_t kDtVerdef = 0x6ffffffc;
constexpr std::uint64_t kDtVerdefnum = 0x6ffffffd;
constexpr std::uint64_t kDtVerneed = 0x6ffffffe;
constexpr std::uint64_t kDtVerneednum = 0x6fffffff;
constexpr std::uint64_t kDtHash = 4;
constexpr std::uint64_t kDtSymtab = 6;
constexpr std::uint64_t kDtRela = 7;
constexpr std::uint64_t kDtRelasz = 8;
constexpr std::uint64_t kDtSyment = 11;
constexpr std::uint64_t kDtGnuHash = 0x6ffffef5;

// Elf64_Sym / Elf64_Rela are both 24 bytes. This reader is ELF64-only (the
// dynamic-entry stride above is 16), so there is no 32-bit variant to carry.
constexpr std::uint64_t kSymEntrySize  = 24;
constexpr std::uint64_t kRelaEntrySize = 24;

constexpr unsigned char kSttObject   = 1;
constexpr unsigned char kSttFunc     = 2;
constexpr unsigned char kSttGnuIfunc = 10;
constexpr unsigned char kStbLocal    = 0;

// The COPY relocation type, per machine.
//
// Machine-dependent by nature, and the table is EXHAUSTIVE-BY-REFUSAL rather
// than by guessing: an unlisted machine returns nullopt, and the caller then
// says "not evaluated" instead of treating every defined data symbol as a
// conflict. A wrong constant here would report a clean image as broken on an
// architecture nobody tested, which is worse than declining to answer.
std::optional<std::uint32_t> copy_relocation_type(std::uint16_t machine) {
    switch (machine) {
        case 3:   return 5;      // EM_386        R_386_COPY
        case 20:  return 19;     // EM_PPC        R_PPC_COPY
        case 21:  return 19;     // EM_PPC64      R_PPC64_COPY
        case 22:  return 9;      // EM_S390       R_390_COPY
        case 40:  return 20;     // EM_ARM        R_ARM_COPY
        case 62:  return 5;      // EM_X86_64     R_X86_64_COPY
        case 183: return 1024;   // EM_AARCH64    R_AARCH64_COPY
        case 243: return 4;      // EM_RISCV      R_RISCV_COPY
        case 258: return 4;      // EM_LOONGARCH  R_LARCH_COPY
        default:  return std::nullopt;
    }
}

struct Reader {
    std::vector<unsigned char> bytes;

    bool range(std::uint64_t off, std::uint64_t size) const {
        return off <= bytes.size() && size <= bytes.size() - off;
    }

    std::optional<std::uint16_t> u16(std::uint64_t off) const {
        if (!range(off, 2)) return std::nullopt;
        return static_cast<std::uint16_t>(bytes[off])
             | static_cast<std::uint16_t>(bytes[off + 1] << 8);
    }

    std::optional<std::uint32_t> u32(std::uint64_t off) const {
        if (!range(off, 4)) return std::nullopt;
        std::uint32_t value = 0;
        for (int i = 3; i >= 0; --i)
            value = (value << 8) | bytes[off + static_cast<std::uint64_t>(i)];
        return value;
    }

    std::optional<std::uint64_t> u64(std::uint64_t off) const {
        if (!range(off, 8)) return std::nullopt;
        std::uint64_t value = 0;
        for (int i = 7; i >= 0; --i)
            value = (value << 8) | bytes[off + static_cast<std::uint64_t>(i)];
        return value;
    }

    std::optional<std::string> cstr(std::uint64_t off,
                                    std::uint64_t limit) const {
        if (!range(off, 1) || limit == 0) return std::nullopt;
        const auto end = std::min<std::uint64_t>(bytes.size(), off + limit);
        std::string out;
        for (auto p = off; p < end; ++p) {
            if (bytes[p] == 0) return out;
            out.push_back(static_cast<char>(bytes[p]));
        }
        return std::nullopt;
    }
};

struct Segment {
    std::uint32_t type = 0;
    std::uint64_t offset = 0;
    std::uint64_t vaddr = 0;
    std::uint64_t filesz = 0;
};

std::optional<std::uint64_t>
vaddr_to_offset(std::span<const Segment> segments,
                std::uint64_t address,
                std::uint64_t size = 1) {
    for (auto const& segment : segments) {
        if (segment.type != kPtLoad || address < segment.vaddr) continue;
        auto delta = address - segment.vaddr;
        if (delta <= segment.filesz && size <= segment.filesz - delta)
            return segment.offset + delta;
    }
    return std::nullopt;
}

void sort_unique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void stable_unique(std::vector<std::string>& values) {
    std::vector<std::string> out;
    for (auto& value : values) {
        if (std::ranges::find(out, value) == out.end())
            out.push_back(std::move(value));
    }
    values = std::move(out);
}

void append_path_list(std::vector<std::string>& out, std::string_view value) {
    for (std::size_t start = 0; start <= value.size();) {
        auto end = value.find(':', start);
        auto item = value.substr(start, end == std::string_view::npos
            ? std::string_view::npos : end - start);
        if (!item.empty()) out.emplace_back(item);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
}

std::filesystem::path comparable_path(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

bool same_path(const std::filesystem::path& lhs,
               const std::filesystem::path& rhs) {
    return !lhs.empty() && !rhs.empty()
        && comparable_path(lhs) == comparable_path(rhs);
}

std::string canonical_text(const std::filesystem::path& path) {
    return comparable_path(path).generic_string();
}

std::string expand_origin(std::string value,
                          const std::filesystem::path& object) {
    const auto origin = object.parent_path().generic_string();
    for (auto token : {std::string_view("${ORIGIN}"), std::string_view("$ORIGIN")}) {
        for (auto pos = value.find(token); pos != std::string::npos;
             pos = value.find(token, pos + origin.size()))
            value.replace(pos, token.size(), origin);
    }
    return value;
}

void append_unique_path(std::vector<std::filesystem::path>& out,
                        std::filesystem::path path) {
    if (path.empty()) return;
    path = path.lexically_normal();
    if (std::ranges::find(out, path) == out.end()) out.push_back(std::move(path));
}

std::vector<std::filesystem::path> host_library_dirs() {
    std::vector<std::filesystem::path> out;
    const auto arch = std::string(mcpp::platform::host_arch);
    std::string triple;
    if (arch == "x86_64") triple = "x86_64-linux-gnu";
    else if (arch == "aarch64") triple = "aarch64-linux-gnu";
    if (!triple.empty()) {
        out.emplace_back(std::filesystem::path("/lib") / triple);
        out.emplace_back(std::filesystem::path("/usr/lib") / triple);
    }
    out.emplace_back("/lib64");
    out.emplace_back("/usr/lib64");
    out.emplace_back("/lib");
    out.emplace_back("/usr/lib");
    return out;
}

std::optional<std::filesystem::path> resolve_needed(
    std::string_view soname,
    const ElfRuntimeFacts& requester,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    std::span<const std::filesystem::path> additionalSearchDirs) {
    std::filesystem::path named(soname);
    std::error_code ec;
    if (named.has_parent_path()) {
        auto candidate = named.is_absolute()
            ? named : requester.artifact.parent_path() / named;
        if (std::filesystem::is_regular_file(candidate, ec))
            return comparable_path(candidate);
        return std::nullopt;
    }

    std::vector<std::filesystem::path> dirs;
    for (auto const& raw : requester.runpaths)
        append_unique_path(dirs, expand_origin(raw, requester.artifact));
    for (auto const& dir : additionalSearchDirs) append_unique_path(dirs, dir);
    for (auto const& dir : binding.libraryDirs) append_unique_path(dirs, dir);
    // NOTE: the SubOS farm is NOT read from the binding here.
    //
    // It reaches this function through `additionalSearchDirs`, which the caller
    // builds from the PLAN — and the plan is where the guards live (no farm for
    // a cross target, none for a non-ELF format). Reading `binding.searchDirs`
    // directly would put this host's x86_64 farm on the search path of an
    // aarch64 artifact that has no such entry in its DT_RPATH, and report a
    // pass the target machine will not honour. The model must look exactly
    // where the artifact looks, no wider.
    //
    // The host loader's built-in defaults — ONLY when the artifact runs under
    // the host loader.
    //
    // A hermetic artifact's PT_INTERP names a private loader compiled with a
    // different default path, so adding /usr/lib here models the wrong loader.
    // Measured: a GL program that cannot start was reported `validation: pass`
    // because the HOST happened to have libGL.so.1 and the model found it
    // there. When the model and the artifact disagree about which loader runs,
    // the model wins the report and the artifact wins reality.
    if (!binding.hermetic())
        for (auto const& dir : host_library_dirs()) append_unique_path(dirs, dir);

    for (auto const& dir : dirs) {
        auto candidate = dir / named;
        ec.clear();
        if (std::filesystem::is_regular_file(candidate, ec))
            return comparable_path(candidate);
    }
    return std::nullopt;
}

std::optional<std::vector<unsigned>> glibc_version(std::string_view value) {
    constexpr std::string_view prefix = "GLIBC_";
    if (!value.starts_with(prefix)) return std::nullopt;
    value.remove_prefix(prefix.size());
    if (value.empty()) return std::nullopt;
    std::vector<unsigned> parts;
    for (std::size_t start = 0; start <= value.size();) {
        auto end = value.find('.', start);
        auto part = value.substr(start, end == std::string_view::npos
            ? std::string_view::npos : end - start);
        if (part.empty() || !std::ranges::all_of(part, [](unsigned char c) {
                return std::isdigit(c) != 0;
            }))
            return std::nullopt;
        try {
            parts.push_back(static_cast<unsigned>(std::stoul(std::string(part))));
        } catch (...) {
            return std::nullopt;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

int compare_versions(std::span<const unsigned> lhs,
                     std::span<const unsigned> rhs) {
    const auto count = std::max(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < count; ++i) {
        auto a = i < lhs.size() ? lhs[i] : 0;
        auto b = i < rhs.size() ? rhs[i] : 0;
        if (a < b) return -1;
        if (a > b) return 1;
    }
    return 0;
}

// How many entries does `.dynsym` have?
//
// The dynamic section does not say. Both hash tables do, in different ways,
// and which one exists is a link-time choice mcpp does not make: its own
// binaries carry BOTH (`--hash-style=both`), while distribution binaries on
// this machine carry GNU_HASH alone — measured on /usr/bin/git and /usr/bin/ls.
// So both readers are required; neither is a fallback for exotic cases.
std::optional<std::uint64_t> dynsym_count_from_sysv_hash(
    const Reader& reader, std::uint64_t hashOffset) {
    // struct { uint32 nbucket; uint32 nchain; ... } — nchain IS the symbol
    // count, because every symbol occupies one chain slot.
    auto nchain = reader.u32(hashOffset + 4);
    if (!nchain) return std::nullopt;
    return *nchain;
}

std::optional<std::uint64_t> dynsym_count_from_gnu_hash(
    const Reader& reader, std::uint64_t hashOffset) {
    auto nbuckets   = reader.u32(hashOffset);
    auto symoffset  = reader.u32(hashOffset + 4);
    auto bloomSize  = reader.u32(hashOffset + 8);
    if (!nbuckets || !symoffset || !bloomSize) return std::nullopt;
    if (*nbuckets > (1u << 24) || *bloomSize > (1u << 24)) return std::nullopt;

    // GNU_HASH omits the first `symoffset` symbols (the undefined ones), so
    // with no hashed symbol at all the table is exactly that long.
    const auto bucketsAt = hashOffset + 16
                         + static_cast<std::uint64_t>(*bloomSize) * 8;
    std::uint32_t maxIndex = 0;
    for (std::uint32_t i = 0; i < *nbuckets; ++i) {
        auto bucket = reader.u32(bucketsAt + static_cast<std::uint64_t>(i) * 4);
        if (!bucket) return std::nullopt;
        maxIndex = std::max(maxIndex, *bucket);
    }
    if (maxIndex < *symoffset) return *symoffset;

    // Walk the chain of the highest bucket to its terminator (low bit set).
    const auto chainAt = bucketsAt + static_cast<std::uint64_t>(*nbuckets) * 4;
    std::uint32_t index = maxIndex;
    for (std::uint32_t guard = 0; guard < (1u << 24); ++guard) {
        auto word = reader.u32(chainAt
            + static_cast<std::uint64_t>(index - *symoffset) * 4);
        if (!word) return std::nullopt;
        if (*word & 1u) return static_cast<std::uint64_t>(index) + 1;
        ++index;
    }
    return std::nullopt;
}

} // namespace detail

std::expected<DynamicSymbols, std::string>
inspect_dynamic_symbols(const std::filesystem::path& object) {
    detail::Reader reader;
    std::ifstream input(object, std::ios::binary);
    if (!input) return std::unexpected(std::format(
        "cannot open ELF object '{}'", object.string()));
    reader.bytes.assign(std::istreambuf_iterator<char>(input), {});

    if (reader.bytes.size() < 0x40
        || reader.bytes[0] != 0x7f || reader.bytes[1] != 'E'
        || reader.bytes[2] != 'L' || reader.bytes[3] != 'F')
        return std::unexpected(std::format(
            "object '{}' is not ELF", object.string()));
    if (reader.bytes[4] != 2 || reader.bytes[5] != 1)
        return std::unexpected(std::format(
            "object '{}' is not ELF64 little-endian", object.string()));

    auto machine   = reader.u16(0x12);
    auto phoff     = reader.u64(0x20);
    auto phentsize = reader.u16(0x36);
    auto phnum     = reader.u16(0x38);
    if (!machine || !phoff || !phentsize || !phnum || *phentsize < 0x38
        || *phnum > 4096
        || !reader.range(*phoff, static_cast<std::uint64_t>(*phentsize) * *phnum))
        return std::unexpected(std::format(
            "object '{}' has a truncated ELF program table", object.string()));

    DynamicSymbols out;

    std::vector<detail::Segment> segments;
    std::optional<detail::Segment> dynamic;
    for (std::uint16_t i = 0; i < *phnum; ++i) {
        auto off     = *phoff + static_cast<std::uint64_t>(i) * *phentsize;
        auto ptype   = reader.u32(off);
        auto poff    = reader.u64(off + 0x08);
        auto pvaddr  = reader.u64(off + 0x10);
        auto pfilesz = reader.u64(off + 0x20);
        if (!ptype || !poff || !pvaddr || !pfilesz
            || !reader.range(*poff, *pfilesz))
            return std::unexpected(std::format(
                "object '{}' has a truncated ELF segment", object.string()));
        segments.push_back(detail::Segment{*ptype, *poff, *pvaddr, *pfilesz});
        if (*ptype == detail::kPtDynamic)
            dynamic = segments.back();
    }
    // No PT_DYNAMIC: a fully static link, or a relocatable object. There is no
    // dynamic symbol table to read, and `present` stays false so the caller
    // reports "not applicable" rather than "clean".
    if (!dynamic || dynamic->filesz % 16 != 0) return out;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
    for (std::uint64_t off = dynamic->offset;
         off + 16 <= dynamic->offset + dynamic->filesz; off += 16) {
        auto tag   = reader.u64(off);
        auto value = reader.u64(off + 8);
        if (!tag || !value) return std::unexpected(std::format(
            "object '{}' has a truncated dynamic entry", object.string()));
        if (*tag == detail::kDtNull) break;
        entries.emplace_back(*tag, *value);
    }
    auto first = [&](std::uint64_t tag) -> std::optional<std::uint64_t> {
        for (auto const& [candidate, value] : entries)
            if (candidate == tag) return value;
        return std::nullopt;
    };

    auto strtabAddr = first(detail::kDtStrtab);
    auto strtabSize = first(detail::kDtStrsz);
    auto symtabAddr = first(detail::kDtSymtab);
    if (!strtabAddr || !strtabSize || !symtabAddr) return out;

    auto strtab = detail::vaddr_to_offset(segments, *strtabAddr, *strtabSize);
    if (!strtab || !reader.range(*strtab, *strtabSize))
        return std::unexpected(std::format(
            "object '{}' has an unmappable dynamic string table", object.string()));

    const auto symEntrySize = first(detail::kDtSyment).value_or(detail::kSymEntrySize);
    if (symEntrySize < detail::kSymEntrySize) return std::unexpected(std::format(
        "object '{}' declares a {}-byte dynamic symbol entry", object.string(),
        symEntrySize));

    std::optional<std::uint64_t> count;
    if (auto hash = first(detail::kDtHash)) {
        if (auto at = detail::vaddr_to_offset(segments, *hash, 8))
            count = detail::dynsym_count_from_sysv_hash(reader, *at);
    }
    if (!count) {
        if (auto hash = first(detail::kDtGnuHash)) {
            if (auto at = detail::vaddr_to_offset(segments, *hash, 16))
                count = detail::dynsym_count_from_gnu_hash(reader, *at);
        }
    }
    // Neither hash table was readable. Refuse rather than guess a table
    // length: a short guess reports a clean image, which is the failure mode
    // this whole area is about.
    if (!count) return std::unexpected(std::format(
        "object '{}' has a dynamic symbol table whose length cannot be "
        "determined (neither DT_HASH nor DT_GNU_HASH is usable)",
        object.string()));
    if (*count > (1u << 24)) return std::unexpected(std::format(
        "object '{}' declares {} dynamic symbols", object.string(), *count));

    auto symtab = detail::vaddr_to_offset(segments, *symtabAddr,
                                          symEntrySize * *count);
    if (!symtab || !reader.range(*symtab, symEntrySize * *count))
        return std::unexpected(std::format(
            "object '{}' has an unmappable dynamic symbol table", object.string()));

    out.present = true;
    out.total = static_cast<std::size_t>(*count);
    for (std::uint64_t i = 0; i < *count; ++i) {
        const auto at = *symtab + i * symEntrySize;
        auto nameOffset = reader.u32(at);
        auto shndx      = reader.u16(at + 6);
        auto value      = reader.u64(at + 8);
        if (!nameOffset || !shndx || !value) return std::unexpected(std::format(
            "object '{}' has a truncated dynamic symbol", object.string()));
        if (*shndx == 0) continue;                       // SHN_UNDEF — imported
        const unsigned char info = reader.bytes[at + 4];
        const unsigned char bind = static_cast<unsigned char>(info >> 4);
        const unsigned char type = static_cast<unsigned char>(info & 0x0f);
        if (bind == detail::kStbLocal) continue;         // not participating
        if (type != detail::kSttObject && type != detail::kSttFunc
            && type != detail::kSttGnuIfunc) continue;   // sections, files, TLS
        auto name = *nameOffset < *strtabSize
            ? reader.cstr(*strtab + *nameOffset, *strtabSize - *nameOffset)
            : std::nullopt;
        if (!name || name->empty()) continue;
        out.defined.push_back(DynamicSymbol{
            .name   = std::move(*name),
            .isFunc = (type == detail::kSttFunc || type == detail::kSttGnuIfunc),
            .value  = *value,
        });
    }

    // Copy relocations. Absent DT_RELA simply means there are none, which is a
    // real answer — unlike an unrecognised machine, which is not.
    if (auto type = detail::copy_relocation_type(*machine)) {
        out.copyRelocationsKnown = true;
        auto relaAddr = first(detail::kDtRela);
        auto relaSize = first(detail::kDtRelasz);
        if (relaAddr && relaSize) {
            auto rela = detail::vaddr_to_offset(segments, *relaAddr, *relaSize);
            if (!rela || !reader.range(*rela, *relaSize))
                return std::unexpected(std::format(
                    "object '{}' has an unmappable relocation table", object.string()));
            for (std::uint64_t off = 0;
                 off + detail::kRelaEntrySize <= *relaSize;
                 off += detail::kRelaEntrySize) {
                auto offset = reader.u64(*rela + off);
                auto info   = reader.u64(*rela + off + 8);
                if (!offset || !info) return std::unexpected(std::format(
                    "object '{}' has a truncated relocation", object.string()));
                if (static_cast<std::uint32_t>(*info & 0xffffffffull) == *type)
                    out.copyRelocations.insert(*offset);
            }
        }
    }
    return out;
}

std::expected<ElfRuntimeFacts, std::string>
inspect_elf_runtime(const std::filesystem::path& artifact) {
    detail::Reader reader;
    std::ifstream input(artifact, std::ios::binary);
    if (!input) return std::unexpected(std::format(
        "cannot open ELF artifact '{}'", artifact.string()));
    reader.bytes.assign(std::istreambuf_iterator<char>(input), {});

    if (reader.bytes.size() < 0x40
        || reader.bytes[0] != 0x7f || reader.bytes[1] != 'E'
        || reader.bytes[2] != 'L' || reader.bytes[3] != 'F')
        return std::unexpected(std::format(
            "artifact '{}' is not ELF", artifact.string()));
    if (reader.bytes[4] != 2 || reader.bytes[5] != 1)
        return std::unexpected(std::format(
            "artifact '{}' is not ELF64 little-endian", artifact.string()));

    auto type = reader.u16(0x10);
    auto phoff = reader.u64(0x20);
    auto phentsize = reader.u16(0x36);
    auto phnum = reader.u16(0x38);
    if (!type || !phoff || !phentsize || !phnum || *phentsize < 0x38
        || *phnum > 4096
        || !reader.range(*phoff, static_cast<std::uint64_t>(*phentsize) * *phnum))
        return std::unexpected(std::format(
            "artifact '{}' has a truncated ELF program table", artifact.string()));

    ElfRuntimeFacts out;
    out.artifact = artifact;
    out.elfType = *type;
    std::vector<detail::Segment> segments;
    std::optional<detail::Segment> dynamic;
    for (std::uint16_t i = 0; i < *phnum; ++i) {
        auto off = *phoff + static_cast<std::uint64_t>(i) * *phentsize;
        auto ptype = reader.u32(off);
        auto poff = reader.u64(off + 0x08);
        auto pvaddr = reader.u64(off + 0x10);
        auto pfilesz = reader.u64(off + 0x20);
        if (!ptype || !poff || !pvaddr || !pfilesz
            || !reader.range(*poff, *pfilesz))
            return std::unexpected(std::format(
                "artifact '{}' has a truncated ELF segment", artifact.string()));
        detail::Segment segment{*ptype, *poff, *pvaddr, *pfilesz};
        segments.push_back(segment);
        if (*ptype == detail::kPtDynamic) dynamic = segment;
        if (*ptype == detail::kPtInterp) {
            auto value = reader.cstr(*poff, *pfilesz);
            if (!value || value->empty()) return std::unexpected(std::format(
                "artifact '{}' has an invalid PT_INTERP", artifact.string()));
            out.interp = std::move(*value);
        }
    }

    if (!dynamic) return out; // static ELF or relocatable object
    if (dynamic->filesz % 16 != 0) return std::unexpected(std::format(
        "artifact '{}' has a malformed PT_DYNAMIC", artifact.string()));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
    for (std::uint64_t off = dynamic->offset;
         off + 16 <= dynamic->offset + dynamic->filesz; off += 16) {
        auto tag = reader.u64(off);
        auto value = reader.u64(off + 8);
        if (!tag || !value) return std::unexpected(std::format(
            "artifact '{}' has a truncated dynamic entry", artifact.string()));
        if (*tag == detail::kDtNull) break;
        entries.emplace_back(*tag, *value);
    }
    auto first = [&](std::uint64_t tag) -> std::optional<std::uint64_t> {
        for (auto const& [candidate, value] : entries)
            if (candidate == tag) return value;
        return std::nullopt;
    };
    auto strtabAddr = first(detail::kDtStrtab);
    auto strtabSize = first(detail::kDtStrsz);
    if (!strtabAddr || !strtabSize) return std::unexpected(std::format(
        "artifact '{}' has no usable dynamic string table", artifact.string()));
    auto strtab = detail::vaddr_to_offset(segments, *strtabAddr, *strtabSize);
    if (!strtab || !reader.range(*strtab, *strtabSize))
        return std::unexpected(std::format(
            "artifact '{}' has an unmappable dynamic string table", artifact.string()));
    auto dynstr = [&](std::uint64_t offset) -> std::optional<std::string> {
        if (offset >= *strtabSize) return std::nullopt;
        return reader.cstr(*strtab + offset, *strtabSize - offset);
    };

    std::vector<std::string> legacyRpaths;
    std::vector<std::string> modernRunpaths;
    for (auto const& [tag, value] : entries) {
        if (tag == detail::kDtNeeded) {
            auto name = dynstr(value);
            if (!name || name->empty()) return std::unexpected(std::format(
                "artifact '{}' has an invalid DT_NEEDED", artifact.string()));
            out.needed.push_back(std::move(*name));
        } else if (tag == detail::kDtSoname) {
            auto name = dynstr(value);
            if (!name || name->empty()) return std::unexpected(std::format(
                "artifact '{}' has an invalid DT_SONAME", artifact.string()));
            out.soname = std::move(*name);
        } else if (tag == detail::kDtRpath || tag == detail::kDtRunpath) {
            auto path = dynstr(value);
            if (!path) return std::unexpected(std::format(
                "artifact '{}' has an invalid DT_RPATH/DT_RUNPATH", artifact.string()));
            detail::append_path_list(
                tag == detail::kDtRunpath ? modernRunpaths : legacyRpaths, *path);
        }
    }
    // Record WHICH tag was present before collapsing the two lists -- the
    // collapse below is lossy and the lost bit is the one the loader-tag
    // contract is about (see SearchPathTag). Presence is keyed on the tag
    // having been seen, so a DT_RPATH holding an empty string still counts as
    // present: the loader saw the tag either way.
    if (!legacyRpaths.empty() && !modernRunpaths.empty())
        out.searchPathTag = SearchPathTag::Both;
    else if (!modernRunpaths.empty())
        out.searchPathTag = SearchPathTag::Runpath;
    else if (!legacyRpaths.empty())
        out.searchPathTag = SearchPathTag::Rpath;

    // glibc ignores legacy DT_RPATH when DT_RUNPATH exists. Preserve that
    // effective distinction while exposing one ordered search-path vector.
    out.runpaths = modernRunpaths.empty()
        ? std::move(legacyRpaths) : std::move(modernRunpaths);

    if (auto address = first(detail::kDtVerneed)) {
        auto count = first(detail::kDtVerneednum).value_or(0);
        auto table = detail::vaddr_to_offset(segments, *address, 16);
        if (!table || count > 4096) return std::unexpected(std::format(
            "artifact '{}' has an invalid GNU version-need table", artifact.string()));
        auto current = *table;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto auxCount = reader.u16(current + 2);
            auto auxDelta = reader.u32(current + 8);
            auto next = reader.u32(current + 12);
            if (!auxCount || !auxDelta || !next || *auxCount > 4096
                || !reader.range(current, 16))
                return std::unexpected(std::format(
                    "artifact '{}' has a truncated GNU version-need table",
                    artifact.string()));
            auto aux = current + *auxDelta;
            for (std::uint16_t j = 0; j < *auxCount; ++j) {
                auto nameOffset = reader.u32(aux + 8);
                auto auxNext = reader.u32(aux + 12);
                if (!nameOffset || !auxNext || !reader.range(aux, 16))
                    return std::unexpected(std::format(
                        "artifact '{}' has a truncated GNU version requirement",
                        artifact.string()));
                if (auto name = dynstr(*nameOffset); name && name->starts_with("GLIBC_"))
                    out.requiredGlibcVersions.push_back(std::move(*name));
                if (j + 1 < *auxCount) {
                    if (*auxNext == 0) return std::unexpected(std::format(
                        "artifact '{}' has a broken GNU version requirement chain",
                        artifact.string()));
                    aux += *auxNext;
                }
            }
            if (i + 1 < count) {
                if (*next == 0) return std::unexpected(std::format(
                    "artifact '{}' has a broken GNU version-need chain",
                    artifact.string()));
                current += *next;
            }
        }
    }

    if (auto address = first(detail::kDtVerdef)) {
        auto count = first(detail::kDtVerdefnum).value_or(0);
        auto table = detail::vaddr_to_offset(segments, *address, 20);
        if (!table || count > 65536) return std::unexpected(std::format(
            "artifact '{}' has an invalid GNU version-definition table",
            artifact.string()));
        auto current = *table;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto auxDelta = reader.u32(current + 12);
            auto next = reader.u32(current + 16);
            if (!auxDelta || !next || !reader.range(current, 20))
                return std::unexpected(std::format(
                    "artifact '{}' has a truncated GNU version-definition table",
                    artifact.string()));
            auto nameOffset = reader.u32(current + *auxDelta);
            if (!nameOffset || !reader.range(current + *auxDelta, 8))
                return std::unexpected(std::format(
                    "artifact '{}' has a truncated GNU version definition",
                    artifact.string()));
            if (auto name = dynstr(*nameOffset); name && name->starts_with("GLIBC_"))
                out.definedGlibcVersions.push_back(std::move(*name));
            if (i + 1 < count) {
                if (*next == 0) return std::unexpected(std::format(
                    "artifact '{}' has a broken GNU version-definition chain",
                    artifact.string()));
                current += *next;
            }
        }
    }

    // Search order is loader physics, not presentation: sorting RUNPATH would
    // be capable of selecting a different libc than the process itself.
    detail::stable_unique(out.runpaths);
    // DT_NEEDED order is loader semantics. Reordering it can change which
    // payload wins when two dependency search paths contain the same SONAME.
    detail::stable_unique(out.needed);
    detail::sort_unique(out.requiredGlibcVersions);
    detail::sort_unique(out.definedGlibcVersions);
    return out;
}

RuntimeResolution resolve_runtime_closure(
    const std::filesystem::path& artifact,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    std::span<const std::filesystem::path> additionalSearchDirs) {
    RuntimeResolution resolution;
    auto root = inspect_elf_runtime(artifact);
    if (!root) {
        resolution.artifact.artifact = artifact;
        resolution.unresolved.push_back(root.error());
        return resolution;
    }
    resolution.artifact = std::move(*root);
    resolution.artifactIsElf = true;

    std::deque<ElfRuntimeFacts> queue;
    queue.push_back(resolution.artifact);
    std::set<std::filesystem::path> visited;
    visited.insert(detail::comparable_path(artifact));
    // The ELF loader maintains one process-global loaded-object namespace.
    // Once a SONAME has been mapped, a later requester reuses that object;
    // its own RUNPATH does not load a second file with the same SONAME.
    std::map<std::string, std::filesystem::path> loadedBySoname;
    if (!resolution.artifact.interp.empty()) {
        auto interp = detail::comparable_path(resolution.artifact.interp);
        loadedBySoname.emplace(interp.filename().string(), std::move(interp));
    }
    constexpr std::size_t kMaxClosureObjects = 512;
    while (!queue.empty() && resolution.objects.size() < kMaxClosureObjects) {
        auto requester = std::move(queue.front());
        queue.pop_front();
        for (auto const& soname : requester.needed) {
            std::optional<std::filesystem::path> path;
            if (auto loaded = loadedBySoname.find(soname);
                loaded != loadedBySoname.end()) {
                path = loaded->second;
            } else {
                path = detail::resolve_needed(
                    soname, requester, binding, additionalSearchDirs);
                if (!path) {
                    resolution.unresolved.push_back(soname);
                    resolution.unresolvedSonames.push_back(soname);
                    continue;
                }
                loadedBySoname.emplace(soname, *path);
            }
            if (soname == "libc.so.6") {
                if (resolution.artifact.resolvedLibc.empty())
                    resolution.artifact.resolvedLibc = *path;
                if (std::ranges::find(resolution.resolvedLibcs, *path)
                    == resolution.resolvedLibcs.end())
                    resolution.resolvedLibcs.push_back(*path);
            }
            resolution.artifact.resolvedObjects.push_back(*path);
            if (!visited.insert(*path).second) continue;
            auto parsed = inspect_elf_runtime(*path);
            if (!parsed) {
                resolution.unresolved.push_back(std::format(
                    "{} ({})", soname, parsed.error()));
                continue;
            }
            if (!parsed->soname.empty()) {
                if (auto loaded = loadedBySoname.find(parsed->soname);
                    loaded != loadedBySoname.end()
                    && !detail::same_path(loaded->second, *path)) {
                    // The file reached through this request aliases a SONAME
                    // that is already mapped. Model the loader's reuse and do
                    // not add a second closure object.
                    loadedBySoname[soname] = loaded->second;
                    continue;
                }
                loadedBySoname.emplace(parsed->soname, *path);
            }
            queue.push_back(*parsed);
            resolution.objects.push_back(std::move(*parsed));
        }
    }
    if (!queue.empty())
        resolution.unresolved.push_back("runtime closure exceeds 512 ELF objects");
    detail::sort_unique(resolution.unresolved);
    detail::sort_unique(resolution.unresolvedSonames);
    std::sort(resolution.artifact.resolvedObjects.begin(),
              resolution.artifact.resolvedObjects.end());
    resolution.artifact.resolvedObjects.erase(
        std::unique(resolution.artifact.resolvedObjects.begin(),
                    resolution.artifact.resolvedObjects.end()),
        resolution.artifact.resolvedObjects.end());
    return resolution;
}

RuntimeVerdict validate_runtime_artifact(
    const std::filesystem::path& artifact,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    const RuntimeResolution& resolution,
    bool hostLibsAllowed) {
    RuntimeVerdict verdict;
    // Provider dispatch, not a prefix match. These are ELF/glibc physics —
    // PT_INTERP, DT_RUNPATH, a private loader — and a `ucrt@…` identity is
    // not a glibc that is missing, it is a different provider whose runtime
    // this validator has no rules for.
    const bool isGlibc =
        mcpp::platform::runtime::runtime_provider(binding.runtimeId) == "glibc";
    if constexpr (!mcpp::platform::is_linux) {
        verdict.diagnostics.push_back(
            "runtime physics: non-Linux platform; ELF/glibc rules are not applicable");
        return verdict;
    }
    if (binding.platform != "linux" || !isGlibc) {
        // Two different reasons land here and they are not the same news.
        //
        //   declared, not glibc   the rules DO NOT APPLY (musl, macOS SDK,
        //                         ucrt) — nothing to check, so Pass.
        //   not declared          the rules CANNOT BE EVALUATED — the SubOS
        //                         never said what it is, so Inconclusive.
        //
        // Reporting the second as the first sends the reader looking for a
        // runtime they did not select, and quietly counts "unknown" as "fine".
        if (!binding.declared) {
            verdict.status = RuntimeVerdict::Status::Inconclusive;
            verdict.diagnostics.push_back(std::format(
                "runtime rules inconclusive: SubOS '{}' does not describe its "
                "runtime, so there is no identity to check the artifact against",
                binding.selection.subosName));
            return verdict;
        }
        verdict.diagnostics.push_back(
            "runtime physics: selected runtime is not Linux/glibc; rules A/B are not applicable");
        return verdict;
    }

    const auto artifactPath = detail::canonical_text(artifact);
    const auto& facts = resolution.artifact;

    // THE ARTIFACT'S FORMAT DECIDES, NOT THE BINDING'S.
    //
    // The binding describes this HOST — Linux, glibc, a private loader. A cross
    // build's artifact is a different format entirely, and ELF rules say
    // nothing about it. Without this, a Linux→Windows cross build reached the
    // ELF validator with `crosswin.exe`, the "not an ELF file" parse error sat
    // in `unresolved`, and the build was failed for a missing library on a file
    // that has no DT_NEEDED at all.
    if (!resolution.artifactIsElf) {
        verdict.diagnostics.push_back(std::format(
            "runtime physics: {} is not ELF; ELF/glibc rules are not applicable",
            artifactPath));
        return verdict;
    }

    // ET_REL and static ET_EXEC/ET_DYN files carry no dynamic closure.
    if (facts.interp.empty() && facts.needed.empty() && resolution.unresolved.empty())
        return verdict;

    auto inconclusive = [&](std::string diagnostic) {
        if (verdict.status == RuntimeVerdict::Status::Pass)
            verdict.status = RuntimeVerdict::Status::Inconclusive;
        verdict.diagnostics.push_back(std::move(diagnostic));
    };
    auto mismatch = [&](std::string diagnostic) {
        verdict.status = RuntimeVerdict::Status::ProvenMismatch;
        verdict.diagnostics.push_back(std::move(diagnostic));
    };

    // Rule B: PT_INTERP, the libc actually found by the artifact's search
    // order, and the RuntimeBinding payload must all name one directory.
    std::filesystem::path expectedLoader = binding.loader.value_or(
        std::filesystem::path{});
    std::filesystem::path expectedLibDir;
    if (!expectedLoader.empty()) expectedLibDir = expectedLoader.parent_path();
    if (expectedLibDir.empty() && !binding.libraryDirs.empty())
        expectedLibDir = binding.libraryDirs.front();

    if (!facts.interp.empty()) {
        if (expectedLoader.empty()) {
            inconclusive(std::format(
                "rule B inconclusive for {}: RuntimeBinding {} has no loader path",
                artifactPath, binding.runtimeId));
        } else if (!detail::same_path(facts.interp, expectedLoader)) {
            mismatch(std::format(
                "rule B: artifact {} uses PT_INTERP {} but RuntimeBinding {} "
                "selects {}; one process cannot mix runtime payloads.\n"
                "       Fix: select/create a compatible SubOS in mcpp.toml "
                "([xlings] subos = \"<name>\").",
                artifactPath, detail::canonical_text(facts.interp), binding.runtimeId,
                detail::canonical_text(expectedLoader)));
        }
    }
    if (facts.resolvedLibc.empty()) {
        inconclusive(std::format(
            "rule B inconclusive for {}: libc.so.6 could not be resolved from "
            "the artifact search path", artifactPath));
    } else if (expectedLibDir.empty()) {
        inconclusive(std::format(
            "rule B inconclusive for {}: RuntimeBinding {} has no library directory",
            artifactPath, binding.runtimeId));
    } else if (!detail::same_path(facts.resolvedLibc.parent_path(), expectedLibDir)) {
        mismatch(std::format(
            "rule B: artifact {} resolves libc from {} while RuntimeBinding {} "
            "selects {}; one process cannot have two libcs.\n"
            "       Fix: rebuild after selecting a compatible SubOS in mcpp.toml "
            "([xlings] subos = \"<name>\").",
            artifactPath, detail::canonical_text(facts.resolvedLibc),
            binding.runtimeId, detail::canonical_text(expectedLibDir)));
    }
    for (auto const& libc : resolution.resolvedLibcs) {
        if (facts.resolvedLibc.empty() || detail::same_path(libc, facts.resolvedLibc))
            continue;
        mismatch(std::format(
            "rule B: artifact closure {} resolves more than one libc payload: "
            "{} and {}; one process cannot have two libcs.",
            artifactPath, detail::canonical_text(facts.resolvedLibc),
            detail::canonical_text(libc)));
    }

    // A proven Rule B mismatch is already terminal and does not need version
    // arithmetic to make it more true. Continue only for same-payload data.
    if (verdict.status == RuntimeVerdict::Status::ProvenMismatch) return verdict;

    const ElfRuntimeFacts* selectedLibc = nullptr;
    for (auto const& object : resolution.objects) {
        if (detail::same_path(object.artifact, facts.resolvedLibc)) {
            selectedLibc = &object;
            break;
        }
    }
    std::optional<std::vector<unsigned>> providedFloor;
    std::string providedName;
    if (selectedLibc) {
        for (auto const& name : selectedLibc->definedGlibcVersions) {
            auto parsed = detail::glibc_version(name);
            if (!parsed) continue;
            if (!providedFloor
                || detail::compare_versions(*providedFloor, *parsed) < 0) {
                providedFloor = std::move(*parsed);
                providedName = name;
            }
        }
    }
    if (!providedFloor) {
        inconclusive(std::format(
            "rule A inconclusive for {}: selected libc {} has no readable GNU "
            "version definitions", artifactPath,
            detail::canonical_text(facts.resolvedLibc)));
    } else {
        auto check_requester = [&](const ElfRuntimeFacts& requester) {
            for (auto const& name : requester.requiredGlibcVersions) {
                auto required = detail::glibc_version(name);
                if (!required) continue;
                if (detail::compare_versions(*providedFloor, *required) >= 0) continue;
                mismatch(std::format(
                    "rule A: requester {} needs {} but selected provider {} "
                    "exports only through {} (artifact {}).\n"
                    "       Fix: create/select a runtime at least {} and rebuild, e.g. "
                    "`xlings subos new <name> --runtime glibc@{}` then set "
                    "`[xlings] subos = \"<name>\"` in mcpp.toml.",
                    detail::canonical_text(requester.artifact), name,
                    detail::canonical_text(facts.resolvedLibc), providedName,
                    artifactPath, name.substr(std::string_view("GLIBC_").size()),
                    name.substr(std::string_view("GLIBC_").size())));
            }
        };
        check_requester(facts);
        for (auto const& object : resolution.objects) check_requester(object);
    }

    if (verdict.status != RuntimeVerdict::Status::ProvenMismatch
        && !resolution.unresolved.empty()) {
        auto join = [](std::span<const std::string> values) {
            std::string out;
            for (auto const& value : values) {
                if (!out.empty()) out += ", ";
                out += value;
            }
            return out;
        };
        // PROVEN under a hermetic binding, merely UNKNOWN otherwise.
        //
        // Hermetic means the artifact's PT_INTERP is a private loader whose
        // entire search path mcpp computed: RPATH/RUNPATH + payloads + farm,
        // with no host defaults and no ld.so.cache. Nothing else will be
        // consulted, so "not found here" is the same answer the loader will
        // give — and the artifact cannot start. Saying `inconclusive` for that
        // is reporting a measurement as the absence of one, and it is exactly
        // how a GL program that exits 127 was shipped as `validation: pass`.
        //
        // A non-hermetic artifact runs under the host loader, which also
        // consults `ld.so.cache` — something mcpp deliberately does not parse.
        // There, unresolved really is unknown.
        //
        // And it must be an unfindable SONAME, not merely "something stopped
        // the walk": an unreadable object or the 512-object cap are statements
        // about the CHECK, and a check that could not look has proven nothing.
        if (binding.hermetic() && !resolution.unresolvedSonames.empty()
            && !hostLibsAllowed) {
            verdict.status = RuntimeVerdict::Status::Unresolvable;
            verdict.diagnostics.push_back(std::format(
                "runtime closure for {} cannot be satisfied: {} not found on the "
                "search path this artifact will actually use.\n"
                "       Its PT_INTERP is a private loader, so the host's "
                "/usr/lib is NOT consulted — the program will fail to start with "
                "\"cannot open shared object file\".\n"
                "       Fix: install the provider into the selected SubOS "
                "(`xlings install <pkg>`), or declare the dependency so mcpp "
                "resolves it.",
                artifactPath, join(resolution.unresolvedSonames)));
        } else if (hostLibsAllowed && !resolution.unresolvedSonames.empty()) {
            inconclusive(std::format(
                "runtime closure for {} is inconclusive: {} is not on the search "
                "path this artifact will use, but [build] allow_host_libs is set "
                "— resolution at run time is yours to arrange (e.g. "
                "LD_LIBRARY_PATH, or installing it where the private loader "
                "looks).\n"
                "       Measured: this loader's built-in default path is the "
                "glibc payload's own prefix, NOT /usr/lib.",
                artifactPath, join(resolution.unresolvedSonames)));
        } else {
            inconclusive(std::format(
                "runtime closure for {} is inconclusive; unresolved objects: {}",
                artifactPath, join(resolution.unresolved)));
        }
    }
    return verdict;
}

} // namespace mcpp::platform::elf
