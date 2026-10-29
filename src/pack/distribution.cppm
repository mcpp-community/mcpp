// mcpp.pack.distribution — what a package is beyond its staged files
// (SPEC-014 §4, §5): the containers, the package's metadata
// (`mcpp-package.json`), the launchers of a universal package, the payload and
// the launcher program of a single file, and `--merge`.
//
// THE ENGINE KNOWS NO PLATFORM HERE. A launcher is generated from the
// platform's `runner` and the legs; a loader of a self-contained universal
// package is the platform's carrier with nothing carried; a single file's
// launcher is the engine's own program, written below as source and built for
// the target at pack time like any program.

export module mcpp.pack.distribution;

import std;
import mcpp.libs.json;
import mcpp.pack.compress;
import mcpp.pack.zip;
import mcpp.toolchain.triple;   // the system of a package's leg (§8)
import mcpplibs.os;

export namespace mcpp::pack::distribution {

// ─── digests ──────────────────────────────────────────────────────────────

// SHA-256 of `bytes`, of a file's content, in lowercase hexadecimal.
std::string sha256(std::string_view bytes);
std::string sha256_file(const std::filesystem::path& file);

// ─── containers ───────────────────────────────────────────────────────────

// `root` (a staged tree) as a tar whose one top-level directory is
// `root.filename()`, compressed as `c` says. The tar is written here, the same
// bytes for the same tree on every host (see `tar_bytes`).
std::expected<void, std::string> write_tar(const std::filesystem::path& root,
                                           const std::filesystem::path& archive,
                                           mcpp::pack::compress::Choice c);
// The same as a zip: members deflated unless `c` is `none`, links kept, a
// file executable as its mode says or when it is named `program`.
std::expected<void, std::string> write_zip(const std::filesystem::path& root,
                                           const std::filesystem::path& archive,
                                           mcpp::pack::compress::Choice c,
                                           std::string_view program = {});

// A package this command wrote, as a tree under `into`: a directory as it is,
// an archive (`.zip`, `.tar`, `.tar.gz`, `.tgz`, `.tar.zst`, `.tar.xz`)
// extracted into `into`, which is new or empty. Returns the tree's root, the
// archive's one top-level directory.
//
// An archive is read here, not by a program on PATH, and creates nothing
// outside `into`: every header is read within the archive, every zip member's
// CRC-32 checked; a member whose path is absolute or holds `..`, one that lies
// under a link member, and a link whose target leaves the tree are refused,
// before anything is written; the links are made after every file.
std::expected<std::filesystem::path, std::string>
unpack(const std::filesystem::path& package, const std::filesystem::path& into);

// ─── launchers (SPEC-014 §4.3) ────────────────────────────────────────────

struct Launch {
    std::string                        name;     // bin/<name>, bin/<name>.cmd
    // The platform's `runner.cmd`, expanded as `mcpp run` expands it: the
    // first `{}` of a word is the image (`{}`, `--image={}`), and the image is
    // appended when no word holds one.
    std::vector<std::string>           runner;
    std::map<std::string, std::string> entry;    // arch -> the entry image's file name
    // The loader a self-contained package carries under
    // `libexec/<loader>/<os>-<arch>/`; empty when it carries none.
    std::string                        loader;
};

// Whether both launchers can state `l`: a `.cmd` keeps a word literal inside
// double quotes, so a word holding a double quote or a line break cannot be
// written in one. Checked before a launcher is written; `windows_launcher` of
// a `Launch` this refuses is a `.cmd` that says so and runs nothing.
std::expected<void, std::string> launchable(const Launch& l);

std::string posix_launcher(const Launch& l);
std::string windows_launcher(const Launch& l);

// ─── mcpp-package.json (SPEC-014 §4.5) ────────────────────────────────────

inline constexpr std::string_view kFileName = "mcpp-package.json";

struct File {
    std::string path;    // relative to the tree's root, `/`-separated
    std::string role;    // program | image | library | loader | launcher | resource | metadata
    std::string leg;     // the leg's triple; empty: shared by every leg
    std::string sha256;
};

struct Package {
    int                                schema = 1;
    std::string                        name, version;
    std::string                        platform;   // "<ns>.<name>@<version>"; empty: none
    std::string                        linkage, mode;
    std::vector<std::string>           legs;       // canonical triples
    std::vector<std::string>           systems;    // `<os>-<arch>` of the loaders under libexec/
    std::vector<std::string>           runner;     // empty for a native package
    std::map<std::string, std::string> entry;      // arch -> path of the entry (universal)
    std::string                        program;    // the program's path (one native leg)
    std::vector<File>                  files;
};

// What a caller knows of a file's role and leg; every other file's role is
// read from where it lies (`libexec/` a loader, `lib/` a library, ...).
struct Stated {
    std::map<std::string, std::string> role;   // path -> role
    std::map<std::string, std::string> leg;    // path -> leg
};

// Every file of the tree described, digested, and the document written at
// `root/mcpp-package.json`.
std::expected<Package, std::string> describe(const std::filesystem::path& root, Package pkg,
                                             const Stated& stated);
std::string to_json(const Package& p);
// `root/mcpp-package.json`, every path it names (`files[].path`, the entries,
// the program) checked to be a normalised path inside the package.
std::expected<Package, std::string> read(const std::filesystem::path& root);

// The `<os>-<arch>` directories of `root/libexec/<loader>/`: the systems whose
// loaders a self-contained universal package carries, sorted.
std::vector<std::string> loader_systems(const std::filesystem::path& root);

// ─── a single file (SPEC-014 §5.2) ────────────────────────────────────────

// The tree under `root` as the payload a single file's launcher extracts: the
// record stream `MCPPTREE1\n` then, per entry, a kind byte (`d` directory,
// `f` file, `x` executable file, `l` link), a 32-bit path length, the path
// (`/`-separated), a 64-bit data length and the data (a link's data is its
// target). Little-endian; entries in path order.
std::expected<std::string, std::string> tree_payload(const std::filesystem::path& root);

// The launcher program's sources: its manifest and its entry. It reads the
// carried payload (`mcpp_carried_begin` .. `mcpp_carried_end`, a zstd frame
// of `tree_payload`), extracts it once into the user's cache under the
// payload's sha256 (`mcpp_carried_digest`), and executes the program at
// `mcpp_carried_name` with its own arguments. On POSIX it runs nothing from a
// cache directory another user owns.
std::string launcher_manifest(std::string_view name, std::string_view version);
std::string_view launcher_main();

// ─── the distribution description (SPEC-014 §8) ───────────────────────────

// The suffixes of the archives a package is written as, the longer of two
// that end alike first.
inline constexpr std::array<std::string_view, 6> kArchiveSuffixes{ ".tar.gz", ".tgz", ".tar.zst", ".tar.xz",
                                                                   ".tar", ".zip" };

// The name of a package's one directory: its archive's name without the
// suffix (`app-1.0-openkal.zip` holds `app-1.0-openkal/`), and a name with no
// archive's suffix as written (`-o app-1.0-openkal` with `--pkg-format dir`;
// a stem would cut at the version's last dot). `mcpp pack -o` and `--merge -o`
// name the directory so, and an index entry finds it so.
std::string archive_directory(const std::filesystem::path& archive);

// A package written by `mcpp pack`, as an index entry reads it.
struct Asset {
    std::filesystem::path file;
    Package               pkg;
    std::string           sha256;
    std::string           top;      // the archive's one top-level directory
};

// The xim-pkgindex entry (spec 2) of `assets`, fetched from `<urlPrefix>/<file
// name>`: a universal package is one asset every system installs, and its
// runner is a dependency unless the package carries its loaders; packages of
// one system and architecture each are that system's entry, the architecture
// in the file name a `${arch}` of one URL. The install moves the archive's
// one top-level directory into place; the program is registered from its
// launcher (`bin/<name>`, `bin/<name>.cmd` on Windows) or the program itself.
std::expected<std::string, std::string> xpkg_descriptor(const std::vector<Asset>& assets,
                                                        std::string_view urlPrefix);

// ─── --merge (SPEC-014 §4.6) ──────────────────────────────────────────────

struct MergeRequest {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path              output;     // empty: ./<tree><ext>
    std::string                        format;     // tar | zip | dir; empty: the first input's
    mcpp::pack::compress::Choice       compression;
};

// The inputs' trees as one package. Every file is checked against the digest
// its package states before it is copied; the output is new, or an archive
// that is a package and none of the inputs (`dir` writes a new directory only).
std::expected<std::filesystem::path, std::string> merge(const MergeRequest& req);

// A new directory under the system's temporary directory, made by this call
// and no other (a name another made first is passed over) and open to this
// user alone: where packages are unpacked to be read.
std::expected<std::filesystem::path, std::string> work_directory(std::string_view purpose);

}  // namespace mcpp::pack::distribution

