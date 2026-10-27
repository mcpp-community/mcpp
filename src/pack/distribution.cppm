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
import mcpp.run;
import mcpplibs.os.platform;

export namespace mcpp::pack::distribution {

// ─── digests ──────────────────────────────────────────────────────────────

// SHA-256 of `bytes`, of a file's content, in lowercase hexadecimal.
std::string sha256(std::string_view bytes);
std::string sha256_file(const std::filesystem::path& file);

// ─── containers ───────────────────────────────────────────────────────────

// `root` (a staged tree) as a tar whose one top-level directory is
// `root.filename()`, compressed as `c` says.
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
// extracted. Returns the tree's root, the archive's one top-level directory.
std::expected<std::filesystem::path, std::string>
unpack(const std::filesystem::path& package, const std::filesystem::path& into);

// ─── launchers (SPEC-014 §4.3) ────────────────────────────────────────────

struct Launch {
    std::string                        name;     // bin/<name>, bin/<name>.cmd
    std::vector<std::string>           runner;   // the platform's `runner.cmd`, `{}` the image
    std::map<std::string, std::string> entry;    // arch -> the entry image's file name
    // The loader a self-contained package carries under
    // `libexec/<loader>/<os>-<arch>/`; empty when it carries none.
    std::string                        loader;
};

std::string posix_launcher(const Launch& l);
std::string windows_launcher(const Launch& l);

// The architecture directory a machine name selects, as the launchers map it.
std::string_view arch_dir(std::string_view machine);

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
// `mcpp_carried_name` with its own arguments.
std::string launcher_manifest(std::string_view name, std::string_view version);
std::string_view launcher_main();

// ─── the distribution description (SPEC-014 §8) ───────────────────────────

// A package written by `mcpp pack`, as an index entry reads it.
struct Asset {
    std::filesystem::path file;
    Package               pkg;
    std::string           sha256;
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

std::expected<std::filesystem::path, std::string> merge(const MergeRequest& req);

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

bool executable_file(const fs::path& p) {
    std::error_code ec;
    const auto perms = fs::status(p, ec).permissions();
    if (ec) return false;
    return (perms & fs::perms::owner_exec) != fs::perms::none;
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

std::expected<void, std::string> write_tar(const fs::path& root, const fs::path& archive,
                                           mcpp::pack::compress::Choice c) {
    std::error_code ec;
    if (archive.has_parent_path()) fs::create_directories(archive.parent_path(), ec);
    // Written uncompressed by tar, compressed by mcpp's codec: `.tar.zst` and
    // `.tar.xz` need no program on PATH, and a `.tar.gz` is the same bytes on
    // every host. `--force-local` on Windows: GNU tar reads `C:/x.tar` as the
    // rsh form `host:path`.
    auto plain = archive;
    plain += ".mcpp-tar";
    std::vector<std::string> argv{"tar"};
    if constexpr (mcpplibs::os::is_windows) argv.push_back("--force-local");
    argv.insert(argv.end(), {"-cf", plain.string(), "-C", root.parent_path().string(),
                             root.filename().string()});
    auto r = mcpp::run::execute(argv);
    if (r.exit_code != 0) {
        fs::remove(plain, ec);
        return std::unexpected(std::format("tar exited with {}: {}", r.exit_code, r.output));
    }
    auto packed = mcpp::pack::compress::file(plain, archive, c);
    fs::remove(plain, ec);
    if (!packed) return std::unexpected(packed.error());
    return {};
}

std::expected<void, std::string> write_zip(const fs::path& root, const fs::path& archive,
                                           mcpp::pack::compress::Choice c,
                                           std::string_view program) {
    std::error_code ec;
    std::vector<mcpp::pack::zip::Entry> entries;
    const auto wrapper = root.filename().string();
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        mcpp::pack::zip::Entry entry;
        entry.name = wrapper + "/" + e.path().lexically_relative(root).generic_string();
        entry.source = e.path();
        if (e.is_symlink(ec)) {
            entry.symlinkTarget = fs::read_symlink(e.path(), ec).generic_string();
            if (entry.symlinkTarget.empty()) continue;
            if (e.is_directory(ec)) it.disable_recursion_pending();
        } else if (!e.is_regular_file(ec)) {
            continue;
        } else {
            entry.executable = executable_file(e.path())
                || (!program.empty() && e.path().filename() == program);
        }
        entries.push_back(std::move(entry));
    }
    // Deterministic order; a comparator rather than a projection onto an
    // imported type's member (mcpp.pack's run_pe says why).
    std::sort(entries.begin(), entries.end(),
              [](const mcpp::pack::zip::Entry& a, const mcpp::pack::zip::Entry& b) {
                  return a.name < b.name;
              });
    mcpp::pack::zip::WriteOptions o;
    o.deflate = c.algorithm != mcpp::pack::compress::Algorithm::None;
    o.level   = c.algorithm == mcpp::pack::compress::Algorithm::Gzip ? c.level : 0;
    return mcpp::pack::zip::write(archive, entries, o);
}

namespace {

std::uint16_t get16(std::string_view b, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[at])
                                      | (static_cast<unsigned char>(b[at + 1]) << 8));
}
std::uint32_t get32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(get16(b, at)) | (static_cast<std::uint32_t>(get16(b, at + 2)) << 16);
}

