// mcpp.pack.zip — write a .zip, from any host, with no external tool.
//
// WHY NOT SHELL OUT. The rest of `pack` produces `.tar.gz` by running `tar`,
// and the obvious symmetry would be to run something for zip too. There is no
// such something that exists everywhere: GNU tar (Linux) cannot write zip,
// `zip(1)` is frequently absent, PowerShell's `Compress-Archive` is
// Windows-only, and bsdtar is Windows-and-macOS-mostly. The earlier design
// (.agents/docs/2026-05-19-pack-windows-design.md) proposed `Compress-Archive`
// precisely because it assumed pack ran ON Windows — the assumption that
// mcpp.pack.binfmt exists to remove. A packer that only works on the target's
// own OS is the thing being fixed; picking an archiver that only exists there
// would reintroduce it one layer down.
//
// STORED OR DEFLATED. A member is DEFLATEd (method 8) by zlib, the reference
// encoder, when the archive asks for it and the member shrinks; otherwise it is
// stored. A package's zip is deflated by default (SPEC-014 §5); `--compress
// none` stores every member, and a member whose data is aligned (a mapped
// image) is stored whatever was asked.
//
// Deterministic by construction: no timestamps are read (every entry gets a
// fixed DOS date), and entries are written in the order given. Two packs of
// the same tree produce byte-identical archives, which is what makes a
// published checksum mean anything.

export module mcpp.pack.zip;

import std;
import mcpp.pack.compress;   // DEFLATE for a member that shrinks

export namespace mcpp::pack::zip {

struct Entry {
    // Path INSIDE the archive, always with forward slashes — the ZIP spec
    // says so (4.4.17.1), and a backslash here is what makes an archive
    // written on Windows extract into one flat file with a strange name
    // everywhere else.
    std::string           name;
    std::filesystem::path source;
    // Only meaningful to POSIX extractors; Windows ignores it. Set for the
    // executable so `unzip` on a Linux box does not hand back a non-runnable
    // file — which matters because a Windows artifact is routinely inspected
    // (and cross-tested under wine) from Linux.
    bool                  executable = false;
    // A symbolic link: the member is the link (Unix mode 0120777) and its data
    // is the target, as Info-ZIP writes and reads one. `source` is unread.
    std::string           symlinkTarget;
};

// How the members are written.
//
//   deflate  each member is DEFLATEd (method 8) when that makes it smaller,
//            and stored otherwise; `level` 0 is zlib's default.
//   align    a STORED member's data starts at a multiple of this many bytes
//            from the start of the archive, the local header's extra field
//            padding the difference (as zipalign does): a reader maps the
//            member where it lies. 0: no alignment. A member that is aligned
//            is stored, whatever `deflate` says.
struct WriteOptions {
    bool          deflate = false;
    int           level   = 0;
    std::uint32_t align   = 0;
};

// Write `entries` to `out`, creating parent directories as needed.
//
// Takes the vector by reference rather than `std::span<const Entry>`. The
// span is the better interface and it is not worth a compiler crash: passing
// one across this module boundary was one of three constructs present when
// clang 20.1.7 (MSVC ABI) segfaulted compiling the consumer — see the note at
// mcpp.pack's `run_pe`. There is exactly one caller.
std::expected<void, std::string>
write(const std::filesystem::path& out, const std::vector<Entry>& entries,
      const WriteOptions& options = {});

// CRC-32 (IEEE), exposed for tests: an archive whose CRCs are wrong extracts
// with a warning on some tools and silently on others, so this is worth
// pinning against known vectors rather than against itself.
std::uint32_t crc32(std::string_view data);

} // namespace mcpp::pack::zip

namespace mcpp::pack::zip {

namespace detail {

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void put16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// A fixed DOS timestamp: 1980-01-01 00:00:00, the earliest the format can
// express. Reading the file's mtime instead would make the archive depend on
// when it was built, which is the usual reason two identical builds produce
// two different checksums.
constexpr std::uint16_t kDosTime = 0;
constexpr std::uint16_t kDosDate = 0x0021;   // (1980-1980)<<9 | 1<<5 | 1

// External attributes. The high 16 bits are the Unix mode when the
// version-made-by byte says Unix (3); the low byte is the DOS attribute set.
std::uint32_t external_attrs(bool executable, bool symlink = false) {
    std::uint32_t mode = symlink ? 0120777u : executable ? 0100755u : 0100644u;
    return mode << 16;
}

std::optional<std::string> slurp(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) return std::nullopt;
    std::ostringstream ss;
    ss << is.rdbuf();
    return ss.str();
}

} // namespace detail

std::uint32_t crc32(std::string_view data) {
    auto const& table = detail::crc_table();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data)
        c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::expected<void, std::string>