namespace mcpp::pack::distribution {

namespace fs = std::filesystem;

// ─── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────────

namespace {

struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block{};
    std::size_t used = 0;
    std::uint64_t bits = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress() {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(block[4 * i]) << 24) | (std::uint32_t(block[4 * i + 1]) << 16)
                 | (std::uint32_t(block[4 * i + 2]) << 8) | std::uint32_t(block[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (int i = 0; i < 64; ++i) {
            const auto t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g))
                          + k[i] + w[i];
            const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(std::string_view data) {
        for (unsigned char ch : data) {
            block[used++] = ch;
            if (used == 64) { compress(); used = 0; }
        }
        bits += static_cast<std::uint64_t>(data.size()) * 8;
    }

    std::string hex() {
        const auto total = bits;
        block[used++] = 0x80;
        if (used > 56) { while (used < 64) block[used++] = 0; compress(); used = 0; }
        while (used < 56) block[used++] = 0;
        for (int i = 7; i >= 0; --i) block[used++] = static_cast<unsigned char>(total >> (8 * i));
        compress();
        std::string out;
        for (auto v : h) out += std::format("{:08x}", v);
        return out;
    }
};

std::optional<std::string> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool spit(const fs::path& p, std::string_view bytes) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bool(out);
}

// Whether a file is executable. A POSIX host's mode says. Windows keeps no
// such bit (its libraries report every file executable), so there what the
// file is says: a program or library image (ELF, Mach-O, PE) or a script
// (`#!`), which are the files a POSIX host's linkers and launcher writer leave
// executable; the tree's bytes are then the same on every host.
bool executable_file(const fs::path& p) {
    if constexpr (mcpplibs::os::is_windows) {
        std::ifstream in(p, std::ios::binary);
        char head[4]{};
        in.read(head, sizeof head);
        const std::string_view h(head, static_cast<std::size_t>(in.gcount()));
        return h.starts_with("#!") || h.starts_with("MZ") || h == std::string_view("\x7f" "ELF", 4)
            || h == std::string_view("\xcf\xfa\xed\xfe", 4) || h == std::string_view("\xce\xfa\xed\xfe", 4)
            || h == std::string_view("\xca\xfe\xba\xbe", 4);
    } else {
        std::error_code ec;
        const auto perms = fs::status(p, ec).permissions();
        if (ec) return false;
        return (perms & fs::perms::owner_exec) != fs::perms::none;
    }
}

// One entry of a staged tree: its path under the root (`/`-separated), what it
// is (`d` a directory, `f` a file, `x` an executable file, `l` a link) and a
// link's target.
struct Node {
    std::string path;
    char        kind = 'f';
    std::string target;
};

// Every entry under `root`, links not followed, in path order (bytewise: a
// directory before what it holds). Whatever cannot be read is an error, not
// an entry left out.
std::expected<std::vector<Node>, std::string> walk(const fs::path& root) {
    std::vector<Node> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entry;
        const auto st = it->symlink_status(entry);
        Node n;
        n.path = it->path().lexically_relative(root).generic_string();
        if (fs::is_symlink(st)) {
            n.kind = 'l';
            n.target = fs::read_symlink(it->path(), entry).generic_string();
        } else if (fs::is_directory(st)) {
            n.kind = 'd';
        } else if (fs::is_regular_file(st)) {
            n.kind = executable_file(it->path()) ? 'x' : 'f';
        } else if (!entry) {
            continue;   // a socket, a device: nothing a package holds
        }
        if (entry) return std::unexpected(std::format("cannot read '{}': {}", it->path().string(), entry.message()));
        out.push_back(std::move(n));
    }
    if (ec) return std::unexpected(std::format("cannot walk '{}': {}", root.string(), ec.message()));
    std::sort(out.begin(), out.end(), [](const Node& a, const Node& b) { return a.path < b.path; });
    return out;
}

}  // namespace

std::string sha256(std::string_view bytes) {
    Sha256 s;
    s.update(bytes);
    return s.hex();
}

std::string sha256_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    std::array<char, 1 << 16> buf;
    while (in) {
        in.read(buf.data(), buf.size());
        if (auto n = in.gcount(); n > 0) s.update(std::string_view(buf.data(), static_cast<std::size_t>(n)));
    }
    return s.hex();
}

// ─── containers ───────────────────────────────────────────────────────────

namespace {

// ── the tar mcpp writes ──
//
// POSIX ustar, written here rather than by a `tar` on PATH, whose bytes are
// its own: GNU tar records the owner, the group and the times of the files,
// and the order the file system lists a directory in, so two packs of one
// tree differed. These bytes are the same on every host: the entries in path
// order, every owner 0 and unnamed, every time 0, the mode 0755 (a directory,
// an executable file), 0644 (another file) or 0777 (a link). A name or a link
// target over the 100 bytes of its field, or a size over the 8 GiB of its, is
// stated in a pax extended header ahead of the entry (POSIX.1-2001), which
// every tar reads.

constexpr std::size_t kBlock = 512;

std::size_t padding(std::size_t n) { return (kBlock - n % kBlock) % kBlock; }

// `v` in octal, `width - 1` digits and a NUL.
void octal(char* field, std::size_t width, std::uint64_t v) {
    for (std::size_t i = width - 1; i-- > 0; v >>= 3) field[i] = static_cast<char>('0' + (v & 7));
    field[width - 1] = '\0';
}

void tar_header(std::string& out, std::string_view name, char type, unsigned mode,
                std::uint64_t size, std::string_view link) {
    std::array<char, kBlock> h{};
    std::copy_n(name.data(), std::min<std::size_t>(name.size(), 100), h.data());
    octal(h.data() + 100, 8, mode);
    octal(h.data() + 108, 8, 0);     // uid
    octal(h.data() + 116, 8, 0);     // gid
    octal(h.data() + 124, 12, size <= 077777777777ull ? size : 0);
    octal(h.data() + 136, 12, 0);    // mtime
    std::fill_n(h.data() + 148, 8, ' ');
    h[156] = type;
    std::copy_n(link.data(), std::min<std::size_t>(link.size(), 100), h.data() + 157);
    std::copy_n("ustar\0" "00", 8, h.data() + 257);
    octal(h.data() + 329, 8, 0);     // devmajor
    octal(h.data() + 337, 8, 0);     // devminor
    unsigned sum = 0;
    for (char ch : h) sum += static_cast<unsigned char>(ch);
    octal(h.data() + 148, 7, sum);   // six digits and a NUL; the space stays
    out.append(h.data(), h.size());
}

// One pax record: "<length> <key>=<value>\n", the length counting its own
// digits.
std::string pax_record(std::string_view key, std::string_view value) {
    const auto rest = key.size() + value.size() + 3;
    auto length = rest + 1;
    while (length != rest + std::to_string(length).size()) length = rest + std::to_string(length).size();
    return std::format("{} {}={}\n", length, key, value);
}

void tar_entry(std::string& out, std::string_view name, char type, unsigned mode,
               std::string_view data, std::string_view link) {
    std::string pax;
    if (name.size() > 100) pax += pax_record("path", name);
    if (link.size() > 100) pax += pax_record("linkpath", link);
    if (data.size() > 077777777777ull) pax += pax_record("size", std::to_string(data.size()));
    if (!pax.empty()) {
        tar_header(out, "././@PaxHeader", 'x', 0644, pax.size(), {});
        out += pax;
        out.append(padding(pax.size()), '\0');
    }
    tar_header(out, name, type, mode, data.size(), link);
    out += data;
    out.append(padding(data.size()), '\0');
}

// The tar of `root`: its directory, then every entry under it.
std::expected<std::string, std::string> tar_bytes(const fs::path& root) {
    auto nodes = walk(root);
    if (!nodes) return std::unexpected(nodes.error());
    const auto top = (root.has_filename() ? root : root.parent_path()).filename().generic_string();
    std::string out;
    tar_entry(out, top + "/", '5', 0755, {}, {});
    for (auto const& n : *nodes) {
        const auto name = top + "/" + n.path;
        if (n.kind == 'd') { tar_entry(out, name + "/", '5', 0755, {}, {}); continue; }
        if (n.kind == 'l') { tar_entry(out, name, '2', 0777, {}, n.target); continue; }
        auto data = slurp(root / fs::path(n.path));
        if (!data) return std::unexpected(std::format("cannot read '{}'", (root / n.path).string()));
        tar_entry(out, name, '0', n.kind == 'x' ? 0755 : 0644, *data, {});
    }
    out.append(2 * kBlock, '\0');
    return out;
}

}  // namespace

std::expected<void, std::string> write_tar(const fs::path& root, const fs::path& archive,
                                           mcpp::pack::compress::Choice c) {
    auto tar = tar_bytes(root);
    if (!tar) return std::unexpected(tar.error());
    auto packed = mcpp::pack::compress::bytes(*tar, c);
    if (!packed) return std::unexpected(std::format("{}: {}", archive.string(), packed.error()));
    if (!spit(archive, *packed)) return std::unexpected(std::format("cannot write '{}'", archive.string()));
    return {};
}

std::expected<void, std::string> write_zip(const fs::path& root, const fs::path& archive,
                                           mcpp::pack::compress::Choice c,
                                           std::string_view program) {
    auto nodes = walk(root);
    if (!nodes) return std::unexpected(nodes.error());
    const auto top = (root.has_filename() ? root : root.parent_path()).filename().generic_string();
    // In path order, as `walk` returns the tree.
    std::vector<mcpp::pack::zip::Entry> entries;
    for (auto const& n : *nodes) {
        if (n.kind == 'd' || (n.kind == 'l' && n.target.empty())) continue;
        mcpp::pack::zip::Entry entry;
        entry.name = top + "/" + n.path;
        entry.source = root / fs::path(n.path);
        entry.symlinkTarget = n.target;
        entry.executable = n.kind == 'x'
            || (n.kind == 'f' && !program.empty() && entry.source.filename() == program);
        entries.push_back(std::move(entry));
    }
    mcpp::pack::zip::WriteOptions o;
    o.deflate = c.algorithm != mcpp::pack::compress::Algorithm::None;
    o.level   = c.algorithm == mcpp::pack::compress::Algorithm::Gzip ? c.level : -1;
    return mcpp::pack::zip::write(archive, entries, o);
}

namespace {

// ── what an archive holds ──

std::uint16_t get16(std::string_view b, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[at])
                                      | (static_cast<unsigned char>(b[at + 1]) << 8));
}
std::uint32_t get32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(get16(b, at)) | (static_cast<std::uint32_t>(get16(b, at + 2)) << 16);
}

// `s` split at every `/`, empty parts kept.
std::vector<std::string_view> parts(std::string_view s) {
    std::vector<std::string_view> out;
    for (std::size_t at = 0; at <= s.size();) {
        auto slash = s.find('/', at);
        if (slash == std::string_view::npos) slash = s.size();
        out.push_back(s.substr(at, slash - at));
        at = slash + 1;
    }
    return out;
}

// What can never be a path inside a tree on every system: empty, absolute
// (`/x`, `C:`), with a backslash or a NUL.
bool foreign(std::string_view p) {
    return p.empty() || p.front() == '/' || p.find('\\') != std::string_view::npos
        || p.find('\0') != std::string_view::npos || (p.size() > 1 && p[1] == ':');
}

// The path inside the extraction root a member's name names, normalised:
// relative, `/`-separated, without `.` or empty parts or a trailing `/`; empty
// for the root itself (`./`, which `tar -C dir .` writes). None when it would
// leave the root. A `..` is refused wherever it is, `a/../b` too, which stays
// inside as words: through a link member `a` it would not, and no archive mcpp
// writes holds one.
std::optional<std::string> member_path(std::string_view name) {
    if (foreign(name)) return std::nullopt;
    std::string out;
    for (auto part : parts(name)) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return std::nullopt;
        if (!out.empty()) out += '/';
        out += part;
    }
    return out;
}