// A relative, `/`-separated name with no `..`: what an archive may create.
bool safe_member(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos
        || (name.size() > 1 && name[1] == ':'))
        return false;
    std::size_t at = 0;
    while (at <= name.size()) {
        auto slash = name.find('/', at);
        if (slash == std::string_view::npos) slash = name.size();
        if (name.substr(at, slash - at) == "..") return false;
        at = slash + 1;
    }
    return true;
}

// The members of a zip, read through its central directory (stored and
// deflated, as mcpp writes them).
std::expected<void, std::string> unzip(std::string_view b, const fs::path& into) {
    std::size_t eocd = std::string_view::npos;
    for (std::size_t i = b.size() >= 22 ? b.size() - 22 : 0; ; --i) {
        if (get32(b, i) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || b.size() - i > 22 + 65535) break;
    }
    if (eocd == std::string_view::npos) return std::unexpected(std::string("not a zip: no end of central directory"));
    const auto count = get16(b, eocd + 10);
    std::size_t at = get32(b, eocd + 16);
    for (std::uint16_t n = 0; n < count; ++n) {
        if (at + 46 > b.size() || get32(b, at) != 0x02014b50)
            return std::unexpected(std::string("a zip's central directory is malformed"));
        const auto method = get16(b, at + 10);
        const auto packed = get32(b, at + 20);
        const auto size   = get32(b, at + 24);
        const auto nameLen = get16(b, at + 28), extraLen = get16(b, at + 30), commentLen = get16(b, at + 32);
        const auto attrs = get32(b, at + 38);
        const auto local = get32(b, at + 42);
        const std::string name(b.substr(at + 46, nameLen));
        at += 46 + nameLen + extraLen + commentLen;
        if (!safe_member(name)) return std::unexpected(std::format("a zip member names '{}'", name));
        if (name.ends_with('/')) { std::error_code ec; fs::create_directories(into / name, ec); continue; }
        const auto lnameLen = get16(b, local + 26), lextraLen = get16(b, local + 28);
        const auto dataAt = static_cast<std::size_t>(local) + 30 + lnameLen + lextraLen;
        if (dataAt + packed > b.size()) return std::unexpected(std::format("zip member '{}' is truncated", name));
        std::string data(b.substr(dataAt, packed));
        if (method == 8) {
            auto d = mcpp::pack::compress::inflate_raw(data, size);
            if (!d) return std::unexpected(std::format("zip member '{}': {}", name, d.error()));
            data = std::move(*d);
        } else if (method != 0) {
            return std::unexpected(std::format("zip member '{}' uses method {}", name, method));
        }
        const auto mode = attrs >> 16;
        const auto target = into / fs::path(name);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if ((mode & 0170000u) == 0120000u) {
            fs::create_symlink(data, target, ec);
            if (ec) return std::unexpected(std::format("cannot link '{}': {}", target.string(), ec.message()));
            continue;
        }
        if (!spit(target, data)) return std::unexpected(std::format("cannot write '{}'", target.string()));
        if (mode & 0100u)
            fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
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
    fs::create_directories(into, ec);
    const auto name = package.filename().string();
    auto bytes = slurp(package);
    if (!bytes) return std::unexpected(std::format("cannot read '{}'", package.string()));
    if (name.ends_with(".zip")) {
        if (auto r = unzip(*bytes, into); !r) return std::unexpected(std::format("{}: {}", name, r.error()));
    } else {
        using mcpp::pack::compress::Algorithm;
        Algorithm a = name.ends_with(".tar.gz") || name.ends_with(".tgz") ? Algorithm::Gzip
                    : name.ends_with(".tar.zst") ? Algorithm::Zstd
                    : name.ends_with(".tar.xz") ? Algorithm::Xz
                    : name.ends_with(".tar") ? Algorithm::None
                    : static_cast<Algorithm>(-1);
        if (static_cast<int>(a) == -1)
            return std::unexpected(std::format(
                "'{}' is not a package this command writes (.zip, .tar, .tar.gz, .tar.zst, .tar.xz, "
                "or a directory)", name));
        auto plain = mcpp::pack::compress::decompress(*bytes, a);
        if (!plain) return std::unexpected(std::format("{}: {}", name, plain.error()));
        const auto tarFile = into / ".mcpp-merge.tar";
        if (!spit(tarFile, *plain)) return std::unexpected(std::format("cannot write '{}'", tarFile.string()));
        std::vector<std::string> argv{"tar"};
        if constexpr (mcpplibs::os::is_windows) argv.push_back("--force-local");
        argv.insert(argv.end(), {"-xf", tarFile.string(), "-C", into.string()});
        auto r = mcpp::run::execute(argv);
        fs::remove(tarFile, ec);
        if (r.exit_code != 0) return std::unexpected(std::format("tar -xf {} exited with {}: {}", name,
                                                                 r.exit_code, r.output));
    }
    // The archive's one top-level directory.
    std::vector<fs::path> tops;
    for (auto const& e : fs::directory_iterator(into, ec))
        if (e.is_directory(ec)) tops.push_back(e.path());
    if (tops.size() != 1)
        return std::unexpected(std::format("'{}' has {} top-level directories; a package has one",
                                           name, tops.size()));
    return tops.front();
}

// ─── launchers ────────────────────────────────────────────────────────────

std::string_view arch_dir(std::string_view machine) {
    if (machine == "x86_64" || machine == "amd64" || machine == "AMD64") return "x86_64";
    if (machine == "aarch64" || machine == "arm64" || machine == "ARM64") return "aarch64";
    return {};
}

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

}  // namespace

