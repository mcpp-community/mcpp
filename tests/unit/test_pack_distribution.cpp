// The distribution half of `mcpp pack` (SPEC-014): the codecs, the zip writer's
// deflated and aligned members, SHA-256, the tar mcpp writes, what `unpack`
// refuses to create, the launchers, the package metadata, `--merge` and the
// single file's payload.

import std;
import mcpplibs.mtest;
import mcpp.pack.compress;
import mcpp.pack.distribution;
import mcpp.pack.zip;

using namespace mcpplibs::mtest;

namespace fs = std::filesystem;
namespace c = mcpp::pack::compress;
namespace d = mcpp::pack::distribution;

namespace {

struct Tmp {
    fs::path path;
    Tmp() {
        path = fs::temp_directory_path() / std::format("mcpp-dist-{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(path);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(path, ec); }
};

void write(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::uint32_t le32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at]))
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

// A zip of `entries` at `dir/name`, members stored.
fs::path zip_of(const fs::path& dir, std::string_view name, const std::vector<mcpp::pack::zip::Entry>& entries) {
    const auto out = dir / name;
    expect(bool(mcpp::pack::zip::write(out, entries)));
    return out;
}

}  // namespace

UTest parses_algorithms_and_levels{"PackCompress/ParsesAlgorithmsAndLevels", [] {
    auto z = c::parse("zstd:19");
    expect(fatal(bool(z)));
    expect(eq(z->algorithm, c::Algorithm::Zstd));
    expect(eq(z->level, 19));
    expect(bool(z->stated));
    expect(bool(c::parse("none")));
    expect(!c::parse("none:3"));
    expect(!c::parse("gzip:10"));
    expect(!c::parse("brotli"));
    expect(eq(c::tar_extension(c::Algorithm::Xz), ".tar.xz"));
    expect(eq(c::tar_extension(c::Algorithm::None), ".tar"));
}};

// An unstated level is -1, the codec's default; every stated one is honoured,
// `xz:0` and `zstd:1` too.
UTest a_stated_level_is_honoured{"PackCompress/AStatedLevelIsHonoured", [] {
    expect(eq(c::Choice{}.level, -1));
    expect(eq(c::parse("xz")->level, -1));
    expect(eq(c::parse("xz:0")->level, 0));
    expect(eq(c::parse("zstd:1")->level, 1));
    std::string text;
    for (int i = 0; i < 4000; ++i) text += std::format("{} ", i * 7919 % 10007);
    auto fastest = c::bytes(text, *c::parse("xz:0"));
    auto preset = c::bytes(text, *c::parse("xz"));
    expect(fatal(fastest && preset));
    expect(ne(*fastest, *preset)) << "xz:0 was written at the default preset";
    auto z1 = c::bytes(text, *c::parse("zstd:1"));
    auto z19 = c::bytes(text, *c::parse("zstd"));
    expect(fatal(z1 && z19));
    expect(ne(*z1, *z19)) << "zstd:1 was written at the default level";
}};

// A gzip member names no file, no time and the system 255, whatever host
// wrote it.
UTest a_gzip_header_is_the_same_on_every_host{"PackCompress/AGzipHeaderIsTheSameOnEveryHost", [] {
    auto gz = c::bytes("the same bytes", c::Choice{});
    expect(fatal(bool(gz)));
    expect(fatal(ge(gz->size(), 10u)));
    expect(eq(static_cast<unsigned char>((*gz)[0]), 0x1f));
    expect(eq(static_cast<unsigned char>((*gz)[3]), 0)) << "a name, a comment or an extra field";
    expect(eq(le32(*gz, 4), 0u)) << "a modification time";
    expect(eq(static_cast<unsigned char>((*gz)[9]), 255)) << "the system byte";
}};

UTest every_codec_round_trips{"PackCompress/EveryCodecRoundTrips", [] {
    std::string text;
    for (int i = 0; i < 4000; ++i) text += std::format("line {} of a package\n", i % 37);
    for (auto a : { c::Algorithm::Gzip, c::Algorithm::Zstd, c::Algorithm::Xz, c::Algorithm::None }) {
        c::Choice ch;
        ch.algorithm = a;
        auto packed = c::bytes(text, ch);
        expect(fatal(bool(packed))) << c::name(a);
        if (a != c::Algorithm::None) expect(lt(packed->size(), text.size())) << c::name(a);
        auto back = c::decompress(*packed, a);
        expect(fatal(bool(back))) << c::name(a);
        expect(eq(*back, text)) << c::name(a);
    }
    auto raw = c::deflate_raw(text, -1);
    expect(fatal(bool(raw)));
    auto back = c::inflate_raw(*raw, text.size());
    expect(fatal(bool(back)));
    expect(eq(*back, text));
}};

// The name of a tar archive states its compression, the inverse of
// tar_extension; a test archive is written and read by it.
UTest a_tar_name_states_its_compression{"PackCompress/ATarNameStatesItsCompression", [] {
    for (auto a : { c::Algorithm::Gzip, c::Algorithm::Zstd, c::Algorithm::Xz, c::Algorithm::None })
        expect(eq(c::tar_algorithm(std::format("t{}", c::tar_extension(a))), a)) << c::name(a);
    expect(eq(c::tar_algorithm("t.tgz"), c::Algorithm::Gzip));
    expect(!c::tar_algorithm("t.zip"));
    expect(!c::tar_algorithm("t.gz"));
    expect(!c::tar_algorithm("tests"));
}};

// zlib counts in 32 bits; the input is handed over in pieces, so one gzip
// member holds more than one piece (1 GiB) and comes back whole. A member cut
// short is refused, not waited on.
UTest a_gzip_member_larger_than_one_piece_round_trips{"PackCompress/AGzipMemberLargerThanOnePieceRoundTrips", [] {
    std::string big((std::size_t{1} << 30) + 4096, '\0');
    big.replace(big.size() - 7, 7, "the end");
    auto packed = c::bytes(big, *c::parse("gzip:1"));
    if (auto&& failed = expect(fatal(bool(packed))); !failed) failed << packed.error();
    auto back = c::decompress(*packed, c::Algorithm::Gzip);
    if (auto&& failed = expect(fatal(bool(back))); !failed) failed << back.error();
    expect(eq(back->size(), big.size()));
    expect(back->ends_with("the end"));
    big = {};
    back = {};
    expect(!(c::decompress(std::string_view(*packed).substr(0, packed->size() / 2), c::Algorithm::Gzip)));
}};