// Whether the link member at `path` whose target is `target` resolves inside
// the extraction root: the target is relative and, walked from the link's
// directory, never climbs above the root and goes on from no link member (one
// may be where it ends: that link's own target is checked the same way).
// A member path as a file system that ignores case sees it (APFS, NTFS): two
// members, or a link and a path, that differ in case alone are one there.
std::string folded(std::string_view path) {
    std::string out(path);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// `links` holds the link members' paths `folded`.
bool link_inside(std::string_view path, std::string_view target, const std::set<std::string>& links) {
    if (foreign(target)) return false;
    // Where the walk is, a path under the root (empty: the root itself); the
    // link's directory first.
    const auto slash = path.rfind('/');
    std::string here(slash == std::string_view::npos ? std::string_view() : path.substr(0, slash));
    for (auto part : parts(target)) {
        if (part.empty() || part == ".") continue;
        if (!here.empty() && links.contains(folded(here))) return false;
        if (part == "..") {
            if (here.empty()) return false;
            const auto up = here.rfind('/');
            here.resize(up == std::string::npos ? 0 : up);
        } else {
            here += (here.empty() ? "" : "/") + std::string(part);
        }
    }
    return true;
}

// One member of an archive: its path (`member_path`) and what it is. A file's
// bytes are as stored (`method` 8: DEFLATEd); a link's target is read at once.
struct Member {
    std::string                  path;
    char                         kind = 'f';    // 'd' directory, 'f' file, 'l' link
    bool                         executable = false;
    std::string_view             stored;
    std::uint16_t                method = 0;
    std::uint64_t                size = 0;
    std::optional<std::uint32_t> crc;           // a zip member's CRC-32
    std::string                  target;
};

// A file member's content: inflated if it was deflated, its length and its
// CRC-32 (a zip's) checked against the archive's.
std::expected<std::string, std::string> contents(const Member& m) {
    std::string data;
    if (m.method == 8) {
        auto d = mcpp::pack::compress::inflate_raw(m.stored, static_cast<std::size_t>(m.size));
        if (!d) return std::unexpected(std::format("'{}': {}", m.path, d.error()));
        data = std::move(*d);
    } else {
        data.assign(m.stored);
    }
    if (data.size() != m.size)
        return std::unexpected(std::format("'{}' holds {} bytes; the archive states {}", m.path,
                                           data.size(), m.size));
    if (m.crc && mcpp::pack::zip::crc32(data) != *m.crc)
        return std::unexpected(std::format("'{}' fails its CRC-32: {:08x}, and the archive states {:08x}",
                                           m.path, mcpp::pack::zip::crc32(data), *m.crc));
    return data;
}

// The members of a zip, read through its central directory (stored and
// deflated, as mcpp writes them). Every field is read within the archive: an
// offset or a length that points past it is refused, never followed.
std::expected<std::vector<Member>, std::string> zip_members(std::string_view b) {
    auto within = [&](std::uint64_t at, std::uint64_t n) { return at <= b.size() && n <= b.size() - at; };
    if (b.size() < 22) return std::unexpected(std::string("not a zip: shorter than its end record"));
    // The end of the central directory: the last 22 bytes, or as far before
    // them as its comment (65535 bytes at most) reaches.
    std::size_t eocd = std::string_view::npos;
    const std::size_t lowest = b.size() - 22 > 65535 ? b.size() - 22 - 65535 : 0;
    for (std::size_t i = b.size() - 22;; --i) {
        if (get32(b, i) == 0x06054b50 && within(i + 22, get16(b, i + 20))) { eocd = i; break; }
        if (i == lowest) break;
    }
    if (eocd == std::string_view::npos) return std::unexpected(std::string("not a zip: no end of central directory"));
    const auto count = get16(b, eocd + 10);
    std::size_t at = get32(b, eocd + 16);
    if (count == 0xFFFF || at == 0xFFFFFFFF)
        return std::unexpected(std::string("a ZIP64 archive, which mcpp neither writes nor reads"));
    std::vector<Member> out;
    for (std::uint16_t n = 0; n < count; ++n) {
        if (!within(at, 46) || get32(b, at) != 0x02014b50)
            return std::unexpected(std::format("the central directory is malformed at {}", at));
        const auto flags = get16(b, at + 8), method = get16(b, at + 10);
        const auto crc = get32(b, at + 16), packed = get32(b, at + 20), size = get32(b, at + 24);
        const auto nameLen = get16(b, at + 28), extraLen = get16(b, at + 30), commentLen = get16(b, at + 32);
        const auto mode = get32(b, at + 38) >> 16;
        const auto local = get32(b, at + 42);
        if (!within(at + 46, std::uint64_t(nameLen) + extraLen + commentLen))
            return std::unexpected(std::string("the central directory is truncated"));
        const auto name = b.substr(at + 46, nameLen);
        at += 46 + std::size_t(nameLen) + extraLen + commentLen;
        const bool dir = name.ends_with('/') || (mode & 0170000u) == 0040000u;
        auto path = member_path(name);
        if (!path || (path->empty() && !dir))
            return std::unexpected(std::format("a member names '{}', which is not a path inside the archive",
                                               name));
        if (path->empty()) continue;   // the root, there already
        if (flags & 1) return std::unexpected(std::format("'{}' is encrypted", name));
        if (method != 0 && method != 8)
            return std::unexpected(std::format("'{}' uses method {}", name, method));
        if (packed == 0xFFFFFFFF || size == 0xFFFFFFFF || local == 0xFFFFFFFF)
            return std::unexpected(std::format("'{}' is a ZIP64 member, which mcpp neither writes nor reads",
                                               name));
        Member m;
        m.path = std::move(*path);
        if (dir) {
            m.kind = 'd';
            out.push_back(std::move(m));
            continue;
        }
        if (!within(local, 30) || get32(b, local) != 0x04034b50)
            return std::unexpected(std::format("'{}' has no local header at {}", name, local));
        const std::uint64_t dataAt = std::uint64_t(local) + 30 + get16(b, local + 26) + get16(b, local + 28);
        if (!within(dataAt, packed)) return std::unexpected(std::format("'{}' is truncated", name));
        m.stored = b.substr(static_cast<std::size_t>(dataAt), packed);
        m.method = method;
        m.size = size;
        m.crc = crc;
        m.executable = (mode & 0100u) != 0;
        if ((mode & 0170000u) == 0120000u) {
            auto target = contents(m);
            if (!target) return std::unexpected(target.error());
            m.kind = 'l';
            m.target = std::move(*target);
            m.stored = {};
        }
        out.push_back(std::move(m));
    }
    return out;
}

// A numeric field of a tar header: octal digits (after spaces, ended by a
// space or a NUL), or base-256 when the first byte's high bit is set.
std::optional<std::uint64_t> tar_number(std::string_view f) {
    std::uint64_t v = 0;
    if (!f.empty() && (static_cast<unsigned char>(f.front()) & 0x80)) {
        if (static_cast<unsigned char>(f.front()) != 0x80) return std::nullopt;   // negative, or too large
        for (char ch : f.substr(1)) {
            if (v >> 56) return std::nullopt;
            v = v << 8 | static_cast<unsigned char>(ch);
        }
        return v;
    }
    std::size_t i = f.find_first_not_of(' ');
    if (i == std::string_view::npos) return 0;
    for (; i < f.size() && f[i] >= '0' && f[i] <= '7'; ++i) {
        if (v >> 60) return std::nullopt;
        v = v * 8 + static_cast<std::uint64_t>(f[i] - '0');
    }
    if (i < f.size() && f[i] != ' ' && f[i] != '\0') return std::nullopt;
    return v;
}

// A text field of a tar header, up to its first NUL.
std::string_view tar_text(std::string_view f) { return f.substr(0, f.find('\0')); }

// The records of a pax extended header that name the next entry: its path,
// its link's target, its size. The others (times, owners) are read past.
std::expected<void, std::string> pax_records(std::string_view data, std::string& path, std::string& link,
                                             std::optional<std::uint64_t>& size) {
    while (!data.empty()) {
        const auto space = data.find(' ');
        std::uint64_t length = 0;
        if (space == std::string_view::npos
            || std::from_chars(data.data(), data.data() + space, length).ec != std::errc{}
            || length <= space + 1 || length > data.size() || data[length - 1] != '\n')
            return std::unexpected(std::string("a pax header is malformed"));
        const auto record = data.substr(space + 1, length - space - 2);
        data.remove_prefix(length);
        const auto eq = record.find('=');
        if (eq == std::string_view::npos) return std::unexpected(std::string("a pax record has no '='"));
        const auto key = record.substr(0, eq), value = record.substr(eq + 1);
        if (key == "path") path = value;
        else if (key == "linkpath") link = value;
        else if (key == "size") {
            std::uint64_t v = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), v).ec != std::errc{})
                return std::unexpected(std::format("a pax size '{}' is not a number", value));
            size = v;
        }
    }
    return {};
}