write(const std::filesystem::path& out, const std::vector<Entry>& entries,
      const WriteOptions& options) {
    std::error_code ec;
    if (out.has_parent_path())
        std::filesystem::create_directories(out.parent_path(), ec);

    struct Central {
        std::string   name;
        std::uint32_t crc = 0, size = 0, packed = 0, offset = 0;
        std::uint16_t method = 0;
        bool          executable = false, symlink = false;
    };
    std::vector<Central> central;
    central.reserve(entries.size());

    std::string blob;
    for (auto const& e : entries) {
        const bool symlink = !e.symlinkTarget.empty();
        auto data = symlink ? std::optional<std::string>(e.symlinkTarget) : detail::slurp(e.source);
        if (!data)
            return std::unexpected(std::format(
                "cannot read '{}' for the archive", e.source.string()));
        // 4 GiB is the point where ZIP64 becomes mandatory. mcpp does not
        // write ZIP64, so refuse rather than emit an archive whose sizes have
        // silently wrapped — a truncated field here produces an archive that
        // extracts to garbage instead of failing.
        if (data->size() > 0xFFFFFFFFull || blob.size() > 0xFFFFFFFFull)
            return std::unexpected(std::format(
                "'{}' exceeds the 4 GiB limit of a non-ZIP64 archive",
                e.source.string()));

        Central c;
        c.name       = e.name;
        c.crc        = crc32(*data);
        c.size       = static_cast<std::uint32_t>(data->size());
        c.offset     = static_cast<std::uint32_t>(blob.size());
        c.executable = e.executable;
        c.symlink    = symlink;

        // The member's bytes: DEFLATEd when asked and smaller, stored
        // otherwise, and stored when its data is to be aligned.
        std::string packed;
        if (options.deflate && options.align == 0 && !symlink && !data->empty()) {
            auto d = mcpp::pack::compress::deflate_raw(*data, options.level);
            if (!d) return std::unexpected(std::format("'{}': {}", e.source.string(), d.error()));
            if (d->size() < data->size()) { packed = std::move(*d); c.method = 8; }
        }
        const std::string& body = c.method == 8 ? packed : *data;
        c.packed = static_cast<std::uint32_t>(body.size());

        // The padding that puts a stored member's data on the boundary, as
        // zipalign writes it: an extra field (id 0xD935) holding the alignment
        // and zero bytes. A gap too short for a field (under six bytes) lies
        // before the local header instead, where no directory entry names it
        // and a reader through the central directory never looks.
        std::string extraField;
        if (options.align > 1 && c.method == 0) {
            auto pad_at = [&](std::size_t headerAt) {
                const auto dataAt = headerAt + 30 + c.name.size();
                return (options.align - dataAt % options.align) % options.align;
            };
            auto need = pad_at(blob.size());
            if (need != 0 && need < 6) {
                blob.append(need, '\0');
                need = pad_at(blob.size());
                // After the gap the data is on the boundary; nothing more.
            }
            if (need >= 6) {
                detail::put16(extraField, 0xD935);
                detail::put16(extraField, static_cast<std::uint16_t>(need - 4));
                detail::put16(extraField, static_cast<std::uint16_t>(
                    options.align > 0xFFFF ? 0 : options.align));
                extraField.append(need - 6, '\0');
            }
            c.offset = static_cast<std::uint32_t>(blob.size());
        }

        detail::put32(blob, 0x04034b50);            // local file header
        detail::put16(blob, 20);                    // version needed (2.0)
        detail::put16(blob, 0);                     // flags — no data descriptor
        detail::put16(blob, c.method);              // 0 stored, 8 deflated
        detail::put16(blob, detail::kDosTime);
        detail::put16(blob, detail::kDosDate);
        detail::put32(blob, c.crc);
        detail::put32(blob, c.packed);
        detail::put32(blob, c.size);
        detail::put16(blob, static_cast<std::uint16_t>(c.name.size()));
        detail::put16(blob, static_cast<std::uint16_t>(extraField.size()));
        blob += c.name;
        blob += extraField;
        blob += body;

        central.push_back(std::move(c));
    }

    const auto centralAt = static_cast<std::uint32_t>(blob.size());
    for (auto const& c : central) {
        detail::put32(blob, 0x02014b50);            // central directory header
        // version made by: 3 (Unix) << 8 | 20. The Unix half is what makes the
        // mode bits in `external attributes` meaningful to an extractor.
        detail::put16(blob, (3 << 8) | 20);
        detail::put16(blob, 20);                    // version needed
        detail::put16(blob, 0);
        detail::put16(blob, c.method);
        detail::put16(blob, detail::kDosTime);
        detail::put16(blob, detail::kDosDate);
        detail::put32(blob, c.crc);
        detail::put32(blob, c.packed);
        detail::put32(blob, c.size);
        detail::put16(blob, static_cast<std::uint16_t>(c.name.size()));
        detail::put16(blob, 0);                     // extra
        detail::put16(blob, 0);                     // comment
        detail::put16(blob, 0);                     // disk number
        detail::put16(blob, 0);                     // internal attrs
        detail::put32(blob, detail::external_attrs(c.executable, c.symlink));
        detail::put32(blob, c.offset);
        blob += c.name;
    }
    const auto centralSize = static_cast<std::uint32_t>(blob.size()) - centralAt;

    detail::put32(blob, 0x06054b50);                // end of central directory
    detail::put16(blob, 0);
    detail::put16(blob, 0);
    detail::put16(blob, static_cast<std::uint16_t>(central.size()));
    detail::put16(blob, static_cast<std::uint16_t>(central.size()));
    detail::put32(blob, centralSize);
    detail::put32(blob, centralAt);
    detail::put16(blob, 0);                         // comment length

    std::ofstream os(out, std::ios::binary | std::ios::trunc);
    if (!os) return std::unexpected(std::format(
        "cannot write '{}'", out.string()));
    os.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (!os) return std::unexpected(std::format(
        "failed writing '{}'", out.string()));
    return {};
}

} // namespace mcpp::pack::zip