std::string posix_launcher(const Launch& l) {
    std::string arches;
    for (auto const& [arch, _] : l.entry) arches += (arches.empty() ? "" : " ") + arch;
    std::string cases;
    for (auto const& [arch, file] : l.entry)
        cases += std::format("  {}) image=\"$top/lib/{}/{}\" ;;\n", arch, arch, file);
    // The runner's argv: its program found in the package, then on PATH; `{}`
    // the image; every other word as written.
    std::string args;
    for (std::size_t i = 1; i < l.runner.size(); ++i)
        args += l.runner[i] == "{}" ? std::string(" \"$image\"") : " " + sh_word(l.runner[i]);
    const std::string program = l.runner.empty() ? std::string("{}") : l.runner.front();
    std::string find;
    if (!l.loader.empty())
        find = std::format(
            "runner=\"$top/libexec/{0}/$os-$arch/{0}\"\n"
            "[ -x \"$runner\" ] || runner={1}\n", l.loader, sh_word(program));
    else
        find = std::format("runner={}\n", sh_word(program));
    return std::format(
        "#!/bin/sh\n"
        "# Generated by mcpp pack (SPEC-014 §4.3): runs {0} on this machine's\n"
        "# architecture through the platform's runner.\n"
        "top=$(CDPATH= cd -- \"$(dirname -- \"$0\")/..\" && pwd)\n"
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
        "  *) echo \"{0}: this machine is $arch; the package holds {2}\" >&2; exit 126 ;;\n"
        "esac\n"
        "{3}"
        "exec \"$runner\"{4} \"$@\"\n",
        l.name, cases, arches, find, args);
}