// The members of a tar: ustar, its pax extended headers and GNU's long names
// (the archives earlier releases wrote through GNU tar). Every header's
// checksum is checked and every length read within the archive.
std::expected<std::vector<Member>, std::string> tar_members(std::string_view b) {
    std::vector<Member> out;
    std::string longName, longLink;                // the next entry's, from a pax or GNU header
    std::optional<std::uint64_t> longSize;
    std::size_t at = 0;
    while (true) {
        if (at == b.size()) break;   // no end blocks: as GNU tar, the end of the archive
        if (b.size() - at < kBlock) return std::unexpected(std::string("the tar ends inside a header"));
        const auto h = b.substr(at, kBlock);
        if (h.find_first_not_of('\0') == std::string_view::npos) break;   // the end of the archive
        unsigned sum = 0;
        int signedSum = 0;
        for (std::size_t i = 0; i < kBlock; ++i) {
            const char ch = i >= 148 && i < 156 ? ' ' : h[i];
            sum += static_cast<unsigned char>(ch);
            signedSum += static_cast<signed char>(ch);
        }
        const auto stated = tar_number(h.substr(148, 8));
        if (!stated || (*stated != sum && *stated != static_cast<std::uint64_t>(signedSum)))
            return std::unexpected(std::format("the tar header at {} fails its checksum", at));
        const char type = h[156];
        const bool meta = type == 'x' || type == 'g' || type == 'L' || type == 'K';
        auto size = tar_number(h.substr(124, 12));
        if (!size) return std::unexpected(std::format("the tar header at {} states no size", at));
        if (longSize && !meta) size = longSize;
        at += kBlock;
        if (*size > b.size() - at || padding(static_cast<std::size_t>(*size)) > b.size() - at - *size)
            return std::unexpected(std::format("the tar entry at {} is truncated", at - kBlock));
        const auto data = b.substr(at, static_cast<std::size_t>(*size));
        at += static_cast<std::size_t>(*size) + padding(static_cast<std::size_t>(*size));
        if (type == 'g') continue;
        if (type == 'x') {
            if (auto r = pax_records(data, longName, longLink, longSize); !r) return std::unexpected(r.error());
            continue;
        }
        if (type == 'L') { longName = tar_text(data); continue; }
        if (type == 'K') { longLink = tar_text(data); continue; }
        std::string name(longName);
        if (name.empty()) {
            name = tar_text(h.substr(0, 100));
            // POSIX ustar's prefix; GNU's own magic ("ustar  ") puts other
            // things there.
            if (const auto prefix = tar_text(h.substr(345, 155)); h.substr(257, 6) == std::string_view("ustar\0", 6)
                                                                 && !prefix.empty())
                name = std::string(prefix) + "/" + name;
        }
        const std::string link = longLink.empty() ? std::string(tar_text(h.substr(157, 100))) : longLink;
        longName.clear();
        longLink.clear();
        longSize.reset();
        const bool dir = type == '5' || ((type == '0' || type == '\0') && name.ends_with('/'));
        auto path = member_path(name);
        if (!path || (path->empty() && !dir))
            return std::unexpected(std::format("a member names '{}', which is not a path inside the archive",
                                               name));
        if (path->empty()) continue;   // the root, there already
        Member m;
        m.path = std::move(*path);
        if (dir) {
            m.kind = 'd';
        } else if (type == '0' || type == '\0' || type == '7') {
            m.stored = data;
            m.size = data.size();
            m.executable = (tar_number(h.substr(100, 8)).value_or(0) & 0100u) != 0;
        } else if (type == '2') {
            m.kind = 'l';
            m.target = link;
        } else if (type == '1') {
            return std::unexpected(std::format("'{}' is a hard link, which mcpp does not write", name));
        } else {
            return std::unexpected(std::format("'{}' is a tar entry of type '{}', which a package does not "
                                               "hold", name, type));
        }
        out.push_back(std::move(m));
    }
    return out;
}

// The members created under `into`, and nothing outside it. Every path is
// checked before anything is written: none twice, none under a link member,
// no link whose target leaves the tree. Then the directories and the files
// are written, and the links made last, so no member is written through one.
std::expected<void, std::string> place(const std::vector<Member>& members, const fs::path& into) {
    // Compared `folded`: on a file system that ignores case `A/B` is `a/b`.
    std::set<std::string> paths, links;
    for (auto const& m : members) {
        if (!paths.insert(folded(m.path)).second)
            return std::unexpected(std::format("'{}' is in the archive twice", m.path));
        if (m.kind == 'l') links.insert(folded(m.path));
    }
    for (auto const& m : members) {
        for (auto slash = m.path.find('/'); slash != std::string::npos; slash = m.path.find('/', slash + 1))
            if (links.contains(folded(m.path.substr(0, slash))))
                return std::unexpected(std::format("'{}' lies under the link '{}'", m.path, m.path.substr(0, slash)));
        if (m.kind == 'l' && !link_inside(m.path, m.target, links))
            return std::unexpected(std::format("the link '{}' points to '{}', outside the package", m.path,
                                               m.target));
    }
    std::error_code ec;
    for (auto const& m : members) {
        const auto target = into / fs::path(m.path);
        if (m.kind == 'd') {
            fs::create_directories(target, ec);
            if (ec) return std::unexpected(std::format("cannot create '{}': {}", target.string(), ec.message()));
            continue;
        }
        if (m.kind != 'f') continue;
        auto data = contents(m);
        if (!data) return std::unexpected(data.error());
        if (!spit(target, *data)) return std::unexpected(std::format("cannot write '{}'", target.string()));
        if (m.executable)
            fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
    }
    for (auto const& m : members) {
        if (m.kind != 'l') continue;
        const auto link = into / fs::path(m.path);
        fs::create_directories(link.parent_path(), ec);
        if (fs::is_directory(link.parent_path() / fs::path(m.target), ec))
            fs::create_directory_symlink(fs::path(m.target), link, ec);
        else
            fs::create_symlink(fs::path(m.target), link, ec);
        if (ec) return std::unexpected(std::format("cannot link '{}': {}", link.string(), ec.message()));
    }
    // And as the file system resolves them: every link ends under the root.
    const auto root = fs::weakly_canonical(into, ec);
    for (auto const& m : members) {
        if (m.kind != 'l') continue;
        const auto real = fs::weakly_canonical(into / fs::path(m.path), ec);
        const auto [r, _] = std::mismatch(root.begin(), root.end(), real.begin(), real.end());
        if (ec || r != root.end())
            return std::unexpected(std::format("the link '{}' points to '{}', outside the package", m.path,
                                               m.target));
    }
    return {};
}

}  // namespace

std::expected<fs::path, std::string> unpack(const fs::path& package, const fs::path& into) {
    std::error_code ec;
    if (fs::is_directory(package, ec)) return package;
    if (!fs::is_regular_file(package, ec))
        return std::unexpected(std::format("'{}' is neither a package directory nor an archive",
                                           package.string()));
    const auto name = package.filename().string();
    using mcpp::pack::compress::Algorithm;
    const bool zip = name.ends_with(".zip");
    const std::optional<Algorithm> a = name.ends_with(".tar.gz") || name.ends_with(".tgz") ? Algorithm::Gzip
                                     : name.ends_with(".tar.zst") ? Algorithm::Zstd
                                     : name.ends_with(".tar.xz") ? Algorithm::Xz
                                     : name.ends_with(".tar") ? std::optional(Algorithm::None)
                                                              : std::nullopt;
    if (!zip && !a)
        return std::unexpected(std::format(
            "'{}' is not a package this command writes (.zip, .tar, .tar.gz, .tar.zst, .tar.xz, "
            "or a directory)", name));
    if (fs::exists(fs::symlink_status(into, ec)) && !(fs::is_directory(fs::symlink_status(into, ec))
                                                      && fs::is_empty(into, ec)))
        return std::unexpected(std::format("'{}' is not an empty directory to unpack {} into",
                                           into.string(), name));
    fs::create_directories(into, ec);
    auto bytes = slurp(package);
    if (!bytes) return std::unexpected(std::format("cannot read '{}'", package.string()));
    // The members point into the archive's bytes, or into its tar.
    std::string plain;
    std::expected<std::vector<Member>, std::string> members;
    if (zip) {
        members = zip_members(*bytes);
    } else {
        auto t = mcpp::pack::compress::decompress(*bytes, *a);
        if (!t) return std::unexpected(std::format("{}: {}", name, t.error()));
        plain = std::move(*t);
        members = tar_members(plain);
    }
    if (!members) return std::unexpected(std::format("{}: {}", name, members.error()));
    if (auto r = place(*members, into); !r) return std::unexpected(std::format("{}: {}", name, r.error()));
    // The archive's one top-level directory (a link there is none).
    std::vector<fs::path> tops;
    for (auto const& e : fs::directory_iterator(into, ec))
        if (fs::is_directory(e.symlink_status(ec))) tops.push_back(e.path());
    if (tops.size() != 1)
        return std::unexpected(std::format("'{}' has {} top-level directories; a package has one",
                                           name, tops.size()));
    return tops.front();
}

// ─── launchers ────────────────────────────────────────────────────────────

