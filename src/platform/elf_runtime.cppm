// mcpp.platform.elf_runtime — Linux ELF runtime-closure facts and physics.
//
// This module reads the bytes the loader will read.  It does not invoke
// readelf/patchelf/ldd and it does not infer a runtime from whichever payload
// happens to appear first in a directory.  Non-Linux callers get the same
// typed API, but validation is a no-op because ELF/glibc rules do not apply.

export module mcpp.platform.elf_runtime;

import std;
import mcpp.platform;
import mcpp.platform.runtime_binding;

export namespace mcpp::platform::elf {

struct ElfRuntimeFacts {
    std::filesystem::path artifact;
    std::uint16_t elfType = 0;
    std::string interp;
    std::vector<std::string> runpaths;
    std::vector<std::string> needed;
    std::vector<std::string> requiredGlibcVersions;
    std::vector<std::string> definedGlibcVersions;
    std::filesystem::path resolvedLibc;
    std::vector<std::filesystem::path> resolvedObjects;
};

struct RuntimeResolution {
    ElfRuntimeFacts artifact;
    std::vector<ElfRuntimeFacts> objects;
    std::vector<std::filesystem::path> resolvedLibcs;
    std::vector<std::string> unresolved;
};

struct RuntimeVerdict {
    enum class Status { Pass, ProvenMismatch, Inconclusive };
    Status status = Status::Pass;
    std::vector<std::string> diagnostics;

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

RuntimeResolution resolve_runtime_closure(
    const std::filesystem::path& artifact,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    std::span<const std::filesystem::path> additionalSearchDirs = {});

RuntimeVerdict validate_runtime_artifact(
    const std::filesystem::path& artifact,
    const mcpp::platform::runtime::RuntimeBinding& binding,
    const RuntimeResolution& resolution);

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
constexpr std::uint64_t kDtRunpath = 29;
constexpr std::uint64_t kDtVerdef = 0x6ffffffc;
constexpr std::uint64_t kDtVerdefnum = 0x6ffffffd;
constexpr std::uint64_t kDtVerneed = 0x6ffffffe;
constexpr std::uint64_t kDtVerneednum = 0x6fffffff;

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

} // namespace detail

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
        } else if (tag == detail::kDtRpath || tag == detail::kDtRunpath) {
            auto path = dynstr(value);
            if (!path) return std::unexpected(std::format(
                "artifact '{}' has an invalid DT_RPATH/DT_RUNPATH", artifact.string()));
            detail::append_path_list(
                tag == detail::kDtRunpath ? modernRunpaths : legacyRpaths, *path);
        }
    }
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
    detail::sort_unique(out.needed);
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

    std::deque<ElfRuntimeFacts> queue;
    queue.push_back(resolution.artifact);
    std::set<std::filesystem::path> visited;
    visited.insert(detail::comparable_path(artifact));
    constexpr std::size_t kMaxClosureObjects = 512;
    while (!queue.empty() && resolution.objects.size() < kMaxClosureObjects) {
        auto requester = std::move(queue.front());
        queue.pop_front();
        for (auto const& soname : requester.needed) {
            auto path = detail::resolve_needed(
                soname, requester, binding, additionalSearchDirs);
            if (!path) {
                resolution.unresolved.push_back(soname);
                continue;
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
            queue.push_back(*parsed);
            resolution.objects.push_back(std::move(*parsed));
        }
    }
    if (!queue.empty())
        resolution.unresolved.push_back("runtime closure exceeds 512 ELF objects");
    detail::sort_unique(resolution.unresolved);
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
    const RuntimeResolution& resolution) {
    RuntimeVerdict verdict;
    const bool isGlibc = binding.runtimeId.starts_with("glibc@");
    if constexpr (!mcpp::platform::is_linux) {
        verdict.diagnostics.push_back(
            "runtime physics: non-Linux platform; ELF/glibc rules are not applicable");
        return verdict;
    }
    if (binding.platform != "linux" || !isGlibc) {
        verdict.diagnostics.push_back(
            "runtime physics: selected runtime is not Linux/glibc; rules A/B are not applicable");
        return verdict;
    }

    const auto artifactPath = detail::canonical_text(artifact);
    const auto& facts = resolution.artifact;
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
        std::string names;
        for (auto const& name : resolution.unresolved) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        inconclusive(std::format(
            "runtime closure for {} is inconclusive; unresolved objects: {}",
            artifactPath, names));
    }
    return verdict;
}

} // namespace mcpp::platform::elf