std::string windows_launcher(const Launch& l) {
    std::string arches;
    for (auto const& [arch, _] : l.entry) arches += (arches.empty() ? "" : " ") + arch;
    std::string picks;
    for (auto const& [arch, file] : l.entry)
        picks += std::format("if \"%arch%\"==\"{0}\" set \"image=%top%\\lib\\{0}\\{1}\"\r\n", arch, file);
    std::string args;
    for (std::size_t i = 1; i < l.runner.size(); ++i)
        args += l.runner[i] == "{}" ? std::string(" \"%image%\"") : " \"" + l.runner[i] + "\"";
    const std::string program = l.runner.empty() ? std::string("{}") : l.runner.front();
    std::string find = std::format("set \"runner={}\"\r\n", program);
    if (!l.loader.empty())
        find = std::format(
            "set \"runner=%top%\\libexec\\{0}\\windows-%arch%\\{0}.exe\"\r\n"
            "if not exist \"%runner%\" set \"runner={1}\"\r\n", l.loader, program);
    return std::format(
        "@echo off\r\n"
        "rem Generated by mcpp pack (SPEC-014 §4.3): runs {0} on this machine's\r\n"
        "rem architecture through the platform's runner.\r\n"
        "setlocal\r\n"
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
        l.name, picks, arches, find, args);
}

// ─── mcpp-package.json ────────────────────────────────────────────────────