namespace {

// A word of a POSIX shell command, single-quoted.
std::string sh_word(std::string_view w) {
    std::string out = "'";
    for (char ch : w) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

// Text of a .cmd inside double quotes: `%` doubled, the one character a batch
// file reads there.
std::string cmd_quoted(std::string_view w) {
    std::string out;
    for (char ch : w) {
        if (ch == '%') out += "%%";
        else out.push_back(ch);
    }
    return out;
}

// Text of a .cmd outside quotes (an `echo`): `%` doubled and every character
// the command line reads escaped with `^`.
std::string cmd_bare(std::string_view w) {
    std::string out;
    for (char ch : w) {
        if (ch == '%') out += "%%";
        else if (std::string_view("&|<>^()").find(ch) != std::string_view::npos) out += {'^', ch};
        else out.push_back(ch);
    }
    return out;
}

// How a launcher's language writes the runner's words: `literal` a piece of
// a word as text, `image` the image's variable, `around` what encloses each
// word.
struct Quoting {
    std::string      (*literal)(std::string_view);
    std::string_view image;
    std::string_view around;
};

// The runner's arguments, its words after the program, by `mcpp run`'s rule
// (mcpp.freestanding's `expand`): a word's first `{}` is the image, in
// `--image={}` as well as in `{}`, and the image is appended when no word
// holds one.
std::string runner_args(const std::vector<std::string>& runner, const Quoting& q) {
    std::string out;
    bool placed = false;
    for (std::size_t i = 1; i < runner.size(); ++i) {
        const std::string_view w = runner[i];
        out += std::format(" {}", q.around);
        if (const auto at = w.find("{}"); at == std::string_view::npos) {
            out += q.literal(w);
        } else {
            placed = true;
            if (at > 0) out += q.literal(w.substr(0, at));
            out += q.image;
            if (at + 2 < w.size()) out += q.literal(w.substr(at + 2));
        }
        out += q.around;
    }
    if (!placed) out += std::format(" {0}{1}{0}", q.around, q.image);
    return out;
}

std::string arch_list(const Launch& l) {
    std::string arches;
    for (auto const& [arch, _] : l.entry) arches += (arches.empty() ? "" : " ") + arch;
    return arches;
}

}  // namespace

std::expected<void, std::string> launchable(const Launch& l) {
    std::vector<std::string_view> words{l.name, l.loader};
    words.insert(words.end(), l.runner.begin(), l.runner.end());
    for (auto const& [arch, file] : l.entry) words.insert(words.end(), {arch, file});
    for (auto w : words)
        // A word ending in `\` would make the quote that closes it a quote of
        // the word for the program reading its arguments.
        if (w.find_first_of(std::string_view("\"\r\n\0", 4)) != std::string_view::npos || w.ends_with('\\'))
            return std::unexpected(std::format(
                "the launcher of {} cannot state '{}': a word of a .cmd holds no double quote and no "
                "line break, and does not end in a backslash", l.name, w));
    return {};
}

std::string posix_launcher(const Launch& l) {
    std::string cases;
    for (auto const& [arch, file] : l.entry)
        cases += std::format("  {}) entry={} ;;\n", sh_word(arch), sh_word(file));
    // The runner: its program found in the package, then on PATH; none, the
    // image itself.
    std::string find, args = " \"$@\"";
    if (l.runner.empty()) {
        find = "runner=$image\n";
    } else {
        args = runner_args(l.runner, { sh_word, "\"$image\"", "" }) + args;
        find = std::format("runner={}\n", sh_word(l.runner.front()));
        if (!l.loader.empty())
            find = std::format(
                "loader={0}\n"
                "runner=\"$top/libexec/$loader/$os-$arch/$loader\"\n"
                "[ -x \"$runner\" ] || runner={1}\n", sh_word(l.loader), sh_word(l.runner.front()));
    }
    return std::format(
        "#!/bin/sh\n"
        "# Generated by mcpp pack (SPEC-014 §4.3): runs the package's program on this\n"
        "# machine's architecture through the platform's runner.\n"
        "#\n"
        "# The package's directory is found through the links a person may have made\n"
        "# to this file, by `readlink` without -f, which not every system has.\n"
        "self=$0\n"
        "n=0\n"
        "while [ -h \"$self\" ] && [ \"$n\" -lt 40 ]; do\n"
        "  link=$(readlink -- \"$self\") || break\n"
        "  case $link in\n"
        "    /*) self=$link ;;\n"
        "    *) self=$(dirname -- \"$self\")/$link ;;\n"
        "  esac\n"
        "  n=$((n + 1))\n"
        "done\n"
        "top=$(CDPATH= cd -P -- \"$(dirname -- \"$self\")/..\" && pwd -P)\n"
        "case \"$(uname -m)\" in\n"
        "  x86_64|amd64|AMD64) arch=x86_64 ;;\n"
        "  aarch64|arm64|ARM64) arch=aarch64 ;;\n"
        "  *) arch=$(uname -m) ;;\n"
        "esac\n"
        "case \"$(uname -s)\" in\n"
        "  Linux) os=linux ;;\n"
        "  Darwin) os=macos ;;\n"
        "  *) os=$(uname -s) ;;\n"
        "esac\n"
        "case \"$arch\" in\n"
        "{1}"
        "  *) printf '%s: this machine is %s; the package holds %s\\n' {0} \"$arch\" {2} >&2; exit 126 ;;\n"
        "esac\n"
        "image=\"$top/lib/$arch/$entry\"\n"
        "{3}"
        "exec \"$runner\"{4}\n",
        sh_word(l.name), cases, sh_word(arch_list(l)), find, args);
}

std::string windows_launcher(const Launch& l) {
    if (!launchable(l))
        return "@echo off\r\n"
               "rem Generated by mcpp pack (SPEC-014 §4.3).\r\n"
               "echo This package's runner has a word a .cmd cannot state; run it from a POSIX shell. 1>&2\r\n"
               "exit /b 126\r\n";
    std::string picks;
    for (auto const& [arch, file] : l.entry)
        picks += std::format("if \"%arch%\"==\"{0}\" set \"image=%top%\\lib\\{0}\\{1}\"\r\n",
                             cmd_quoted(arch), cmd_quoted(file));
    std::string find = "set \"runner=%image%\"\r\n", args;
    if (!l.runner.empty()) {
        args = runner_args(l.runner, { cmd_quoted, "%image%", "\"" });
        const auto program = cmd_quoted(l.runner.front());
        find = std::format("set \"runner={}\"\r\n", program);
        if (!l.loader.empty())
            find = std::format(
                "set \"runner=%top%\\libexec\\{0}\\windows-%arch%\\{0}.exe\"\r\n"
                "if not exist \"%runner%\" set \"runner={1}\"\r\n", cmd_quoted(l.loader), program);
    }
    return std::format(
        "@echo off\r\n"
        "rem Generated by mcpp pack (SPEC-014 §4.3): runs the package's program on this\r\n"
        "rem machine's architecture through the platform's runner.\r\n"
        "setlocal DisableDelayedExpansion\r\n"
        "for %%I in (\"%~dp0..\") do set \"top=%%~fI\"\r\n"
        "set \"machine=%PROCESSOR_ARCHITECTURE%\"\r\n"
        "if defined PROCESSOR_ARCHITEW6432 set \"machine=%PROCESSOR_ARCHITEW6432%\"\r\n"
        "set \"arch=%machine%\"\r\n"
        "if /i \"%machine%\"==\"AMD64\" set \"arch=x86_64\"\r\n"
        "if /i \"%machine%\"==\"ARM64\" set \"arch=aarch64\"\r\n"
        "set \"image=\"\r\n"
        "{1}"
        "if not defined image (\r\n"
        "  echo {0}: this machine is %arch%; the package holds {2} 1>&2\r\n"
        "  exit /b 126\r\n"
        ")\r\n"
        "{3}"
        "\"%runner%\"{4} %*\r\n"
        "exit /b %ERRORLEVEL%\r\n",
        cmd_bare(l.name), picks, cmd_bare(arch_list(l)), find, args);
}

// ─── mcpp-package.json ────────────────────────────────────────────────────

namespace {

// A shared object's name: `.so`, or `.so.` and a version of digits and dots
// (`libz.so.1.3`); not a name that merely holds `.so` (`notes.sol`).
bool shared_object(std::string_view file) {
    const auto at = file.rfind(".so");
    if (at == std::string_view::npos) return false;
    const auto rest = file.substr(at + 3);
    return rest.empty()
        || (rest.size() > 1 && rest.front() == '.' && rest.find_first_not_of("0123456789.") == std::string_view::npos);
}

std::string role_of(std::string_view path, bool executable) {
    auto ends = [&](std::string_view s) { return path.ends_with(s); };
    const auto file = path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1);
    if (path.starts_with("libexec/")) return "loader";
    if (file.starts_with("README") || file.starts_with("LICENSE") || file == "HOST-REQUIREMENTS"
        || path.starts_with("share/licenses/"))
        return "metadata";
    if (ends(".dll") || ends(".dylib") || shared_object(file) || ends(".okso"))
        return "library";
    if (ends(".oke")) return "image";
    if (path.starts_with("bin/") && (executable || ends(".exe") || ends(".js") || ends(".wasm")))
        return "program";
    if (ends(".cmd") || ends(".sh")) return "launcher";
    // An executable at the root: the wrapper that starts the program.
    if (executable && path.find('/') == std::string_view::npos) return "launcher";
    return "resource";
}

}  // namespace

std::string to_json(const Package& p) {
    nlohmann::json files = nlohmann::json::array();
    for (auto const& f : p.files)
        files.push_back({{"path", f.path}, {"role", f.role},
                         {"leg", f.leg.empty() ? nlohmann::json(nullptr) : nlohmann::json(f.leg)},
                         {"sha256", f.sha256}});
    nlohmann::json launch = nlohmann::json::object();
    if (!p.runner.empty()) {
        launch["runner"] = p.runner;
        nlohmann::json entry = nlohmann::json::object();
        for (auto const& [arch, path] : p.entry) entry[arch] = path;
        launch["entry"] = std::move(entry);
    } else {
        launch["program"] = p.program.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.program);
    }
    nlohmann::ordered_json j;
    j["schema"] = p.schema;
    j["name"] = p.name;
    j["version"] = p.version;
    j["platform"] = p.platform.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.platform);
    j["linkage"] = p.linkage;
    j["mode"] = p.mode;
    j["legs"] = p.legs;
    j["systems"] = p.systems;
    j["launch"] = std::move(launch);
    j["files"] = std::move(files);
    return j.dump(2) + "\n";
}

std::expected<Package, std::string> describe(const fs::path& root, Package pkg, const Stated& stated) {
    pkg.files.clear();
    auto nodes = walk(root);
    if (!nodes) return std::unexpected(nodes.error());
    // The files, in path order; a link is not a file of the package.
    for (auto const& n : *nodes) {
        if ((n.kind != 'f' && n.kind != 'x') || n.path == kFileName) continue;
        File f;
        f.path = n.path;
        if (auto r = stated.role.find(n.path); r != stated.role.end()) f.role = r->second;
        else f.role = role_of(n.path, n.kind == 'x');
        if (auto l = stated.leg.find(n.path); l != stated.leg.end()) f.leg = l->second;
        else if (pkg.legs.size() == 1 && f.role != "metadata" && f.role != "resource") f.leg = pkg.legs.front();
        f.sha256 = sha256_file(root / fs::path(n.path));
        pkg.files.push_back(std::move(f));
    }
    if (!spit(root / kFileName, to_json(pkg)))
        return std::unexpected(std::format("cannot write '{}'", (root / kFileName).string()));
    return pkg;
}