UTest sha256_of_known_vectors{"PackDistribution/Sha256OfKnownVectors", [] {
    expect(eq(d::sha256(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    expect(eq(d::sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    expect(eq(d::sha256(std::string(1000, 'a')), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}};

UTest an_aligned_member_is_stored_on_its_boundary{"PackZip/AnAlignedMemberIsStoredOnItsBoundary", [] {
    Tmp t;
    write(t.path / "a.oke", std::string(3000, 'x'));
    write(t.path / "libb.okso", std::string(70000, 'y'));
    std::vector<mcpp::pack::zip::Entry> entries{
        { "a.oke", t.path / "a.oke", false, {} },
        { "libb.okso", t.path / "libb.okso", false, {} },
    };
    mcpp::pack::zip::WriteOptions o;
    o.align = 65536;
    expect(fatal(bool(mcpp::pack::zip::write(t.path / "out.zip", entries, o))));
    std::ifstream in(t.path / "out.zip", std::ios::binary);
    std::string b{std::istreambuf_iterator<char>(in), {}};
    // Each member's data, found through the central directory, starts on a
    // 64 KiB boundary and is stored.
    std::size_t eocd = b.rfind(std::string("PK\x05\x06", 4));
    expect(fatal(ne(eocd, std::string::npos)));
    std::size_t at = le32(b, eocd + 16);
    for (int n = 0; n < 2; ++n) {
        expect(fatal(eq(le32(b, at), 0x02014b50u)));
        const auto method = static_cast<unsigned char>(b[at + 10]);
        const auto nameLen = static_cast<std::size_t>(static_cast<unsigned char>(b[at + 28]))
                           | static_cast<std::size_t>(static_cast<unsigned char>(b[at + 29])) << 8;
        const auto local = le32(b, at + 42);
        const auto lname = static_cast<std::size_t>(static_cast<unsigned char>(b[local + 26]))
                         | static_cast<std::size_t>(static_cast<unsigned char>(b[local + 27])) << 8;
        const auto lextra = static_cast<std::size_t>(static_cast<unsigned char>(b[local + 28]))
                          | static_cast<std::size_t>(static_cast<unsigned char>(b[local + 29])) << 8;
        expect(eq(method, 0));
        expect(eq((local + 30 + lname + lextra) % 65536, 0u));
        at += 46 + nameLen;
    }
}};

UTest deflate_when_it_shrinks{"PackZip/DeflateWhenItShrinks", [] {
    Tmp t;
    write(t.path / "text", std::string(10000, 'z'));
    mcpp::pack::zip::WriteOptions o;
    o.deflate = true;
    expect(fatal(bool(mcpp::pack::zip::write(t.path / "out.zip", { { "text", t.path / "text", false, {} } }, o))));
    expect(lt(fs::file_size(t.path / "out.zip"), 1000u));
    // And it reads back.
    auto root = d::unpack(t.path / "out.zip", t.path / "x");
    expect(fatal(!root)) << "a zip without one top-level directory is not a package";
}};

UTest launchers_pick_the_architecture_and_the_runner{"PackDistribution/LaunchersPickTheArchitectureAndTheRunner", [] {
    d::Launch l;
    l.name = "hello";
    l.runner = { "okrun", "{}" };
    l.entry = { { "x86_64", "hello.oke" }, { "aarch64", "hello.oke" } };
    l.loader = "okrun";
    const auto sh = d::posix_launcher(l);
    expect(sh.starts_with("#!/bin/sh\n"));
    expect(ne(sh.find("x86_64|amd64|AMD64) arch=x86_64"), std::string::npos));
    expect(ne(sh.find("'aarch64') entry='hello.oke' ;;"), std::string::npos));
    expect(ne(sh.find("image=\"$top/lib/$arch/$entry\""), std::string::npos));
    expect(ne(sh.find("runner=\"$top/libexec/$loader/$os-$arch/$loader\""), std::string::npos));
    expect(ne(sh.find("exec \"$runner\" \"$image\" \"$@\""), std::string::npos));
    // The package is found through a link to the launcher, without readlink -f.
    expect(ne(sh.find("readlink -- \"$self\""), std::string::npos));
    expect(eq(sh.find("readlink -f"), std::string::npos));
    // A POSIX shell on Windows hands over to the program of the same name.
    expect(ne(sh.find("MINGW*|MSYS*|CYGWIN*) exec \"$top/bin/\"'hello'.exe \"$@\""), std::string::npos)) << sh;
    // On macOS a runner carrying the quarantine mark of a browser's download is
    // named, with the command that removes the mark (mcpp design 2026-10-30
    // part 2 §10 D6), before it is started.
    const auto mark = sh.find("xattr -p com.apple.quarantine \"$runner\"");
    expect(ne(mark, std::string::npos)) << sh;
    expect(ne(sh.find("xattr -dr com.apple.quarantine"), std::string::npos)) << sh;
    expect(lt(mark, sh.find("exec \"$runner\""))) << sh;
}};

// The runner's words expand as `mcpp run` expands them: a word's `{}` is the
// image inside the word as well as alone; with no `{}` the image is appended.
UTest launchers_expand_the_runner_as_mcpp_run_does{"PackDistribution/LaunchersExpandTheRunnerAsMcppRunDoes", [] {
    d::Launch l;
    l.name = "hello";
    l.entry = { { "x86_64", "hello.oke" } };
    l.runner = { "okrun", "--image={}", "it's" };
    expect(ne(d::posix_launcher(l).find("exec \"$runner\" '--image='\"$image\" 'it'\\''s' \"$@\""), std::string::npos)) << d::posix_launcher(l);
    l.runner = { "okrun", "--quiet" };
    expect(ne(d::posix_launcher(l).find("exec \"$runner\" '--quiet' \"$image\" \"$@\""), std::string::npos))
        << d::posix_launcher(l);
}};

// The Windows launcher is a program (design 2026-10-29 §4.3): its source states
// the launch as constants, finds the package from its own path, reads the
// machine's architecture, and expands the runner's words as `mcpp run` does.
// Any word is stated, quotes and percent signs included: a raw string holds it.
UTest the_windows_launcher_is_a_program{"PackDistribution/TheWindowsLauncherIsAProgram", [] {
    d::Launch l;
    l.name = "hello";
    l.entry = { { "x86_64", "hello.oke" }, { "aarch64", "hello.oke" } };
    l.runner = { "okrun", "--say=\"50%\"", "{}" };
    l.loader = "okrun";
    const auto src = d::exe_launcher_main(l);
    expect(ne(src.find("import mcpplibs.os;"), std::string::npos)) << src;
    expect(ne(src.find("os::env::image()"), std::string::npos)) << src;
    expect(ne(src.find("PROCESSOR_ARCHITEW6432"), std::string::npos)) << src;
    expect(ne(src.find("R\"mcpp(--say=\"50%\")mcpp\""), std::string::npos)) << src;
    // One program for every set of legs: the entry's name, the architectures
    // the directories under `lib/`.
    expect(ne(src.find("kEntry = R\"mcpp(hello.oke)mcpp\""), std::string::npos)) << src;
    expect(eq(src.find("R\"mcpp(aarch64)mcpp\""), std::string::npos)) << src;
    expect(ne(src.find("fs::directory_iterator(top / \"lib\""), std::string::npos)) << src;
    expect(ne(src.find("(\"windows-\" + arch)"), std::string::npos)) << src;
    expect(ne(src.find("os::process::exec"), std::string::npos)) << src;
    const auto manifest = d::exe_launcher_manifest("hello", "1.0.0");
    expect(ne(manifest.find("mcpplibs.os = \"0.4.0\""), std::string::npos)) << manifest;
    expect(ne(manifest.find("[targets.hello]"), std::string::npos)) << manifest;
}};

// The POSIX launcher, run through a link to it from another directory, finds
// its package and starts the runner with the image spliced into its word.
UTest a_posix_launcher_runs_through_a_link{"PackDistribution/APosixLauncherRunsThroughALink", [] {
    if (!fs::exists("/bin/sh")) skip("no /bin/sh");
    std::string arch;
    {
        Tmp probe;
        if (std::system(std::format("uname -m > '{}'", (probe.path / "m").string()).c_str()) != 0)
            skip("no uname");
        arch = slurp(probe.path / "m");
    }
    arch = arch.starts_with("x86_64") || arch.starts_with("amd64") ? "x86_64"
         : arch.starts_with("aarch64") || arch.starts_with("arm64") ? "aarch64" : "";
    if (arch.empty()) skip("no launcher row for this machine");
    Tmp t;
    const auto pkg = t.path / "my pkg";
    d::Launch l;
    l.name = "hello";
    l.entry = { { "x86_64", "hello.oke" }, { "aarch64", "hello.oke" } };
    l.runner = { "fakerun", "--image={}" };
    write(pkg / "bin/hello", d::posix_launcher(l));
    write(pkg / "lib" / arch / "hello.oke", "image");
    write(t.path / "path/fakerun", "#!/bin/sh\nprintf '%s|' \"$@\"\n");
    for (auto f : { pkg / "bin/hello", t.path / "path/fakerun" })
        fs::permissions(f, fs::perms::owner_all, fs::perm_options::add);
    fs::create_directories(t.path / "elsewhere");
    fs::create_symlink(pkg / "bin/hello", t.path / "elsewhere/hello");
    const auto out = t.path / "out";
    const auto rc = std::system(std::format("PATH='{}':\"$PATH\" '{}' 'a b' c > '{}' 2>&1",
                                            (t.path / "path").string(), (t.path / "elsewhere/hello").string(),
                                            out.string()).c_str());
    expect(eq(rc, 0)) << slurp(out);
    // The physical path: the launcher follows links to the directories too
    // (macOS's temporary directory is under a link, /var -> /private/var).
    expect(eq(slurp(out), std::format("--image={}|a b|c|",
                                      (fs::canonical(pkg) / "lib" / arch / "hello.oke").string())));
}};

// A shared object is `.so` or `.so.<version>`; a name that merely holds `.so`
// is not one.
UTest a_library_is_named_so_or_so_version{"PackDistribution/ALibraryIsNamedSoOrSoVersion", [] {
    Tmp t;
    const auto root = t.path / "app-1.0";
    for (auto f : { "lib/libz.so", "lib/libz.so.1.3", "share/notes.sol", "share/a.so.x", "share/b.socket" })
        write(root / f, "x");
    d::Package p;
    p.name = "app";
    p.version = "1.0";
    auto described = d::describe(root, p, {});
    if (auto&& failed = expect(fatal(bool(described))); !failed) failed << described.error();
    std::map<std::string, std::string> roles;
    for (auto const& f : described->files) roles[f.path] = f.role;
    expect(eq(roles["lib/libz.so"], "library"));
    expect(eq(roles["lib/libz.so.1.3"], "library"));
    expect(eq(roles["share/notes.sol"], "resource"));
    expect(eq(roles["share/a.so.x"], "resource"));
    expect(eq(roles["share/b.socket"], "resource"));
}};

// `read` refuses a document that names a path outside the package.
UTest read_refuses_a_path_outside_the_package{"PackDistribution/ReadRefusesAPathOutsideThePackage", [] {
    Tmp t;
    for (auto bad : { "../outside", "/etc/passwd", "a/../../b", "./bin/x", "bin\\\\x", "" }) {
        write(t.path / "mcpp-package.json",
              std::format(R"({{"schema": 1, "name": "x", "version": "1", "files": [{{"path": "{}", "role": "resource", "sha256": ""}}]}})",
                          bad));
        auto r = d::read(t.path);
        expect(fatal(!r)) << bad;
        if (auto&& failed = expect(ne(r.error().find("not a path inside the package"), std::string::npos)); !failed) failed << r.error();
    }
}};

UTest package_metadata_describes_every_file{"PackDistribution/PackageMetadataDescribesEveryFile", [] {
    Tmp t;
    const auto root = t.path / "app-1.0-openkal";
    write(root / "bin/app", "#!/bin/sh\n");
    write(root / "lib/x86_64/app.oke", "image");
    write(root / "share/data.txt", "data");
    write(root / "LICENSE", "license");
    d::Package p;
    p.name = "app";
    p.version = "1.0";
    p.legs = { "x86_64-openkal" };
    p.runner = { "okrun", "{}" };
    p.entry = { { "x86_64", "lib/x86_64/app.oke" } };
    d::Stated s;
    s.role["bin/app"] = "launcher";
    auto described = d::describe(root, p, s);
    expect(fatal(bool(described)));
    auto back = d::read(root);
    if (auto&& failed = expect(fatal(bool(back))); !failed) failed << back.error();
    std::map<std::string, std::string> roles;
    for (auto const& f : back->files) roles[f.path] = f.role;
    expect(eq(roles["bin/app"], "launcher"));
    expect(eq(roles["lib/x86_64/app.oke"], "image"));
    expect(eq(roles["share/data.txt"], "resource"));
    expect(eq(roles["LICENSE"], "metadata"));
    expect(eq(back->entry.at("x86_64"), "lib/x86_64/app.oke"));
    for (auto const& f : back->files)
        expect(eq(f.sha256, d::sha256_file(root / f.path))) << f.path;
}};

UTest tree_payload_is_ordered_records{"PackDistribution/TreePayloadIsOrderedRecords", [] {
    Tmp t;
    write(t.path / "b/x", "12");
    write(t.path / "a", "3");
    auto payload = d::tree_payload(t.path);
    expect(fatal(bool(payload)));
    expect(payload->starts_with("MCPPTREE1\n"));
    // `a` before `b` before `b/x`.
    const auto a = payload->find("a"), b = payload->find("b/x");
    expect(lt(a, b));
}};

// ─── the tar mcpp writes ──────────────────────────────────────────────────

namespace {

// A tree with a directory, an executable, a link, and a name and a link target
// past the 100 bytes of a ustar field.
fs::path sample_tree(const fs::path& dir) {
    const auto root = dir / "app-1.0";
    write(root / "bin/app", "#!/bin/sh\necho app\n");
    fs::permissions(root / "bin/app", fs::perms::owner_exec, fs::perm_options::add);
    write(root / "share/data.txt", "data");
    write(root / "share" / std::string(60, 'd') / (std::string(60, 'e') + ".txt"), "deep");
    fs::create_directories(root / "empty");
    fs::create_symlink("data.txt", root / "share/link");
    fs::create_symlink(std::string(60, 'd') + "/" + std::string(60, 'e') + ".txt", root / "share/far");
    return root;
}

const c::Choice kPlain{ c::Algorithm::None, -1, false };

}  // namespace

// Two writes of one tree are the same bytes, its times and modes changed in
// between: no owner, no time, the modes normalised.
UTest the_same_tree_is_the_same_bytes{"PackTar/TheSameTreeIsTheSameBytes", [] {
    Tmp t;
    const auto root = sample_tree(t.path);
    for (auto ch : { kPlain, c::Choice{} }) {
        expect(fatal(bool(d::write_tar(root, t.path / "one", ch))));
        fs::last_write_time(root / "share/data.txt", fs::file_time_type::clock::now() - std::chrono::hours(48));
        fs::permissions(root / "share/data.txt", fs::perms::group_write, fs::perm_options::add);
        expect(fatal(bool(d::write_tar(root, t.path / "two", ch))));
        expect(eq(slurp(t.path / "one"), slurp(t.path / "two"))) << c::name(ch.algorithm);
    }
    expect(fatal(bool(d::write_tar(root, t.path / "plain.tar", kPlain))));
    const auto tar = slurp(t.path / "plain.tar");
    expect(fatal(ge(tar.size(), 1024u)));
    expect(eq(tar.substr(0, 9), std::string("app-1.0/\0", 9)));
    expect(eq(tar.substr(108, 8), std::string("0000000\0", 8))) << "uid";
    expect(eq(tar.substr(116, 8), std::string("0000000\0", 8))) << "gid";
    expect(eq(tar.substr(136, 12), std::string("00000000000\0", 12))) << "mtime";
    expect(eq(tar.substr(257, 6), std::string("ustar\0", 6)));
    expect(eq(tar.substr(265, 32), std::string(32, '\0'))) << "uname";
    expect(eq(tar.size() % 512, 0u));
}};

// What mcpp writes, `unpack` reads back: files, modes, links, long names.
UTest unpack_reads_what_write_tar_wrote{"PackTar/UnpackReadsWhatWriteTarWrote", [] {
    Tmp t;
    const auto root = sample_tree(t.path);
    for (auto ch : { kPlain, c::Choice{}, c::Choice{ c::Algorithm::Zstd, -1, false },
                     c::Choice{ c::Algorithm::Xz, -1, false } }) {
        const auto archive = t.path / ("app" + std::string(c::tar_extension(ch.algorithm)));
        expect(fatal(bool(d::write_tar(root, archive, ch))));
        const auto into = t.path / ("x-" + std::string(c::name(ch.algorithm)));
        auto back = d::unpack(archive, into);
        if (auto&& failed = expect(fatal(bool(back))); !failed) failed << back.error();
        expect(eq(back->filename(), "app-1.0"));
        expect(eq(slurp(*back / "share/data.txt"), "data"));
        expect(eq(slurp(*back / "share" / std::string(60, 'd') / (std::string(60, 'e') + ".txt")), "deep"));
        expect(ne(fs::status(*back / "bin/app").permissions() & fs::perms::owner_exec, fs::perms::none));
        expect(fs::is_directory(*back / "empty"));
        expect(fs::is_symlink(*back / "share/link"));
        expect(eq(fs::read_symlink(*back / "share/far").generic_string(), std::string(60, 'd') + "/" + std::string(60, 'e') + ".txt"));
        expect(eq(slurp(*back / "share/far"), "deep"));
    }
}};

// A tar holding a link out of its tree is refused, and nothing is written.
UTest unpack_refuses_a_link_out_of_the_tree{"PackTar/UnpackRefusesALinkOutOfTheTree", [] {
    Tmp t;
    const auto root = t.path / "app-1.0";
    write(root / "a", "a");
    fs::create_symlink("../../outside", root / "escape");
    expect(fatal(bool(d::write_tar(root, t.path / "app.tar", kPlain))));
    auto r = d::unpack(t.path / "app.tar", t.path / "x");
    expect(fatal(!r));
    if (auto&& failed = expect(ne(r.error().find("outside the package"), std::string::npos)); !failed) failed << r.error();
    expect(fs::is_empty(t.path / "x"));
}};

// ─── what unpack refuses ──────────────────────────────────────────────────

// A member that would land outside the extraction root, or be written through
// a link member, is refused before anything is written.
UTest a_zip_creates_nothing_outside_its_root{"PackUnpack/AZipCreatesNothingOutsideItsRoot", [] {
    Tmp t;
    write(t.path / "f", "content");
    using E = mcpp::pack::zip::Entry;
    const auto f = t.path / "f";
    struct Case { std::string_view label, why; std::vector<E> entries; };
    const std::vector<Case> cases{
        { "dot-dot", "not a path inside", { { "pkg/a", f, false, {} }, { "pkg/../evil", f, false, {} } } },
        { "absolute", "not a path inside", { { "/tmp/mcpp-evil", f, false, {} } } },
        { "drive", "not a path inside", { { "C:/evil", f, false, {} } } },
        { "the root as a file", "not a path inside", { { "pkg/.", f, false, {} }, { ".", f, false, {} } } },
        { "backslash", "not a path inside", { { "pkg\\..\\evil", f, false, {} } } },
        { "link out", "outside the package", { { "pkg/a", f, false, {} }, { "pkg/l", {}, false, "../../outside" } } },
        { "absolute link", "outside the package", { { "pkg/a", f, false, {} }, { "pkg/l", {}, false, "/etc" } } },
        { "link through", "lies under the link", { { "pkg/l", {}, false, "sub" }, { "pkg/l/x", f, false, {} } } },
        { "link chain out", "outside the package",
          { { "pkg/d/up", {}, false, ".." }, { "pkg/l", {}, false, "d/up/../.." }, { "pkg/a", f, false, {} } } },
        { "twice", "twice", { { "pkg/a", f, false, {} }, { "pkg/a", {}, false, "b" } } },
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto into = t.path / std::format("x{}", i);
        auto r = d::unpack(zip_of(t.path, std::format("c{}.zip", i), cases[i].entries), into);
        if (auto&& failed = expect(fatal(!r)); !failed) failed << cases[i].label;
        if (auto&& failed = expect(ne(r.error().find(cases[i].why), std::string::npos)); !failed) failed << cases[i].label << ": " << r.error();
        if (auto&& failed = expect(fs::is_empty(into)); !failed) failed << cases[i].label << ": something was written";
    }
    expect(!fs::exists(t.path / "evil"));
    // A link inside the tree is made, after the files.
    auto ok = d::unpack(zip_of(t.path, "ok.zip", { { "pkg/l", {}, false, "sub" }, { "pkg/sub/x", f, false, {} } }),
                        t.path / "ok");
    if (auto&& failed = expect(fatal(bool(ok))); !failed) failed << ok.error();
    expect(fs::is_symlink(*ok / "l"));
    expect(eq(slurp(*ok / "l/x"), "content"));
}};

// Every truncation of a zip, and every byte of it changed, is an error or a
// tree, never a read past the archive.
UTest a_truncated_or_corrupt_zip_is_an_error{"PackUnpack/ATruncatedOrCorruptZipIsAnError", [] {
    Tmp t;
    write(t.path / "f", std::string(300, 'q'));
    write(t.path / "g", "small");
    mcpp::pack::zip::WriteOptions o;
    o.deflate = true;
    expect(fatal(bool(mcpp::pack::zip::write(t.path / "a.zip", { { "pkg/f", t.path / "f", false, {} },
                                                           { "pkg/g", t.path / "g", true, {} } }, o))));
    const auto whole = slurp(t.path / "a.zip");
    for (std::size_t n = 0; n < whole.size(); ++n) {
        write(t.path / "cut.zip", whole.substr(0, n));
        const auto into = t.path / std::format("cut{}", n);
        expect(!d::unpack(t.path / "cut.zip", into)) << n << " bytes";
        fs::remove_all(into);
    }
    for (std::size_t n = 0; n < whole.size(); ++n) {
        auto bad = whole;
        bad[n] = static_cast<char>(bad[n] ^ 0x5a);
        write(t.path / "bad.zip", bad);
        const auto into = t.path / std::format("bad{}", n);
        (void)d::unpack(t.path / "bad.zip", into);
        fs::remove_all(into);
    }
}};

// A member whose bytes do not match its CRC-32 is refused.
UTest a_crc_mismatch_is_refused{"PackUnpack/ACrcMismatchIsRefused", [] {
    Tmp t;
    write(t.path / "f", "the member's content");
    auto zip = slurp(zip_of(t.path, "a.zip", { { "pkg/f", t.path / "f", false, {} } }));
    const auto at = zip.find("the member's content");
    expect(fatal(ne(at, std::string::npos)));
    zip[at] = 'T';
    write(t.path / "a.zip", zip);
    auto r = d::unpack(t.path / "a.zip", t.path / "x");
    expect(fatal(!r));
    if (auto&& failed = expect(ne(r.error().find("CRC-32"), std::string::npos)); !failed) failed << r.error();
}};

// ─── --merge ──────────────────────────────────────────────────────────────

namespace {

// A described package of one leg under `dir`.
fs::path leg_package(const fs::path& dir, std::string_view leg, std::string_view arch) {
    const auto root = dir / "app-1.0";
    write(root / "lib" / arch / "app.oke", std::format("image of {}", arch));
    write(root / "README.md", "readme");
    d::Package p;
    p.name = "app";
    p.version = "1.0";
    p.legs = { std::string(leg) };
    p.runner = { "okrun", "{}" };
    p.entry = { { std::string(arch), std::format("lib/{}/app.oke", arch) } };
    expect(bool(d::describe(root, p, {})));
    return root;
}

}  // namespace

// A file changed after its package was written is refused, named with both
// digests.
UTest a_file_that_is_not_its_digest_is_refused{"PackMerge/AFileThatIsNotItsDigestIsRefused", [] {
    Tmp t;
    const auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    const auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    write(b / "lib/aarch64/app.oke", "changed");
    d::MergeRequest req;
    req.inputs = { a, b };
    req.output = t.path / "out";
    auto r = d::merge(req);
    expect(fatal(!r));
    if (auto&& failed = expect(ne(r.error().find("lib/aarch64/app.oke"), std::string::npos)); !failed) failed << r.error();
    if (auto&& failed = expect(ne(r.error().find(d::sha256("changed")), std::string::npos)); !failed) failed << r.error();
    if (auto&& failed = expect(ne(r.error().find(d::sha256("image of aarch64")), std::string::npos)); !failed) failed << r.error();
    expect(!fs::exists(t.path / "out"));
}};

// THE LAUNCHERS ARE DERIVED (design 2026-10-29 §4.3): two one-leg trees whose
// POSIX launchers differ merge into the tree of both, its launcher written for
// both legs; the Windows launcher, one program for every set of legs, is taken
// once. A package written before `runs_on` existed is given its pairs.
UTest the_launchers_are_written_for_the_merged_legs{"PackMerge/TheLaunchersAreWrittenForTheMergedLegs", [] {
    Tmp t;
    auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    for (auto const& [root, arch] : { std::pair{ a, "x86_64" }, std::pair{ b, "aarch64" } }) {
        write(root / "bin/app", std::format("#!/bin/sh\n# the launcher of {}\n", arch));
        write(root / "bin/app.exe", std::format("MZ the program of {}", arch));
        auto pkg = d::read(root);
        if (auto&& failed = expect(fatal(bool(pkg))); !failed) failed << pkg.error();
        d::Stated stated;
        stated.role["bin/app"] = "launcher";
        stated.role["bin/app.exe"] = "launcher";
        expect(fatal(bool(d::describe(root, *pkg, stated))));
    }
    d::MergeRequest req;
    req.inputs = { a, b };
    req.format = "dir";
    req.output = t.path / "out";
    auto r = d::merge(req);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    const auto script = slurp(*r / "bin/app");
    expect(ne(script.find("'x86_64') entry='app.oke'"), std::string::npos)) << script;
    expect(ne(script.find("'aarch64') entry='app.oke'"), std::string::npos)) << script;
    expect(eq(slurp(*r / "bin/app.exe"), "MZ the program of x86_64"));
    auto merged = d::read(*r);
    if (auto&& failed = expect(fatal(bool(merged))); !failed) failed << merged.error();
    expect(fatal(bool(merged->runsOnSet)));
    expect(fatal(merged->runsOnWith.contains("okrun")));
    expect(ne(std::ranges::find(merged->runsOnWith["okrun"], "macos-aarch64"), merged->runsOnWith["okrun"].end()));
    for (auto const& f : merged->files)
        expect(eq(d::sha256_file(*r / f.path), f.sha256)) << f.path;
}};

// Merged without `-o`, a tree of a platform's legs is named after the platform
// (design 2026-10-29 §4.2).
UTest a_merged_tree_is_named_after_the_platform{"PackMerge/AMergedTreeIsNamedAfterThePlatform", [] {
    Tmp t;
    std::vector<fs::path> inputs;
    for (auto [leg, arch] : { std::pair{ "x86_64-openkal", "x86_64" }, std::pair{ "aarch64-openkal", "aarch64" } }) {
        const auto root = t.path / arch / std::format("app-1.0-{}", leg);
        write(root / "lib" / arch / "app.oke", std::format("image of {}", arch));
        d::Package p;
        p.name = "app";
        p.version = "1.0";
        p.platform = "mcpplibs.openkal-platform@0.18.0";
        p.legs = { leg };
        p.runner = { "okrun", "{}" };
        p.entry = { { arch, std::format("lib/{}/app.oke", arch) } };
        expect(fatal(bool(d::describe(root, p, {}))));
        inputs.push_back(root);
    }
    std::error_code ec;
    const auto cwd = fs::current_path();
    fs::current_path(t.path, ec);
    d::MergeRequest req;
    req.inputs = inputs;
    req.format = "dir";
    auto r = d::merge(req);
    fs::current_path(cwd, ec);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(eq(r->filename(), "app-1.0-openkal"));
}};

// The legs merge into a new directory; an output that exists, or is an input,
// is refused, and an input written with a trailing separator is its directory.
UTest the_output_is_new_or_a_package{"PackMerge/TheOutputIsNewOrAPackage", [] {
    Tmp t;
    const auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    const auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    d::MergeRequest req;
    req.inputs = { fs::path(a.string() + "/"), b };
    req.output = t.path / "out";
    auto r = d::merge(req);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    auto merged = d::read(*r);
    if (auto&& failed = expect(fatal(bool(merged))); !failed) failed << merged.error();
    expect(eq(merged->legs, (std::vector<std::string>{ "aarch64-openkal", "x86_64-openkal" })));
    expect(eq(slurp(*r / "lib/aarch64/app.oke"), "image of aarch64"));

    // Again into the directory it wrote: a directory is never replaced.
    auto again = d::merge(req);
    expect(fatal(!again));
    if (auto&& failed = expect(ne(again.error().find("exists"), std::string::npos)); !failed) failed << again.error();
    // Onto an input.
    req.output = a;
    expect(fatal(!d::merge(req)));
    expect(fs::exists(a / "mcpp-package.json"));
    // Over a file that is not a package.
    write(t.path / "notes.tar.gz", "my notes");
    req.output = t.path / "notes.tar.gz";
    auto over = d::merge(req);
    expect(fatal(!over));
    if (auto&& failed = expect(ne(over.error().find("not a package"), std::string::npos)); !failed) failed << over.error();
    expect(eq(slurp(t.path / "notes.tar.gz"), "my notes"));
    // Over an archive that is one: replaced.
    req.output = t.path / "app.tar.gz";
    expect(fatal(bool(d::merge(req))));
    auto replaced = d::merge(req);
    if (auto&& failed = expect(fatal(bool(replaced))); !failed) failed << replaced.error();
}};

// What the metadata states reaches the generated Lua as strings: a quote or a
// line break in a name is escaped, never code.
UTest xpkg_descriptor_writes_metadata_as_strings{"PackDistribution/XpkgDescriptorWritesMetadataAsStrings", [] {
    d::Asset a;
    a.file = "evil-1.0-openkal.zip";
    a.top = "evil-1.0-openkal";
    a.sha256 = std::string(64, 'a');
    a.pkg.name = "ev\"il\nos.exit()";
    a.pkg.version = "1.0";
    a.pkg.runner = { "okrun", "{}" };
    a.pkg.entry = { { "x86_64", "lib/x86_64/evil.oke" } };
    auto text = d::xpkg_descriptor({ a }, "https://example.org/v1.0");
    if (auto&& failed = expect(fatal(bool(text))); !failed) failed << text.error();
    if (auto&& failed = expect(ne(text->find("description = \"ev\\\"il\\010os.exit() 1.0"), std::string::npos)); !failed) failed << *text;
    if (auto&& failed = expect(eq(text->find("\nos.exit()"), std::string::npos)); !failed) failed << *text;
    if (auto&& failed = expect(ne(text->find("deps = { \"xim:okrun\" }"), std::string::npos)); !failed) failed << *text;
}};

// The compression fits the container the merge writes and the name it is
// given: zstd in a .tar.gz, or in a zip the first input decided, is refused.
UTest the_compression_fits_the_container{"PackMerge/TheCompressionFitsTheContainer", [] {
    Tmp t;
    const auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    const auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    d::MergeRequest req;
    req.inputs = { a, b };
    req.compression = *c::parse("zstd");
    req.output = t.path / "out.tar.gz";
    auto named = d::merge(req);
    expect(fatal(!named));
    if (auto&& failed = expect(ne(named.error().find(".tar.zst"), std::string::npos)); !failed) failed << named.error();
    req.output = t.path / "out.zip";
    expect(fatal(!d::merge(req)));
    req.output = t.path / "out.tar.zst";
    auto fits = d::merge(req);
    if (auto&& failed = expect(fatal(bool(fits))); !failed) failed << fits.error();
}};

// #860 (design 2026-10-29 §4.4): a self-contained tree of images runs where
// it carries a runner, and its entry names exactly those pairs -- macOS aarch64
// only, Windows x86_64 only -- with no runner to install. A package that needs
// the machine's runner names the pairs the platform's runner exists for.
UTest an_entry_names_where_the_package_runs{"PackDistribution/AnEntryNamesWhereThePackageRuns", [] {
    d::Asset a;
    a.file = "mcpp-ide-0.1.1-openkal.tar.gz";
    a.sha256 = std::string(64, 'c');
    a.top = "mcpp-ide-0.1.1-openkal";
    a.pkg.name = "mcpp-ide";
    a.pkg.version = "0.1.1";
    a.pkg.mode = "self-contained";
    a.pkg.runner = { "okrun", "{}" };
    a.pkg.legs = { "x86_64-openkal", "aarch64-openkal" };
    a.pkg.entry = { { "x86_64", "lib/x86_64/mcpp-ide.oke" }, { "aarch64", "lib/aarch64/mcpp-ide.oke" } };
    a.pkg.systems = { "linux-aarch64", "linux-x86_64", "macos-aarch64", "windows-x86_64" };
    const std::vector<std::string> hosts = { "linux-x86_64", "linux-aarch64", "macos-aarch64",
                                             "macos-x86_64", "windows-x86_64" };
    d::compute_runs_on(a.pkg, hosts, false);
    expect(eq(a.pkg.runsOnSelf, a.pkg.systems));
    expect(fatal(a.pkg.runsOnWith.contains("okrun")));
    expect(eq(a.pkg.runsOnWith.at("okrun"), (std::vector<std::string>{"macos-x86_64"})));
    auto r = d::xpkg_descriptor({ a }, "https://example.org/v0.1.1");
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    auto block = [&](std::string_view system) {
        const auto at = r->find(std::format("        {} = {{", system));
        return at == std::string::npos ? std::string() : r->substr(at, r->find("        },", at) - at);
    };
    if (auto&& failed = expect(ne(block("macosx").find("[\"aarch64\"]"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(eq(block("macosx").find("[\"x86_64\"]"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(ne(block("windows").find("[\"x86_64\"]"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(eq(block("windows").find("[\"aarch64\"]"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(eq(r->find("deps = {"), std::string::npos)); !failed) failed << *r;

    a.pkg.mode = "vendored";
    a.pkg.systems.clear();
    d::compute_runs_on(a.pkg, hosts, false);
    expect(a.pkg.runsOnSelf.empty());
    expect(eq(a.pkg.runsOnWith.at("okrun").size(), hosts.size()));
    auto v = d::xpkg_descriptor({ a }, "https://example.org/v0.1.1");
    if (auto&& failed = expect(fatal(bool(v))); !failed) failed << v.error();
    if (auto&& failed = expect(ne(v->find("deps = { \"xim:okrun\" }"), std::string::npos)); !failed) failed << *v;

    // A platform that states no hosts for its runner is read as every system
    // of each architecture, as before runs_on existed.
    d::compute_runs_on(a.pkg, {}, false);
    expect(eq(a.pkg.runsOnWith.at("okrun").size(), 6u));

    // The document carries it, and a reader takes it back.
    auto j = d::to_json(a.pkg);
    expect(ne(j.find("\"runs_on\""), std::string::npos)) << j;
}};

// The entry is named after the command a person runs, not the package, and
// moves the archive's own top-level directory into place.
UTest xpkg_descriptor_names_the_command_and_the_archive_top{"PackDistribution/XpkgDescriptorNamesTheCommandAndTheArchiveTop", [] {
    d::Asset uni;
    uni.file = "mcppls-0.0.18-openkal.zip";
    uni.sha256 = std::string(64, 'a');
    uni.top = "mcppls-0.0.18-openkal";
    uni.pkg.name = "mcpp-language-server";
    uni.pkg.version = "0.0.18";
    uni.pkg.runner = { "okrun", "{}" };
    uni.pkg.entry = { { "x86_64", "lib/x86_64/mcppls.oke" }, { "aarch64", "lib/aarch64/mcppls.oke" } };
    auto u = d::xpkg_descriptor({ uni }, "https://example.org/v0.0.18");
    if (auto&& failed = expect(fatal(bool(u))); !failed) failed << u.error();
    if (auto&& failed = expect(ne(u->find("name = \"mcppls\""), std::string::npos)); !failed) failed << *u;
    if (auto&& failed = expect(ne(u->find("xvm.add(\"mcppls\", opts)"), std::string::npos)); !failed) failed << *u;
    if (auto&& failed = expect(ne(u->find("opts.alias = \"mcppls.exe\""), std::string::npos)); !failed) failed << *u;
    if (auto&& failed = expect(ne(u->find("for _, ext in ipairs({ \".zip\" }) do"), std::string::npos)); !failed) failed << *u;
    if (auto&& failed = expect(ne(u->find("local top = pkginfo.install_file()"), std::string::npos)); !failed) failed << *u;
    if (auto&& failed = expect(ne(u->find("os.mv(top, pkginfo.install_dir())"), std::string::npos)); !failed) failed << *u;

    std::vector<d::Asset> per;
    for (auto [triple, top] : { std::pair{ "x86_64-linux-musl", "okrun-0.1.3-linux-x86_64" },
                                std::pair{ "aarch64-macos", "okrun-0.1.3-macos-aarch64" } }) {
        d::Asset a;
        a.file = std::string(top) + ".zip";
        a.sha256 = std::string(64, 'b');
        a.top = top;
        a.pkg.name = "openkal-tools";
        a.pkg.version = "0.1.3";
        a.pkg.legs = { triple };
        a.pkg.program = "bin/okrun";
        per.push_back(a);
    }
    auto r = d::xpkg_descriptor(per, "https://example.org/0.1.3");
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    if (auto&& failed = expect(ne(r->find("name = \"okrun\""), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(ne(r->find("for _, ext in ipairs({ \".zip\" }) do"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(ne(r->find("path.join(pkginfo.install_dir(), \"bin\")"), std::string::npos)); !failed) failed << *r;
}};

// The install takes the package's directory from the downloaded archive's
// name, which is what `mcpp pack` names it after; an archive renamed since
// is refused, and so is a file xim does not unpack.
UTest xpkg_descriptor_installs_the_directory_named_after_the_archive{"PackDistribution/XpkgDescriptorInstallsTheDirectoryNamedAfterTheArchive", [] {
    d::Asset a;
    a.file = "tool-1.0-linux-x86_64.tar.gz";
    a.top = "tool-1.0-linux-x86_64";
    a.sha256 = std::string(64, 'd');
    a.pkg.name = "tool";
    a.pkg.version = "1.0";
    a.pkg.legs = { "x86_64-linux-gnu" };
    a.pkg.program = "bin/tool";
    d::Asset w = a;
    w.file = "tool-1.0-windows-x86_64.zip";
    w.top = "tool-1.0-windows-x86_64";
    w.pkg.legs = { "x86_64-windows-gnu" };
    w.pkg.program = "tool.exe";
    auto r = d::xpkg_descriptor({ a, w }, "https://example.org/1.0");
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    if (auto&& failed = expect(ne(r->find("for _, ext in ipairs({ \".tar.gz\", \".zip\" }) do"), std::string::npos)); !failed) failed << *r;

    d::Asset renamed = a;
    renamed.file = "tool.tar.gz";
    auto refused = d::xpkg_descriptor({ renamed }, "https://example.org/1.0");
    expect(fatal(!refused));
    if (auto&& failed = expect(ne(refused.error().find("named after the archive"), std::string::npos)); !failed) failed << refused.error();

    d::Asset tgz = a;
    tgz.file = "tool-1.0-linux-x86_64.tgz";
    auto t = d::xpkg_descriptor({ tgz }, "https://example.org/1.0");
    if (auto&& failed = expect(fatal(bool(t))); !failed) failed << t.error();
    if (auto&& failed = expect(ne(t->find("for _, ext in ipairs({ \".tgz\" }) do"), std::string::npos)); !failed) failed << *t;
    expect(eq(d::archive_directory("app-1.0-openkal.tar.zst"), "app-1.0-openkal"));
    expect(eq(d::archive_directory("app-1.0-openkal"), "app-1.0-openkal"));

    d::Asset okb = a;
    okb.file = "tool-1.0-linux-x86_64.okb";
    auto notArchive = d::xpkg_descriptor({ okb }, "https://example.org/1.0");
    expect(fatal(!notArchive));
    if (auto&& failed = expect(ne(notArchive.error().find("not an archive xim unpacks"), std::string::npos)); !failed) failed << notArchive.error();
}};

// Packages of two systems that place their program apart -- a Windows one at
// the root, a POSIX one under bin/ -- give each system its own directory.
UTest xpkg_descriptor_names_each_systems_program_directory{"PackDistribution/XpkgDescriptorNamesEachSystemsProgramDirectory", [] {
    std::vector<d::Asset> per;
    for (auto [triple, top, program] :
         { std::tuple{ "x86_64-linux-musl", "okrun-0.1.3-linux-x86_64", "bin/okrun" },
           std::tuple{ "aarch64-macos", "okrun-0.1.3-macos-aarch64", "bin/okrun" },
           std::tuple{ "x86_64-windows-gnu", "okrun-0.1.3-windows-x86_64", "okrun.exe" } }) {
        d::Asset a;
        a.file = std::string(top) + ".zip";
        a.sha256 = std::string(64, 'c');
        a.top = top;
        a.pkg.name = "openkal-tools";
        a.pkg.version = "0.1.3";
        a.pkg.legs = { triple };
        a.pkg.program = program;
        per.push_back(a);
    }
    auto r = d::xpkg_descriptor(per, "https://example.org/0.1.3");
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    if (auto&& failed = expect(ne(r->find("local bindir = { linux = \"bin\", macosx = \"bin\", windows = \".\" }"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(ne(r->find("path.join(pkginfo.install_dir(), bindir[os.host()])"), std::string::npos)); !failed) failed << *r;
    if (auto&& failed = expect(ne(r->find("name = \"okrun\""), std::string::npos)); !failed) failed << *r;
}};

// An entry describes packages of one leg for the system their program runs on
// (SPEC-014 §8): a Linux, macOS or Windows program, or the carrier of an ok
// image, of the system and architecture the package records. A leg whose
// program is no program of a system -- a WebAssembly module, an Android
// application, a bare-metal image, an ok image without its carrier -- is
// refused, naming the package, the triple and the route that distributes it;
// it was written `linux` before.
UTest xpkg_descriptor_describes_programs_of_a_system{"PackDistribution/XpkgDescriptorDescribesProgramsOfASystem", [] {
    auto one = [](std::string triple, std::string top, std::string program) {
        d::Asset a;
        a.file = top + ".tar.gz";
        a.top = top;
        a.sha256 = std::string(64, 'e');
        a.pkg.name = "web";
        a.pkg.version = "0.1.0";
        a.pkg.legs = { triple };
        a.pkg.program = program;
        return a;
    };
    struct Row { const char* triple; const char* says; };
    for (auto [triple, says] : { Row{ "wasm32-emscripten", "dist-web" },
                                 Row{ "aarch64-linux-android", "Linux, macOS or Windows target" },
                                 Row{ "riscv64-none-elf", "Linux, macOS or Windows target" },
                                 Row{ "x86_64-openkal", "--target openkal" } }) {
        auto r = d::xpkg_descriptor({ one(triple, std::format("web-0.1.0-{}", triple), "bin/web.js") },
                                    "https://example.org/0.1.0");
        if (auto&& failed = expect(fatal(!r)); !failed) failed << triple << ": " << *r;
        if (auto&& failed = expect(ne(r.error().find(std::format("is a package of {}, which is no program of Linux, "
                                             "macOS or Windows", triple)), std::string::npos)); !failed) failed << r.error();
        if (auto&& failed = expect(ne(r.error().find(says), std::string::npos)); !failed) failed << r.error();
    }
    for (auto [triple, system] : { std::pair{ "x86_64-linux-gnu", "linux" },
                                   std::pair{ "aarch64-linux-musl", "linux" },
                                   std::pair{ "aarch64-macos", "macosx" },
                                   std::pair{ "x86_64-windows-msvc", "windows" } }) {
        auto r = d::xpkg_descriptor({ one(triple, std::format("tool-0.1.0-{}", triple), "bin/tool") },
                                    "https://example.org/0.1.0");
        if (auto&& failed = expect(fatal(bool(r))); !failed) failed << triple << ": " << r.error();
        if (auto&& failed = expect(ne(r->find(std::format("        {} = {{", system)), std::string::npos)); !failed) failed << *r;
    }
    // A one-leg ok image packed with its carrier on Linux: a Linux program of
    // the image's architecture, as `systems` records it.
    auto carried = one("x86_64-openkal", "app-0.1.0-x86_64-openkal-bundle-all", "bin/app");
    carried.pkg.mode = "self-contained";
    carried.pkg.systems = { "linux-x86_64" };
    auto c = d::xpkg_descriptor({ carried }, "https://example.org/0.1.0");
    if (auto&& failed = expect(fatal(bool(c))); !failed) failed << c.error();
    if (auto&& failed = expect(ne(c->find("archs = {\"x86_64\"}"), std::string::npos)); !failed) failed << *c;
    if (auto&& failed = expect(ne(c->find("        linux = {"), std::string::npos)); !failed) failed << *c;
    if (auto&& failed = expect(eq(c->find("macosx = {"), std::string::npos)); !failed) failed << *c;
    if (auto&& failed = expect(ne(c->find("app-0.1.0-${arch}-openkal-bundle-all.tar.gz"), std::string::npos)); !failed) failed << *c;
    if (auto&& failed = expect(ne(c->find("path.join(pkginfo.install_dir(), \"bin\")"), std::string::npos)); !failed) failed << *c;
    // ...and on a macOS machine of the other architecture, a macOS program.
    carried.pkg.systems = { "macos-aarch64" };
    carried.pkg.legs = { "aarch64-openkal" };
    carried.file = "app-0.1.0-aarch64-openkal-bundle-all.tar.gz";
    carried.top = "app-0.1.0-aarch64-openkal-bundle-all";
    auto m = d::xpkg_descriptor({ carried }, "https://example.org/0.1.0");
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << m.error();
    if (auto&& failed = expect(ne(m->find("archs = {\"aarch64\"}"), std::string::npos)); !failed) failed << *m;
    if (auto&& failed = expect(ne(m->find("        macosx = {"), std::string::npos)); !failed) failed << *m;
    if (auto&& failed = expect(eq(m->find("linux = {"), std::string::npos)); !failed) failed << *m;
}};