namespace {

std::string role_of(std::string_view path, bool executable) {
    auto ends = [&](std::string_view s) { return path.ends_with(s); };
    const auto file = path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1);
    if (path.starts_with("libexec/")) return "loader";
    if (file.starts_with("README") || file.starts_with("LICENSE") || file == "HOST-REQUIREMENTS"
        || path.starts_with("share/licenses/"))
        return "metadata";
    if (ends(".dll") || ends(".dylib") || path.find(".so") != std::string_view::npos
        || ends(".okso"))
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
    std::error_code ec;
    pkg.files.clear();
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        if (e.is_symlink(ec)) { if (e.is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (!e.is_regular_file(ec)) continue;
        const auto rel = e.path().lexically_relative(root).generic_string();
        if (rel == kFileName) continue;
        File f;
        f.path = rel;
        if (auto r = stated.role.find(rel); r != stated.role.end()) f.role = r->second;
        else f.role = role_of(rel, executable_file(e.path()));
        if (auto l = stated.leg.find(rel); l != stated.leg.end()) f.leg = l->second;
        else if (pkg.legs.size() == 1 && f.role != "metadata" && f.role != "resource") f.leg = pkg.legs.front();
        f.sha256 = sha256_file(e.path());
        pkg.files.push_back(std::move(f));
    }
    std::sort(pkg.files.begin(), pkg.files.end(),
              [](const File& a, const File& b) { return a.path < b.path; });
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
    struct Item { std::string path; char kind; std::string data; };
    std::vector<Item> items;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        Item item;
        item.path = e.path().lexically_relative(root).generic_string();
        if (e.is_symlink(ec)) {
            item.kind = 'l';
            item.data = fs::read_symlink(e.path(), ec).generic_string();
            if (e.is_directory(ec)) it.disable_recursion_pending();
        } else if (e.is_directory(ec)) {
            item.kind = 'd';
        } else if (e.is_regular_file(ec)) {
            item.kind = executable_file(e.path()) ? 'x' : 'f';
            auto data = slurp(e.path());
            if (!data) return std::unexpected(std::format("cannot read '{}'", e.path().string()));
            item.data = std::move(*data);
        } else {
            continue;
        }
        items.push_back(std::move(item));
    }
    if (ec) return std::unexpected(std::format("cannot walk '{}': {}", root.string(), ec.message()));
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
    std::string out = "MCPPTREE1\n";
    auto put = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    for (auto const& i : items) {
        out.push_back(i.kind);
        put(i.path.size(), 4);
        out += i.path;
        put(i.data.size(), 8);
        out += i.data;
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

}  // namespace

int main(int argc, char** argv) {
    auto cache = os::dirs::cache();
    if (!cache) return fail("this machine names no cache directory for its user");
    const fs::path base = *cache / "mcpp" / "single-file";
    const fs::path root = base / std::string(kDigest);
    std::error_code ec;
    if (!fs::exists(root / ".mcpp-complete", ec)) {
        fs::create_directories(base, ec);
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

// The xlings system key of a triple's system.
std::string xlings_system(std::string_view triple) {
    if (triple.find("-windows") != std::string_view::npos) return "windows";
    if (triple.find("-macos") != std::string_view::npos || triple.find("-darwin") != std::string_view::npos
        || triple.find("-apple") != std::string_view::npos)
        return "macosx";
    return "linux";
}

std::string lua_list(const std::vector<std::string>& v) {
    std::string out = "{";
    for (std::size_t i = 0; i < v.size(); ++i) out += std::format("{}\"{}\"", i ? ", " : "", v[i]);
    return out + "}";
}

}  // namespace

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
        std::string deps;
        if (first.mode != "self-contained")
            deps = std::format("            deps = {{ \"{}\" }},\n",
                               std::filesystem::path(first.runner.front()).stem().string());
        for (auto system : { "linux", "macosx", "windows" })
            blocks[system] = std::format(
                "{0}            [\"latest\"] = {{ ref = \"{1}\" }},\n"
                "            [\"{1}\"] = {{\n                url = \"{2}/{3}\",\n                sha256 = \"{4}\",\n            }},\n",
                deps, first.version, prefix, a.file.filename().string(), a.sha256);
    } else {
        // system -> (arch -> asset)
        std::map<std::string, std::map<std::string, const Asset*>> bySystem;
        for (auto const& a : assets) {
            if (a.pkg.legs.size() != 1)
                return std::unexpected(std::format("{} holds {} legs; an entry per system describes "
                                                   "packages of one leg", a.file.filename().string(),
                                                   a.pkg.legs.size()));
            const auto& triple = a.pkg.legs.front();
            const auto arch = triple.substr(0, triple.find('-'));
            bySystem[xlings_system(triple)][arch] = &a;
            if (std::ranges::find(archs, arch) == archs.end()) archs.push_back(arch);
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
                shas += std::format("                    {} = \"{}\",\n", arch, a->sha256);
            }
            blocks[system] = std::format(
                "            [\"latest\"] = {{ ref = \"{0}\" }},\n"
                "            [\"{0}\"] = {{\n                url = \"{1}/{2}\",\n                sha256 = {{\n{3}                }},\n            }},\n",
                first.version, prefix, tmpl, shas);
        }
    }
    std::ranges::sort(archs);
    std::string xpm;
    for (auto const& [system, text] : blocks) xpm += std::format("        {} = {{\n{}        }},\n", system, text);

    // Where the program a person runs lies in the installed tree.
    std::string bindir = "\"bin\"", alias;
    if (universal) {
        alias = std::format("if os.host() == \"windows\" then opts.alias = \"{}.cmd\" end\n    ", first.name);
    } else {
        const auto program = std::filesystem::path(first.program);
        bindir = program.has_parent_path() ? std::format("\"{}\"", program.parent_path().generic_string())
                                           : std::string("\".\"");
    }
    const auto wrapper = std::format("{}-{}-", first.name, first.version);
    return std::format(
        "-- Generated by `mcpp emit xpkg --package` from the packages' mcpp-package.json\n"
        "-- (SPEC-014 §8).\n"
        "package = {{\n"
        "    spec = \"2\",\n"
        "    name = \"{0}\",\n"
        "    description = \"{0} {1}{2}\",\n"
        "    type = \"package\",\n"
        "    archs = {3},\n"
        "    programs = {{\"{0}\"}},\n"
        "    xvm_enable = true,\n"
        "    xpm = {{\n{4}    }},\n"
        "}}\n\n"
        "import(\"xim.libxpkg.pkginfo\")\n"
        "import(\"xim.libxpkg.xvm\")\n\n"
        "function install()\n"
        "    -- The archive's one top-level directory is the package.\n"
        "    os.tryrm(pkginfo.install_dir())\n"
        "    for _, d in ipairs(os.dirs(\"{5}*\")) do\n"
        "        os.mv(d, pkginfo.install_dir())\n"
        "        return true\n"
        "    end\n"
        "    return false\n"
        "end\n\n"
        "function config()\n"
        "    local opts = {{ bindir = path.join(pkginfo.install_dir(), {6}) }}\n"
        "    {7}xvm.add(\"{0}\", opts)\n"
        "    return true\n"
        "end\n\n"
        "function uninstall()\n"
        "    xvm.remove(\"{0}\")\n"
        "    return true\n"
        "end\n",
        first.name, first.version,
        universal ? std::format(", one package for every system ({})", first.platform) : std::string(),
        lua_list(archs), xpm, wrapper, bindir, alias);
}