std::expected<Package, std::string> read(const fs::path& root) {
    auto text = slurp(root / kFileName);
    if (!text) return std::unexpected(std::format("'{}' has no {}", root.string(), kFileName));
    Package p;
    try {
        auto j = nlohmann::json::parse(*text);
        p.schema  = j.value("schema", 0);
        p.name    = j.value("name", std::string{});
        p.version = j.value("version", std::string{});
        if (j.contains("platform") && j["platform"].is_string()) p.platform = j["platform"].get<std::string>();
        p.linkage = j.value("linkage", std::string{});
        p.mode    = j.value("mode", std::string{});
        p.legs    = j.value("legs", std::vector<std::string>{});
        p.systems = j.value("systems", std::vector<std::string>{});
        if (j.contains("launch")) {
            auto const& l = j["launch"];
            if (l.contains("runner")) p.runner = l["runner"].get<std::vector<std::string>>();
            if (l.contains("entry") && l["entry"].is_object())
                for (auto it = l["entry"].begin(); it != l["entry"].end(); ++it)
                    p.entry[it.key()] = it.value().get<std::string>();
            if (l.contains("program") && l["program"].is_string()) p.program = l["program"].get<std::string>();
        }
        for (auto const& f : j.value("files", nlohmann::json::array())) {
            File x;
            x.path = f.value("path", std::string{});
            x.role = f.value("role", std::string{});
            if (f.contains("leg") && f["leg"].is_string()) x.leg = f["leg"].get<std::string>();
            x.sha256 = f.value("sha256", std::string{});
            p.files.push_back(std::move(x));
        }
    } catch (const std::exception& e) {
        return std::unexpected(std::format("{}: {}", (root / kFileName).string(), e.what()));
    }
    if (p.schema != 1)
        return std::unexpected(std::format("{} states schema {}; this mcpp reads schema 1",
                                           (root / kFileName).string(), p.schema));
    // Every path it names is one inside the package, written as `describe`
    // writes it: what reads the package (--merge, the index entry) opens them.
    std::vector<std::string_view> named;
    for (auto const& f : p.files) named.push_back(f.path);
    for (auto const& [_, e] : p.entry) named.push_back(e);
    if (!p.program.empty()) named.push_back(p.program);
    for (auto n : named)
        if (n.empty() || member_path(n) != n)
            return std::unexpected(std::format("{} names '{}', which is not a path inside the package",
                                               (root / kFileName).string(), n));
    return p;
}

std::vector<std::string> loader_systems(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    const auto libexec = root / "libexec";
    if (!fs::is_directory(libexec, ec)) return out;
    // One walk, two levels deep: `libexec/<loader>/<os>-<arch>`.
    for (auto it = fs::recursive_directory_iterator(libexec, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() < 1) continue;
        it.disable_recursion_pending();
        if (it->is_directory(ec)) out.push_back(it->path().filename().string());
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ─── a single file ────────────────────────────────────────────────────────

std::expected<std::string, std::string> tree_payload(const fs::path& root) {
    auto nodes = walk(root);
    if (!nodes) return std::unexpected(nodes.error());
    std::string out = "MCPPTREE1\n";
    auto put = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    for (auto const& n : *nodes) {
        std::string data = n.target;
        if (n.kind == 'f' || n.kind == 'x') {
            auto bytes = slurp(root / fs::path(n.path));
            if (!bytes) return std::unexpected(std::format("cannot read '{}'", (root / n.path).string()));
            data = std::move(*bytes);
        }
        out.push_back(n.kind);
        put(n.path.size(), 4);
        out += n.path;
        put(data.size(), 8);
        out += data;
    }
    return out;
}

std::string launcher_manifest(std::string_view name, std::string_view version) {
    return std::format(
        "# Generated by `mcpp pack --pkg-format single-file`: the launcher of {0}, which\n"
        "# carries the package as a zstd frame and runs it from the user's cache.\n"
        "[package]\nname    = \"{0}\"\nversion = \"{1}\"\n\n"
        "[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n"
        "[build]\nsources = [\"src/main.cpp\", \"src/carried.S\"]\n\n"
        "[dependencies.mcpplibs]\nos = \"0.2.0\"\n\n"
        "[dependencies.compat]\nzstd = \"1.5.7\"\n",
        name, version.empty() ? std::string_view("0.0.0") : version);
}

std::string_view launcher_main() {
    return R"MCPP(// The launcher of a single file (SPEC-014 §5.2), written by mcpp pack.
//
// The package it runs is carried in this program as one zstd frame of a record
// stream (`mcpp_carried_begin` .. `mcpp_carried_end`). The first run extracts
// it into the user's cache, under the sha256 mcpp wrote (`mcpp_carried_digest`):
// into a name of its own beside the destination, renamed into place when every
// file is written, so a directory that is there is complete. Every later run
// executes the program at once. The frame carries its content's checksum, and
// zstd refuses a payload that does not match it.
//
// Nothing runs from a directory another user owns. On POSIX the cache's
// directories this program makes (`mcpp/`, `single-file/`) and the tree it runs
// from are this user's and no links, or the program refuses: another user who
// owned one could have put a program of theirs under the payload's name.
#include <zstd.h>

import std;
import mcpplibs.os;

extern "C" const unsigned char mcpp_carried_begin[];
extern "C" const unsigned char mcpp_carried_end[];
extern "C" const char mcpp_carried_name[];
extern "C" const char mcpp_carried_digest[];

namespace {

namespace fs = std::filesystem;
namespace os = mcpplibs::os;

const std::string_view kName{mcpp_carried_name};
const std::string_view kDigest{mcpp_carried_digest};

int fail(std::string_view what) {
    std::cerr << std::format("{}: {}\n", kName, what);
    return 127;
}

bool safe(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos
        || (name.size() > 1 && name[1] == ':'))
        return false;
    for (std::size_t at = 0; at <= name.size();) {
        auto slash = name.find('/', at);
        if (slash == std::string_view::npos) slash = name.size();
        if (name.substr(at, slash - at) == "..") return false;
        at = slash + 1;
    }
    return true;
}

std::optional<std::string> extract(const fs::path& into) {
    const auto* begin = reinterpret_cast<const char*>(mcpp_carried_begin);
    const auto size = static_cast<std::size_t>(mcpp_carried_end - mcpp_carried_begin);
    const auto content = ZSTD_getFrameContentSize(begin, size);
    if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN)
        return "the carried payload is not a zstd frame";
    std::string tree(static_cast<std::size_t>(content), '\0');
    const auto n = ZSTD_decompress(tree.data(), tree.size(), begin, size);
    if (ZSTD_isError(n)) return std::format("the carried payload: {}", ZSTD_getErrorName(n));
    std::string_view t(tree);
    if (!t.starts_with("MCPPTREE1\n")) return "the carried payload is not a tree";
    std::size_t at = 10;
    auto get = [&](int bytes) {
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= std::uint64_t(static_cast<unsigned char>(t[at + i])) << (8 * i);
        at += static_cast<std::size_t>(bytes);
        return v;
    };
    std::error_code ec;
    while (at < t.size()) {
        if (at + 13 > t.size()) return "the carried tree is truncated";
        const char kind = t[at++];
        const auto pathLen = get(4);
        if (at + pathLen + 8 > t.size()) return "the carried tree is truncated";
        const std::string_view name = t.substr(at, pathLen);
        at += pathLen;
        const auto dataLen = get(8);
        if (at + dataLen > t.size()) return "the carried tree is truncated";
        const std::string_view data = t.substr(at, dataLen);
        at += dataLen;
        if (!safe(name)) return std::format("the carried tree names '{}'", name);
        const auto target = into / fs::path(std::string(name));
        fs::create_directories(target.parent_path(), ec);
        if (kind == 'd') { fs::create_directories(target, ec); continue; }
        if (kind == 'l') {
            if (auto r = os::fs::create_link(fs::path(std::string(data)), target, false); !r)
                return std::format("cannot link {}", target.string());
            continue;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return std::format("cannot write {}", target.string());
        out.close();
        if (kind == 'x') (void)os::fs::set_executable(target, true);
    }
    std::ofstream(into / ".mcpp-complete") << kDigest << '\n';
    return std::nullopt;
}

// Whether `dir` is a directory, not a link, and (on POSIX) this user's.
bool own(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return false;
    if constexpr (os::is_posix) return os::fs::owner_of(dir) == os::fs::owner::this_user;
    else return true;
}

int foreign(const fs::path& dir) {
    return fail(std::format("{} is not a directory of this user's; nothing is run from it", dir.string()));
}

}  // namespace

int main(int argc, char** argv) {
    auto cache = os::dirs::cache();
    if (!cache) return fail("this machine names no cache directory for its user");
    const fs::path base = *cache / "mcpp" / "single-file";
    const fs::path root = base / std::string(kDigest);
    std::error_code ec;
    fs::create_directories(base, ec);
    if (!own(base.parent_path())) return foreign(base.parent_path());
    if (!own(base)) return foreign(base);
    if (!fs::exists(root / ".mcpp-complete", ec)) {
        if constexpr (os::is_posix)
            fs::permissions(base, fs::perms::owner_all, fs::perm_options::replace, ec);
        const auto self = os::process::self();
        const fs::path tmp = base / std::format(".{}-{}", kDigest,
                                                self ? self->id : std::uint64_t{0});
        fs::remove_all(tmp, ec);
        if (auto why = extract(tmp)) { fs::remove_all(tmp, ec); return fail(*why); }
        if (auto r = os::fs::rename(tmp, root, /*replace=*/false); !r) {
            // Another run placed it first; that one is complete.
            fs::remove_all(tmp, ec);
            if (!fs::exists(root / ".mcpp-complete", ec))
                return fail(std::format("cannot place the program under {}", root.string()));
        }
    }
    if (!own(root)) return foreign(root);
    const fs::path program = root / fs::path(std::string(kName));
    os::process::spawn_options o;
    o.program = program;
    for (int i = 1; i < argc; ++i) o.args.emplace_back(argv[i]);
    o.in = o.out = o.err = os::process::stdio::inherit;
    (void)os::process::exec(std::move(o));
    return fail(std::format("cannot start {}", program.string()));
}
)MCPP";
}

// ─── the distribution description ─────────────────────────────────────────

namespace {

// The xlings key of a system as a package names it (linux | macos | windows).
std::optional<std::string> xlings_system_of(std::string_view os) {
    if (os == "linux") return "linux";
    if (os == "macos") return "macosx";
    if (os == "windows") return "windows";
    return std::nullopt;
}

// THE SYSTEM AND ARCHITECTURE A PACKAGE OF ONE LEG INSTALLS ON: those its
// program runs on (SPEC-014 §8). An entry's `xpm.<system>` installs a program
// of that system and its `archs` are machine architectures. A leg of a Linux
// (not Android), macOS or Windows triple is a program of that system. An ok
// image packed with its carrier (`--mode self-contained`, `static`) is a
// program of the carrier's system and architecture, which the package records
// as the one loader it carries (`systems`). Every other leg -- a WebAssembly
// module, an Android or iOS application, a bare-metal image, an ok image
// without a carrier -- is no program of a system. Every triple that was not
// Windows or macOS used to be written `linux` (a wasm32-emscripten package:
// `archs = {"wasm32"}` under `linux`).
struct ProgramHost { std::string system, arch; };
std::optional<ProgramHost> program_host(const Package& pkg) {
    auto t = mcpp::toolchain::triple::parse(pkg.legs.front());
    if (!t) return std::nullopt;
    if (t->os != "linux" || t->env != "android")
        if (auto system = xlings_system_of(t->os)) return ProgramHost{ *system, t->arch };
    if (t->os == "openkal" && pkg.systems.size() == 1) {
        const auto& carrier = pkg.systems.front();   // `<os>-<arch>`
        const auto dash = carrier.find('-');
        if (dash != std::string::npos)
            if (auto system = xlings_system_of(std::string_view(carrier).substr(0, dash)))
                return ProgramHost{ *system, carrier.substr(dash + 1) };
    }
    return std::nullopt;
}

// The route that distributes a package of `triple` when no entry describes it.
std::string distributed_instead(std::string_view triple) {
    auto t = mcpp::toolchain::triple::parse(triple);
    if (t && t->is_wasm())
        return "a page for the web, mcpp pack --pkg-format dist-web (mcpp-plugins)";
    if (t && t->os == "openkal")
        return "the platform's universal package (mcpp pack without --no-universal), which every "
               "system installs, or a package that carries the image in its carrier (--mode "
               "self-contained)";
    return "the package of a Linux, macOS or Windows target";
}

// A text inside a Lua double-quoted literal: what the metadata states is
// written as a string, never as code.
std::string lua_text(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20 || c == 0x7f) out += std::format("\\{:03}", static_cast<unsigned>(c));
        else out += static_cast<char>(c);
    }
    return out;
}

std::string lua_list(const std::vector<std::string>& v) {
    std::string out = "{";
    for (std::size_t i = 0; i < v.size(); ++i) out += std::format("{}\"{}\"", i ? ", " : "", lua_text(v[i]));
    return out + "}";
}

}  // namespace

std::string archive_directory(const fs::path& archive) {
    const auto name = archive.filename().string();
    for (auto suffix : kArchiveSuffixes)
        if (name.ends_with(suffix)) return name.substr(0, name.size() - suffix.size());
    return name;
}

std::expected<std::string, std::string> xpkg_descriptor(const std::vector<Asset>& assets,
                                                        std::string_view urlPrefix) {
    if (assets.empty()) return std::unexpected(std::string("no package to describe"));
    auto const& first = assets.front().pkg;
    std::string prefix(urlPrefix);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    if (prefix.empty())
        return std::unexpected(std::string("--url names where the packages are published"));
    for (auto const& a : assets)
        if (a.pkg.name != first.name || a.pkg.version != first.version)
            return std::unexpected(std::format("{} is {} {}, and {} is {} {}: one entry describes one "
                                               "package", assets.front().file.filename().string(),
                                               first.name, first.version, a.file.filename().string(),
                                               a.pkg.name, a.pkg.version));
    const bool universal = !first.runner.empty();
    std::vector<std::string> archs;
    // system -> block text
    std::map<std::string, std::string> blocks;
    if (universal) {
        if (assets.size() != 1)
            return std::unexpected(std::string("a universal package is one asset; merge its legs first "
                                               "(mcpp pack --merge)"));
        auto const& a = assets.front();
        for (auto const& [arch, _] : first.entry) archs.push_back(arch);
        // The runner is a package of the index's own namespace; a bare name
        // turns ambiguous as soon as a second index the home lists has one
        // (xim-pkgindex refuses it: check-dep-namespace).
        std::string deps;
        if (first.mode != "self-contained")
            deps = std::format("            deps = {{ \"xim:{}\" }},\n",
                               lua_text(std::filesystem::path(first.runner.front()).stem().string()));
        for (auto system : { "linux", "macosx", "windows" })
            blocks[system] = std::format(
                "{0}            [\"latest\"] = {{ ref = \"{1}\" }},\n"
                "            [\"{1}\"] = {{\n                url = \"{2}/{3}\",\n                sha256 = \"{4}\",\n            }},\n",
                deps, lua_text(first.version), lua_text(prefix), lua_text(a.file.filename().string()),
                lua_text(a.sha256));
    } else {
        // system -> (arch -> asset)
        std::map<std::string, std::map<std::string, const Asset*>> bySystem;
        for (auto const& a : assets) {
            if (a.pkg.legs.size() != 1)
                return std::unexpected(std::format("{} holds {} legs; an entry per system describes "
                                                   "packages of one leg", a.file.filename().string(),
                                                   a.pkg.legs.size()));
            const auto& triple = a.pkg.legs.front();
            const auto host = program_host(a.pkg);
            if (!host)
                return std::unexpected(std::format(
                    "{} is a package of {}, which is no program of Linux, macOS or Windows: an "
                    "xlings entry installs a program of the system it runs on.\n  use: {}",
                    a.file.filename().string(), triple, distributed_instead(triple)));
            bySystem[host->system][host->arch] = &a;
            if (std::ranges::find(archs, host->arch) == archs.end()) archs.push_back(host->arch);
        }
        for (auto const& [system, byArch] : bySystem) {
            // One URL with `${arch}`: the file names may differ by the
            // architecture alone.
            std::string tmpl;
            std::string shas;
            for (auto const& [arch, a] : byArch) {
                auto name = a->file.filename().string();
                if (auto at = name.find(arch); at != std::string::npos) name.replace(at, arch.size(), "${arch}");
                if (!tmpl.empty() && tmpl != name)
                    return std::unexpected(std::format("the {} packages' names differ beyond the "
                                                       "architecture ({} and {})", system, tmpl, name));
                tmpl = name;
                shas += std::format("                    [\"{}\"] = \"{}\",\n", lua_text(arch),
                                    lua_text(a->sha256));
            }
            blocks[system] = std::format(
                "            [\"latest\"] = {{ ref = \"{0}\" }},\n"
                "            [\"{0}\"] = {{\n                url = \"{1}/{2}\",\n                sha256 = {{\n{3}                }},\n            }},\n",
                lua_text(first.version), lua_text(prefix), lua_text(tmpl), shas);
        }
    }
    std::ranges::sort(archs);
    std::string xpm;
    for (auto const& [system, text] : blocks) xpm += std::format("        {} = {{\n{}        }},\n", system, text);

    // The command a person runs, and the entry's name: the launcher of a
    // universal package (named after its images, `bin/<name>`), else the
    // program -- not the package, which may be named otherwise
    // (`mcpp-language-server` runs as `mcppls`).
    std::string command;
    if (universal && !first.entry.empty())
        command = std::filesystem::path(first.entry.begin()->second).stem().string();
    else if (!first.program.empty())
        command = std::filesystem::path(first.program).stem().string();
    if (command.empty()) command = first.name;
    // Where the program lies in the installed tree. The packages of two
    // systems may place it apart (a Windows program at the root, a POSIX one
    // under bin/ beside the root's convenience script); the entry then names
    // each system's directory.
    std::string bindir = "\"bin\"", alias, bindirs;
    if (universal) {
        alias = std::format("if os.host() == \"windows\" then opts.alias = \"{}.cmd\" end\n    ",
                            lua_text(command));
    } else {
        std::map<std::string, std::string> dirOf;   // system -> the program's directory
        for (auto const& a : assets) {
            const auto program = std::filesystem::path(a.pkg.program);
            dirOf[program_host(a.pkg)->system] =   // each package has one: checked above
                program.has_parent_path() ? program.parent_path().generic_string() : std::string(".");
        }
        const auto& one = dirOf.begin()->second;
        if (std::ranges::all_of(dirOf, [&](auto const& e) { return e.second == one; })) {
            bindir = std::format("\"{}\"", lua_text(one));
        } else {
            bindirs = "    local bindir = {";
            for (auto const& [system, dir] : dirOf)
                bindirs += std::format(" {} = \"{}\",", system, lua_text(dir));
            bindirs.back() = ' ';
            bindirs += "}\n";
            bindir = "bindir[os.host()]";
        }
    }
    // xim unpacks the archive beside the file it downloaded, and `mcpp pack`
    // names the archive's one top-level directory after the archive; the
    // install takes the directory from that file's name, which holds for
    // every system, architecture and later version of the entry.
    std::array<bool, kArchiveSuffixes.size()> used{};
    for (auto const& a : assets) {
        const auto file = a.file.filename().string();
        const auto ext = std::ranges::find_if(kArchiveSuffixes, [&](std::string_view e) { return file.ends_with(e); });
        if (ext == kArchiveSuffixes.end())
            return std::unexpected(std::format("{} is not an archive xim unpacks (.tar.gz, .tgz, .tar.zst, "
                                               ".tar.xz, .tar, .zip)", file));
        if (archive_directory(a.file) != a.top)
            return std::unexpected(std::format(
                "{} holds the directory {}/: an entry installs the directory named after the archive, as "
                "mcpp pack writes it (name the archive {}{})", file, a.top, a.top, *ext));
        used[static_cast<std::size_t>(ext - kArchiveSuffixes.begin())] = true;
    }
    std::string exts;
    for (std::size_t i = 0; i < kArchiveSuffixes.size(); ++i)
        if (used[i]) exts += std::format("{}\"{}\"", exts.empty() ? "" : ", ", kArchiveSuffixes[i]);
    return std::format(
        "-- Generated by `mcpp emit xpkg --package` from the packages' mcpp-package.json\n"
        "-- (SPEC-014 §8).\n"
        "package = {{\n"
        "    spec = \"2\",\n"
        "    name = \"{0}\",\n"
        "    description = \"{8} {1}{2}\",\n"
        "    type = \"package\",\n"
        "    archs = {3},\n"
        "    programs = {{\"{0}\"}},\n"
        "    xvm_enable = true,\n"
        "    xpm = {{\n{4}    }},\n"
        "}}\n\n"
        "import(\"xim.libxpkg.pkginfo\")\n"
        "import(\"xim.libxpkg.xvm\")\n\n"
        "function install()\n"
        "    -- The archive's one top-level directory is the package; it is named\n"
        "    -- after the archive, which xim unpacks beside it.\n"
        "    local top = pkginfo.install_file()\n"
        "    for _, ext in ipairs({{ {5} }}) do\n"
        "        if top:sub(-#ext) == ext then top = top:sub(1, #top - #ext) break end\n"
        "    end\n"
        "    if not os.isdir(top) then return false end\n"
        "    os.tryrm(pkginfo.install_dir())\n"
        "    os.mv(top, pkginfo.install_dir())\n"
        "    return os.isdir(pkginfo.install_dir())\n"
        "end\n\n"
        "function config()\n"
        "{9}"
        "    local opts = {{ bindir = path.join(pkginfo.install_dir(), {6}) }}\n"
        "    {7}xvm.add(\"{0}\", opts)\n"
        "    return true\n"
        "end\n\n"
        "function uninstall()\n"
        "    xvm.remove(\"{0}\")\n"
        "    return true\n"
        "end\n",
        lua_text(command), lua_text(first.version),
        universal ? std::format(", one package for every system ({})", lua_text(first.platform)) : std::string(),
        lua_list(archs), xpm, exts, bindir, alias, lua_text(first.name), bindirs);
}