// ─── --merge ──────────────────────────────────────────────────────────────

std::expected<fs::path, std::string> merge(const MergeRequest& req) {
    if (req.inputs.size() < 2)
        return std::unexpected(std::string("--merge reads two packages or more"));
    std::error_code ec;
    const auto work = fs::temp_directory_path(ec)
        / std::format("mcpp-merge-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(work, ec);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};

    std::vector<std::pair<fs::path, Package>> trees;
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
        auto root = unpack(req.inputs[i], work / std::format("in{}", i));
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
    const auto wrapper = trees.front().first.filename();
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
        for (auto const& f : pkg.files) {
            if (auto it = seen.find(f.path); it != seen.end()) {
                if (it->second.first != f.sha256)
                    return std::unexpected(std::format(
                        "--merge: '{}' is in {} with sha256 {} and in {} with sha256 {}; one package "
                        "holds one of each file", f.path, it->second.second, it->second.first,
                        req.inputs[i].string(), f.sha256));
                continue;
            }
            seen[f.path] = {f.sha256, req.inputs[i].string()};
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

    // The container: as asked, else as the first input was written.
    std::string format = req.format;
    const auto firstName = req.inputs.front().filename().string();
    if (format.empty())
        format = fs::is_directory(req.inputs.front(), ec) ? "dir" : firstName.ends_with(".zip") ? "zip" : "tar";
    auto compression = req.compression;
    if (!compression.stated && format == "tar") {
        using mcpp::pack::compress::Algorithm;
        compression.algorithm = firstName.ends_with(".tar.zst") ? Algorithm::Zstd
                              : firstName.ends_with(".tar.xz")  ? Algorithm::Xz
                              : firstName.ends_with(".tar")     ? Algorithm::None
                                                                : Algorithm::Gzip;
    }
    fs::path dest = req.output;
    const auto ext = format == "zip" ? std::string(".zip") : format == "dir" ? std::string()
                   : std::string(mcpp::pack::compress::tar_extension(compression.algorithm));
    if (dest.empty()) dest = fs::current_path(ec) / (wrapper.string() + ext);
    if (format == "dir") {
        fs::remove_all(dest, ec);
        fs::create_directories(dest.parent_path(), ec);
        fs::copy(out, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return std::unexpected(std::format("cannot write '{}': {}", dest.string(), ec.message()));
        return dest;
    }
    if (format != "tar" && format != "zip")
        return std::unexpected(std::format("--merge writes tar, zip or dir; '{}' is a format a package "
                                           "provides, which reads a staged tree: merge to dir first",
                                           format));
    auto r = format == "zip" ? write_zip(out, dest, compression) : write_tar(out, dest, compression);
    if (!r) return std::unexpected(r.error());
    return dest;
}

}  // namespace mcpp::pack::distribution