// ─── --merge ──────────────────────────────────────────────────────────────

namespace {

// `p` absolute, normalised, and with a name: `pkg/` is `pkg`, `.` the current
// directory.
fs::path named(const fs::path& p) {
    std::error_code ec;
    auto out = fs::absolute(p, ec).lexically_normal();
    if (!out.has_filename() && out.has_relative_path()) out = out.parent_path();
    return out;
}

}  // namespace

std::expected<fs::path, std::string> work_directory(std::string_view purpose) {
    std::error_code ec;
    const auto base = fs::temp_directory_path(ec);
    if (ec) return std::unexpected(std::format("no temporary directory: {}", ec.message()));
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto p = base / std::format("mcpp-{}-{:08x}{:08x}", purpose, rd(), rd());
        if (!fs::create_directory(p, ec)) {
            if (ec) return std::unexpected(std::format("cannot create '{}': {}", p.string(), ec.message()));
            continue;   // another's
        }
        fs::permissions(p, fs::perms::owner_all, fs::perm_options::replace, ec);
        return p;
    }
    return std::unexpected(std::format("no new directory could be made under '{}'", base.string()));
}

std::expected<fs::path, std::string> merge(const MergeRequest& req) {
    if (req.inputs.size() < 2)
        return std::unexpected(std::string("--merge reads two packages or more"));
    std::error_code ec;
    auto made = work_directory("merge");
    if (!made) return std::unexpected(made.error());
    const auto work = *made;
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};

    std::vector<fs::path> inputs;
    for (auto const& i : req.inputs) inputs.push_back(named(i));
    std::vector<std::pair<fs::path, Package>> trees;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto root = unpack(inputs[i], work / std::format("in{}", i));
        if (!root) return std::unexpected(root.error());
        auto pkg = read(*root);
        if (!pkg) return std::unexpected(pkg.error());
        trees.emplace_back(*root, std::move(*pkg));
    }
    auto const& first = trees.front().second;
    for (std::size_t i = 1; i < trees.size(); ++i) {
        auto const& p = trees[i].second;
        auto differ = [&](std::string_view field, std::string const& a, std::string const& b)
            -> std::optional<std::string> {
            if (a == b) return std::nullopt;
            return std::format("--merge reads one package's legs and systems: {} has {} '{}', and {} "
                               "has '{}'", req.inputs.front().string(), field, a, req.inputs[i].string(), b);
        };
        for (auto r : { differ("name", first.name, p.name), differ("version", first.version, p.version),
                        differ("platform", first.platform, p.platform),
                        differ("linkage", first.linkage, p.linkage), differ("mode", first.mode, p.mode) })
            if (r) return std::unexpected(*r);
    }
    // The merged package's directory: named after the output, as `mcpp pack
    // -o` names it, else the first input's.
    const fs::path wrapper = req.output.empty() ? trees.front().first.filename()
                                                : fs::path(archive_directory(req.output));

    // The container: as asked, else as the output's name says, else as the
    // first input was written.
    std::string format = req.format;
    const auto outName = req.output.filename().string();
    const auto firstName = !req.output.empty() && (outName.ends_with(".zip") || outName.find(".tar") != std::string::npos)
        ? outName : inputs.front().filename().string();
    if (format.empty())
        format = firstName.ends_with(".zip") ? "zip"
               : firstName.find(".tar") != std::string::npos ? "tar"
               : fs::is_directory(inputs.front(), ec) ? "dir" : "tar";
    if (format != "tar" && format != "zip" && format != "dir")
        return std::unexpected(std::format("--merge writes tar, zip or dir; '{}' is a format a package "
                                           "provides, which reads a staged tree: merge to dir first",
                                           format));
    auto compression = req.compression;
    if (!compression.stated && format == "tar") {
        using mcpp::pack::compress::Algorithm;
        compression.algorithm = firstName.ends_with(".tar.zst") ? Algorithm::Zstd
                              : firstName.ends_with(".tar.xz")  ? Algorithm::Xz
                              : firstName.ends_with(".tar")     ? Algorithm::None
                                                                : Algorithm::Gzip;
    }
    // The compression fits the container and the name the output is given.
    {
        using mcpp::pack::compress::Algorithm;
        const auto a = compression.algorithm;
        if (compression.stated && format == "dir")
            return std::unexpected(std::string("--compress compresses a container, and the merge writes a "
                                               "directory"));
        if (compression.stated && format == "zip" && (a == Algorithm::Zstd || a == Algorithm::Xz))
            return std::unexpected(std::format("a zip's members are deflated (gzip) or stored (none); "
                                               "--compress {} is a tar's", mcpp::pack::compress::name(a)));
        const auto want = mcpp::pack::compress::tar_extension(a);
        const bool namedTar = outName.find(".tar") != std::string::npos || outName.ends_with(".tgz");
        if (format == "tar" && namedTar && !outName.ends_with(want)
            && !(a == Algorithm::Gzip && outName.ends_with(".tgz")))
            return std::unexpected(std::format("-o {} names another compression than the archive's ({})",
                                               outName, want));
    }
    const auto ext = format == "zip" ? std::string(".zip") : format == "dir" ? std::string()
                   : std::string(mcpp::pack::compress::tar_extension(compression.algorithm));
    const fs::path dest = named(req.output.empty() ? fs::path(wrapper.string() + ext) : req.output);

    // What is there already is replaced only when it is an archive this
    // command wrote and none of the inputs: --merge never removes a directory,
    // nor a file it cannot read as a package.
    for (std::size_t i = 0; i < inputs.size(); ++i)
        if (dest == inputs[i] || fs::equivalent(dest, inputs[i], ec))
            return std::unexpected(std::format("--merge: the output {} is the input {}", dest.string(),
                                               req.inputs[i].string()));
    if (const auto st = fs::symlink_status(dest, ec); fs::exists(st)) {
        if (format == "dir")
            return std::unexpected(std::format("--merge: {} exists; a merged directory is written as a new "
                                               "one", dest.string()));
        auto old = fs::is_regular_file(st) ? unpack(dest, work / "existing")
                                           : std::unexpected(std::string("not a file"));
        if (!old || !read(*old))
            return std::unexpected(std::format("--merge: {} exists and is not a package; it is not replaced",
                                               dest.string()));
    }

    const auto out = work / "merged" / wrapper;
    fs::create_directories(out, ec);
    Package merged = first;
    merged.legs.clear();
    merged.systems.clear();
    merged.entry.clear();
    Stated stated;
    std::map<std::string, std::pair<std::string, std::string>> seen;   // path -> (digest, input)
    for (std::size_t i = 0; i < trees.size(); ++i) {
        auto const& [root, pkg] = trees[i];
        for (auto const& l : pkg.legs)
            if (std::ranges::find(merged.legs, l) == merged.legs.end()) merged.legs.push_back(l);
        for (auto const& s : pkg.systems)
            if (std::ranges::find(merged.systems, s) == merged.systems.end()) merged.systems.push_back(s);
        for (auto const& [arch, e] : pkg.entry) merged.entry[arch] = e;
        // Each file as it is, not as its package says: a file changed after the
        // package was written is refused, and two inputs agree on a file when
        // its bytes do.
        for (auto const& f : pkg.files) {
            const auto digest = sha256_file(root / fs::path(f.path));
            if (digest != f.sha256)
                return std::unexpected(std::format(
                    "--merge: '{}' in {} has sha256 {}, and its {} states {}", f.path,
                    req.inputs[i].string(), digest.empty() ? std::string("(no such file)") : digest, kFileName,
                    f.sha256));
            if (auto it = seen.find(f.path); it != seen.end()) {
                if (it->second.first != digest)
                    return std::unexpected(std::format(
                        "--merge: '{}' is in {} with sha256 {} and in {} with sha256 {}; one package "
                        "holds one of each file", f.path, it->second.second, it->second.first,
                        req.inputs[i].string(), digest));
                continue;
            }
            seen[f.path] = {digest, req.inputs[i].string()};
            stated.role[f.path] = f.role;
            if (!f.leg.empty()) stated.leg[f.path] = f.leg;
            fs::create_directories((out / f.path).parent_path(), ec);
            fs::copy_file(root / f.path, out / f.path, fs::copy_options::overwrite_existing, ec);
            if (ec) return std::unexpected(std::format("cannot copy '{}': {}", f.path, ec.message()));
        }
    }
    std::ranges::sort(merged.legs);
    std::ranges::sort(merged.systems);
    if (auto d = describe(out, merged, stated); !d) return std::unexpected(d.error());

    if (format == "dir") {
        fs::create_directories(dest.parent_path(), ec);
        fs::copy(out, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return std::unexpected(std::format("cannot write '{}': {}", dest.string(), ec.message()));
        return dest;
    }
    auto r = format == "zip" ? write_zip(out, dest, compression) : write_tar(out, dest, compression);
    if (!r) return std::unexpected(r.error());
    return dest;
}

}  // namespace mcpp::pack::distribution
