// The distribution half of `mcpp pack` (SPEC-014): the codecs, the zip writer's
// deflated and aligned members, SHA-256, the tar mcpp writes, what `unpack`
// refuses to create, the launchers, the package metadata, `--merge` and the
// single file's payload.
#include <gtest/gtest.h>

import std;
import mcpp.pack.compress;
import mcpp.pack.distribution;
import mcpp.pack.zip;

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
    EXPECT_TRUE(mcpp::pack::zip::write(out, entries));
    return out;
}

}  // namespace

TEST(PackCompress, ParsesAlgorithmsAndLevels) {
    auto z = c::parse("zstd:19");
    ASSERT_TRUE(z);
    EXPECT_EQ(z->algorithm, c::Algorithm::Zstd);
    EXPECT_EQ(z->level, 19);
    EXPECT_TRUE(z->stated);
    EXPECT_TRUE(c::parse("none"));
    EXPECT_FALSE(c::parse("none:3"));
    EXPECT_FALSE(c::parse("gzip:10"));
    EXPECT_FALSE(c::parse("brotli"));
    EXPECT_EQ(c::tar_extension(c::Algorithm::Xz), ".tar.xz");
    EXPECT_EQ(c::tar_extension(c::Algorithm::None), ".tar");
}

// An unstated level is -1, the codec's default; every stated one is honoured,
// `xz:0` and `zstd:1` too.
TEST(PackCompress, AStatedLevelIsHonoured) {
    EXPECT_EQ(c::Choice{}.level, -1);
    EXPECT_EQ(c::parse("xz")->level, -1);
    EXPECT_EQ(c::parse("xz:0")->level, 0);
    EXPECT_EQ(c::parse("zstd:1")->level, 1);
    std::string text;
    for (int i = 0; i < 4000; ++i) text += std::format("{} ", i * 7919 % 10007);
    auto fastest = c::bytes(text, *c::parse("xz:0"));
    auto preset = c::bytes(text, *c::parse("xz"));
    ASSERT_TRUE(fastest && preset);
    EXPECT_NE(*fastest, *preset) << "xz:0 was written at the default preset";
    auto z1 = c::bytes(text, *c::parse("zstd:1"));
    auto z19 = c::bytes(text, *c::parse("zstd"));
    ASSERT_TRUE(z1 && z19);
    EXPECT_NE(*z1, *z19) << "zstd:1 was written at the default level";
}

// A gzip member names no file, no time and the system 255, whatever host
// wrote it.
TEST(PackCompress, AGzipHeaderIsTheSameOnEveryHost) {
    auto gz = c::bytes("the same bytes", c::Choice{});
    ASSERT_TRUE(gz);
    ASSERT_GE(gz->size(), 10u);
    EXPECT_EQ(static_cast<unsigned char>((*gz)[0]), 0x1f);
    EXPECT_EQ(static_cast<unsigned char>((*gz)[3]), 0) << "a name, a comment or an extra field";
    EXPECT_EQ(le32(*gz, 4), 0u) << "a modification time";
    EXPECT_EQ(static_cast<unsigned char>((*gz)[9]), 255) << "the system byte";
}

TEST(PackCompress, EveryCodecRoundTrips) {
    std::string text;
    for (int i = 0; i < 4000; ++i) text += std::format("line {} of a package\n", i % 37);
    for (auto a : { c::Algorithm::Gzip, c::Algorithm::Zstd, c::Algorithm::Xz, c::Algorithm::None }) {
        c::Choice ch;
        ch.algorithm = a;
        auto packed = c::bytes(text, ch);
        ASSERT_TRUE(packed) << c::name(a);
        if (a != c::Algorithm::None) EXPECT_LT(packed->size(), text.size()) << c::name(a);
        auto back = c::decompress(*packed, a);
        ASSERT_TRUE(back) << c::name(a);
        EXPECT_EQ(*back, text) << c::name(a);
    }
    auto raw = c::deflate_raw(text, -1);
    ASSERT_TRUE(raw);
    auto back = c::inflate_raw(*raw, text.size());
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, text);
}

TEST(PackDistribution, Sha256OfKnownVectors) {
    EXPECT_EQ(d::sha256(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(d::sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(d::sha256(std::string(1000, 'a')),
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST(PackZip, AnAlignedMemberIsStoredOnItsBoundary) {
    Tmp t;
    write(t.path / "a.oke", std::string(3000, 'x'));
    write(t.path / "libb.okso", std::string(70000, 'y'));
    std::vector<mcpp::pack::zip::Entry> entries{
        { "a.oke", t.path / "a.oke", false, {} },
        { "libb.okso", t.path / "libb.okso", false, {} },
    };
    mcpp::pack::zip::WriteOptions o;
    o.align = 65536;
    ASSERT_TRUE(mcpp::pack::zip::write(t.path / "out.zip", entries, o));
    std::ifstream in(t.path / "out.zip", std::ios::binary);
    std::string b{std::istreambuf_iterator<char>(in), {}};
    // Each member's data, found through the central directory, starts on a
    // 64 KiB boundary and is stored.
    std::size_t eocd = b.rfind(std::string("PK\x05\x06", 4));
    ASSERT_NE(eocd, std::string::npos);
    std::size_t at = le32(b, eocd + 16);
    for (int n = 0; n < 2; ++n) {
        ASSERT_EQ(le32(b, at), 0x02014b50u);
        const auto method = static_cast<unsigned char>(b[at + 10]);
        const auto nameLen = static_cast<std::size_t>(static_cast<unsigned char>(b[at + 28]))
                           | static_cast<std::size_t>(static_cast<unsigned char>(b[at + 29])) << 8;
        const auto local = le32(b, at + 42);
        const auto lname = static_cast<std::size_t>(static_cast<unsigned char>(b[local + 26]))
                         | static_cast<std::size_t>(static_cast<unsigned char>(b[local + 27])) << 8;
        const auto lextra = static_cast<std::size_t>(static_cast<unsigned char>(b[local + 28]))
                          | static_cast<std::size_t>(static_cast<unsigned char>(b[local + 29])) << 8;
        EXPECT_EQ(method, 0);
        EXPECT_EQ((local + 30 + lname + lextra) % 65536, 0u);
        at += 46 + nameLen;
    }
}

TEST(PackZip, DeflateWhenItShrinks) {
    Tmp t;
    write(t.path / "text", std::string(10000, 'z'));
    mcpp::pack::zip::WriteOptions o;
    o.deflate = true;
    ASSERT_TRUE(mcpp::pack::zip::write(t.path / "out.zip", { { "text", t.path / "text", false, {} } }, o));
    EXPECT_LT(fs::file_size(t.path / "out.zip"), 1000u);
    // And it reads back.
    auto root = d::unpack(t.path / "out.zip", t.path / "x");
    ASSERT_FALSE(root) << "a zip without one top-level directory is not a package";
}

TEST(PackDistribution, LaunchersPickTheArchitectureAndTheRunner) {
    d::Launch l;
    l.name = "hello";
    l.runner = { "okrun", "{}" };
    l.entry = { { "x86_64", "hello.oke" }, { "aarch64", "hello.oke" } };
    l.loader = "okrun";
    ASSERT_TRUE(d::launchable(l));
    const auto sh = d::posix_launcher(l);
    EXPECT_TRUE(sh.starts_with("#!/bin/sh\n"));
    EXPECT_NE(sh.find("x86_64|amd64|AMD64) arch=x86_64"), std::string::npos);
    EXPECT_NE(sh.find("'aarch64') entry='hello.oke' ;;"), std::string::npos);
    EXPECT_NE(sh.find("image=\"$top/lib/$arch/$entry\""), std::string::npos);
    EXPECT_NE(sh.find("runner=\"$top/libexec/$loader/$os-$arch/$loader\""), std::string::npos);
    EXPECT_NE(sh.find("exec \"$runner\" \"$image\" \"$@\""), std::string::npos);
    // The package is found through a link to the launcher, without readlink -f.
    EXPECT_NE(sh.find("readlink -- \"$self\""), std::string::npos);
    EXPECT_EQ(sh.find("readlink -f"), std::string::npos);
    const auto cmd = d::windows_launcher(l);
    EXPECT_NE(cmd.find("PROCESSOR_ARCHITEW6432"), std::string::npos);
    EXPECT_NE(cmd.find("libexec\\okrun\\windows-%arch%\\okrun.exe"), std::string::npos);
    EXPECT_NE(cmd.find("\"%runner%\" \"%image%\" %*"), std::string::npos);
}

// The runner's words expand as `mcpp run` expands them: a word's `{}` is the
// image inside the word as well as alone; with no `{}` the image is appended.
TEST(PackDistribution, LaunchersExpandTheRunnerAsMcppRunDoes) {
    d::Launch l;
    l.name = "hello";
    l.entry = { { "x86_64", "hello.oke" } };
    l.runner = { "okrun", "--image={}", "it's" };
    EXPECT_NE(d::posix_launcher(l).find("exec \"$runner\" '--image='\"$image\" 'it'\\''s' \"$@\""),
              std::string::npos) << d::posix_launcher(l);
    EXPECT_NE(d::windows_launcher(l).find("\"%runner%\" \"--image=%image%\" \"it's\" %*"), std::string::npos)
        << d::windows_launcher(l);
    l.runner = { "okrun", "--quiet" };
    EXPECT_NE(d::posix_launcher(l).find("exec \"$runner\" '--quiet' \"$image\" \"$@\""), std::string::npos)
        << d::posix_launcher(l);
    EXPECT_NE(d::windows_launcher(l).find("\"%runner%\" \"--quiet\" \"%image%\" %*"), std::string::npos)
        << d::windows_launcher(l);
}

// A .cmd doubles `%` in what it states literally, and a word it cannot state
// (a double quote) is refused: the .cmd written for it runs nothing.
TEST(PackDistribution, AWindowsLauncherEscapesWhatItStates) {
    d::Launch l;
    l.name = "hello";
    l.entry = { { "x86_64", "hello.oke" } };
    l.runner = { "ok%run", "--load=50%", "{}" };
    const auto cmd = d::windows_launcher(l);
    EXPECT_NE(cmd.find("set \"runner=ok%%run\""), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("\"%runner%\" \"--load=50%%\" \"%image%\" %*"), std::string::npos) << cmd;
    l.runner = { "okrun", "--say=\"hi\"", "{}" };
    auto ok = d::launchable(l);
    ASSERT_FALSE(ok);
    EXPECT_NE(ok.error().find("double quote"), std::string::npos) << ok.error();
    const auto refused = d::windows_launcher(l);
    EXPECT_EQ(refused.find("%runner%"), std::string::npos) << refused;
    EXPECT_NE(refused.find("exit /b 126"), std::string::npos) << refused;
}

// The POSIX launcher, run through a link to it from another directory, finds
// its package and starts the runner with the image spliced into its word.
TEST(PackDistribution, APosixLauncherRunsThroughALink) {
    if (!fs::exists("/bin/sh")) GTEST_SKIP() << "no /bin/sh";
    std::string arch;
    {
        Tmp probe;
        if (std::system(std::format("uname -m > '{}'", (probe.path / "m").string()).c_str()) != 0)
            GTEST_SKIP() << "no uname";
        arch = slurp(probe.path / "m");
    }
    arch = arch.starts_with("x86_64") || arch.starts_with("amd64") ? "x86_64"
         : arch.starts_with("aarch64") || arch.starts_with("arm64") ? "aarch64" : "";
    if (arch.empty()) GTEST_SKIP() << "no launcher row for this machine";
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
    EXPECT_EQ(rc, 0) << slurp(out);
    // The physical path: the launcher follows links to the directories too
    // (macOS's temporary directory is under a link, /var -> /private/var).
    EXPECT_EQ(slurp(out), std::format("--image={}|a b|c|",
                                      (fs::canonical(pkg) / "lib" / arch / "hello.oke").string()));
}

// A shared object is `.so` or `.so.<version>`; a name that merely holds `.so`
// is not one.
TEST(PackDistribution, ALibraryIsNamedSoOrSoVersion) {
    Tmp t;
    const auto root = t.path / "app-1.0";
    for (auto f : { "lib/libz.so", "lib/libz.so.1.3", "share/notes.sol", "share/a.so.x", "share/b.socket" })
        write(root / f, "x");
    d::Package p;
    p.name = "app";
    p.version = "1.0";
    auto described = d::describe(root, p, {});
    ASSERT_TRUE(described) << described.error();
    std::map<std::string, std::string> roles;
    for (auto const& f : described->files) roles[f.path] = f.role;
    EXPECT_EQ(roles["lib/libz.so"], "library");
    EXPECT_EQ(roles["lib/libz.so.1.3"], "library");
    EXPECT_EQ(roles["share/notes.sol"], "resource");
    EXPECT_EQ(roles["share/a.so.x"], "resource");
    EXPECT_EQ(roles["share/b.socket"], "resource");
}

// `read` refuses a document that names a path outside the package.
TEST(PackDistribution, ReadRefusesAPathOutsideThePackage) {
    Tmp t;
    for (auto bad : { "../outside", "/etc/passwd", "a/../../b", "./bin/x", "bin\\\\x", "" }) {
        write(t.path / "mcpp-package.json",
              std::format(R"({{"schema": 1, "name": "x", "version": "1", "files": [{{"path": "{}", "role": "resource", "sha256": ""}}]}})",
                          bad));
        auto r = d::read(t.path);
        ASSERT_FALSE(r) << bad;
        EXPECT_NE(r.error().find("not a path inside the package"), std::string::npos) << r.error();
    }
}

TEST(PackDistribution, PackageMetadataDescribesEveryFile) {
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
    ASSERT_TRUE(described);
    auto back = d::read(root);
    ASSERT_TRUE(back) << back.error();
    std::map<std::string, std::string> roles;
    for (auto const& f : back->files) roles[f.path] = f.role;
    EXPECT_EQ(roles["bin/app"], "launcher");
    EXPECT_EQ(roles["lib/x86_64/app.oke"], "image");
    EXPECT_EQ(roles["share/data.txt"], "resource");
    EXPECT_EQ(roles["LICENSE"], "metadata");
    EXPECT_EQ(back->entry.at("x86_64"), "lib/x86_64/app.oke");
    for (auto const& f : back->files)
        EXPECT_EQ(f.sha256, d::sha256_file(root / f.path)) << f.path;
}

TEST(PackDistribution, TreePayloadIsOrderedRecords) {
    Tmp t;
    write(t.path / "b/x", "12");
    write(t.path / "a", "3");
    auto payload = d::tree_payload(t.path);
    ASSERT_TRUE(payload);
    EXPECT_TRUE(payload->starts_with("MCPPTREE1\n"));
    // `a` before `b` before `b/x`.
    const auto a = payload->find("a"), b = payload->find("b/x");
    EXPECT_LT(a, b);
}

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
TEST(PackTar, TheSameTreeIsTheSameBytes) {
    Tmp t;
    const auto root = sample_tree(t.path);
    for (auto ch : { kPlain, c::Choice{} }) {
        ASSERT_TRUE(d::write_tar(root, t.path / "one", ch));
        fs::last_write_time(root / "share/data.txt", fs::file_time_type::clock::now() - std::chrono::hours(48));
        fs::permissions(root / "share/data.txt", fs::perms::group_write, fs::perm_options::add);
        ASSERT_TRUE(d::write_tar(root, t.path / "two", ch));
        EXPECT_EQ(slurp(t.path / "one"), slurp(t.path / "two")) << c::name(ch.algorithm);
    }
    ASSERT_TRUE(d::write_tar(root, t.path / "plain.tar", kPlain));
    const auto tar = slurp(t.path / "plain.tar");
    ASSERT_GE(tar.size(), 1024u);
    EXPECT_EQ(tar.substr(0, 9), std::string("app-1.0/\0", 9));
    EXPECT_EQ(tar.substr(108, 8), std::string("0000000\0", 8)) << "uid";
    EXPECT_EQ(tar.substr(116, 8), std::string("0000000\0", 8)) << "gid";
    EXPECT_EQ(tar.substr(136, 12), std::string("00000000000\0", 12)) << "mtime";
    EXPECT_EQ(tar.substr(257, 6), std::string("ustar\0", 6));
    EXPECT_EQ(tar.substr(265, 32), std::string(32, '\0')) << "uname";
    EXPECT_EQ(tar.size() % 512, 0u);
}

// What mcpp writes, `unpack` reads back: files, modes, links, long names.
TEST(PackTar, UnpackReadsWhatWriteTarWrote) {
    Tmp t;
    const auto root = sample_tree(t.path);
    for (auto ch : { kPlain, c::Choice{}, c::Choice{ c::Algorithm::Zstd, -1, false },
                     c::Choice{ c::Algorithm::Xz, -1, false } }) {
        const auto archive = t.path / ("app" + std::string(c::tar_extension(ch.algorithm)));
        ASSERT_TRUE(d::write_tar(root, archive, ch));
        const auto into = t.path / ("x-" + std::string(c::name(ch.algorithm)));
        auto back = d::unpack(archive, into);
        ASSERT_TRUE(back) << back.error();
        EXPECT_EQ(back->filename(), "app-1.0");
        EXPECT_EQ(slurp(*back / "share/data.txt"), "data");
        EXPECT_EQ(slurp(*back / "share" / std::string(60, 'd') / (std::string(60, 'e') + ".txt")), "deep");
        EXPECT_NE(fs::status(*back / "bin/app").permissions() & fs::perms::owner_exec, fs::perms::none);
        EXPECT_TRUE(fs::is_directory(*back / "empty"));
        EXPECT_TRUE(fs::is_symlink(*back / "share/link"));
        EXPECT_EQ(fs::read_symlink(*back / "share/far").generic_string(),
                  std::string(60, 'd') + "/" + std::string(60, 'e') + ".txt");
        EXPECT_EQ(slurp(*back / "share/far"), "deep");
    }
}

// A tar holding a link out of its tree is refused, and nothing is written.
TEST(PackTar, UnpackRefusesALinkOutOfTheTree) {
    Tmp t;
    const auto root = t.path / "app-1.0";
    write(root / "a", "a");
    fs::create_symlink("../../outside", root / "escape");
    ASSERT_TRUE(d::write_tar(root, t.path / "app.tar", kPlain));
    auto r = d::unpack(t.path / "app.tar", t.path / "x");
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("outside the package"), std::string::npos) << r.error();
    EXPECT_TRUE(fs::is_empty(t.path / "x"));
}

// ─── what unpack refuses ──────────────────────────────────────────────────

// A member that would land outside the extraction root, or be written through
// a link member, is refused before anything is written.
TEST(PackUnpack, AZipCreatesNothingOutsideItsRoot) {
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
        ASSERT_FALSE(r) << cases[i].label;
        EXPECT_NE(r.error().find(cases[i].why), std::string::npos) << cases[i].label << ": " << r.error();
        EXPECT_TRUE(fs::is_empty(into)) << cases[i].label << ": something was written";
    }
    EXPECT_FALSE(fs::exists(t.path / "evil"));
    // A link inside the tree is made, after the files.
    auto ok = d::unpack(zip_of(t.path, "ok.zip", { { "pkg/l", {}, false, "sub" }, { "pkg/sub/x", f, false, {} } }),
                        t.path / "ok");
    ASSERT_TRUE(ok) << ok.error();
    EXPECT_TRUE(fs::is_symlink(*ok / "l"));
    EXPECT_EQ(slurp(*ok / "l/x"), "content");
}

// Every truncation of a zip, and every byte of it changed, is an error or a
// tree, never a read past the archive.
TEST(PackUnpack, ATruncatedOrCorruptZipIsAnError) {
    Tmp t;
    write(t.path / "f", std::string(300, 'q'));
    write(t.path / "g", "small");
    mcpp::pack::zip::WriteOptions o;
    o.deflate = true;
    ASSERT_TRUE(mcpp::pack::zip::write(t.path / "a.zip", { { "pkg/f", t.path / "f", false, {} },
                                                           { "pkg/g", t.path / "g", true, {} } }, o));
    const auto whole = slurp(t.path / "a.zip");
    for (std::size_t n = 0; n < whole.size(); ++n) {
        write(t.path / "cut.zip", whole.substr(0, n));
        const auto into = t.path / std::format("cut{}", n);
        EXPECT_FALSE(d::unpack(t.path / "cut.zip", into)) << n << " bytes";
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
}

// A member whose bytes do not match its CRC-32 is refused.
TEST(PackUnpack, ACrcMismatchIsRefused) {
    Tmp t;
    write(t.path / "f", "the member's content");
    auto zip = slurp(zip_of(t.path, "a.zip", { { "pkg/f", t.path / "f", false, {} } }));
    const auto at = zip.find("the member's content");
    ASSERT_NE(at, std::string::npos);
    zip[at] = 'T';
    write(t.path / "a.zip", zip);
    auto r = d::unpack(t.path / "a.zip", t.path / "x");
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("CRC-32"), std::string::npos) << r.error();
}

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
    EXPECT_TRUE(d::describe(root, p, {}));
    return root;
}

}  // namespace

// A file changed after its package was written is refused, named with both
// digests.
TEST(PackMerge, AFileThatIsNotItsDigestIsRefused) {
    Tmp t;
    const auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    const auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    write(b / "lib/aarch64/app.oke", "changed");
    d::MergeRequest req;
    req.inputs = { a, b };
    req.output = t.path / "out";
    auto r = d::merge(req);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("lib/aarch64/app.oke"), std::string::npos) << r.error();
    EXPECT_NE(r.error().find(d::sha256("changed")), std::string::npos) << r.error();
    EXPECT_NE(r.error().find(d::sha256("image of aarch64")), std::string::npos) << r.error();
    EXPECT_FALSE(fs::exists(t.path / "out"));
}

// The legs merge into a new directory; an output that exists, or is an input,
// is refused, and an input written with a trailing separator is its directory.
TEST(PackMerge, TheOutputIsNewOrAPackage) {
    Tmp t;
    const auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    const auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    d::MergeRequest req;
    req.inputs = { fs::path(a.string() + "/"), b };
    req.output = t.path / "out";
    auto r = d::merge(req);
    ASSERT_TRUE(r) << r.error();
    auto merged = d::read(*r);
    ASSERT_TRUE(merged) << merged.error();
    EXPECT_EQ(merged->legs, (std::vector<std::string>{ "aarch64-openkal", "x86_64-openkal" }));
    EXPECT_EQ(slurp(*r / "lib/aarch64/app.oke"), "image of aarch64");

    // Again into the directory it wrote: a directory is never replaced.
    auto again = d::merge(req);
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().find("exists"), std::string::npos) << again.error();
    // Onto an input.
    req.output = a;
    ASSERT_FALSE(d::merge(req));
    EXPECT_TRUE(fs::exists(a / "mcpp-package.json"));
    // Over a file that is not a package.
    write(t.path / "notes.tar.gz", "my notes");
    req.output = t.path / "notes.tar.gz";
    auto over = d::merge(req);
    ASSERT_FALSE(over);
    EXPECT_NE(over.error().find("not a package"), std::string::npos) << over.error();
    EXPECT_EQ(slurp(t.path / "notes.tar.gz"), "my notes");
    // Over an archive that is one: replaced.
    req.output = t.path / "app.tar.gz";
    ASSERT_TRUE(d::merge(req));
    auto replaced = d::merge(req);
    ASSERT_TRUE(replaced) << replaced.error();
}

// What the metadata states reaches the generated Lua as strings: a quote or a
// line break in a name is escaped, never code.
TEST(PackDistribution, XpkgDescriptorWritesMetadataAsStrings) {
    d::Asset a;
    a.file = "evil-1.0-openkal.zip";
    a.sha256 = std::string(64, 'a');
    a.pkg.name = "ev\"il\nos.exit()";
    a.pkg.version = "1.0";
    a.pkg.runner = { "okrun", "{}" };
    a.pkg.entry = { { "x86_64", "lib/x86_64/evil.oke" } };
    auto text = d::xpkg_descriptor({ a }, "https://example.org/v1.0");
    ASSERT_TRUE(text) << text.error();
    EXPECT_NE(text->find("description = \"ev\\\"il\\010os.exit() 1.0"), std::string::npos) << *text;
    EXPECT_EQ(text->find("\nos.exit()"), std::string::npos) << *text;
    EXPECT_NE(text->find("deps = { \"xim:okrun\" }"), std::string::npos) << *text;
}

// The compression fits the container the merge writes and the name it is
// given: zstd in a .tar.gz, or in a zip the first input decided, is refused.
TEST(PackMerge, TheCompressionFitsTheContainer) {
    Tmp t;
    const auto a = leg_package(t.path / "a", "x86_64-openkal", "x86_64");
    const auto b = leg_package(t.path / "b", "aarch64-openkal", "aarch64");
    d::MergeRequest req;
    req.inputs = { a, b };
    req.compression = *c::parse("zstd");
    req.output = t.path / "out.tar.gz";
    auto named = d::merge(req);
    ASSERT_FALSE(named);
    EXPECT_NE(named.error().find(".tar.zst"), std::string::npos) << named.error();
    req.output = t.path / "out.zip";
    ASSERT_FALSE(d::merge(req));
    req.output = t.path / "out.tar.zst";
    auto fits = d::merge(req);
    ASSERT_TRUE(fits) << fits.error();
}

// The entry is named after the command a person runs, not the package, and
// moves the archive's own top-level directory into place.
TEST(PackDistribution, XpkgDescriptorNamesTheCommandAndTheArchiveTop) {
    d::Asset uni;
    uni.file = "mcppls-0.0.18-openkal.zip";
    uni.sha256 = std::string(64, 'a');
    uni.top = "mcppls-0.0.18-openkal";
    uni.pkg.name = "mcpp-language-server";
    uni.pkg.version = "0.0.18";
    uni.pkg.runner = { "okrun", "{}" };
    uni.pkg.entry = { { "x86_64", "lib/x86_64/mcppls.oke" }, { "aarch64", "lib/aarch64/mcppls.oke" } };
    auto u = d::xpkg_descriptor({ uni }, "https://example.org/v0.0.18");
    ASSERT_TRUE(u) << u.error();
    EXPECT_NE(u->find("name = \"mcppls\""), std::string::npos) << *u;
    EXPECT_NE(u->find("xvm.add(\"mcppls\", opts)"), std::string::npos) << *u;
    EXPECT_NE(u->find("opts.alias = \"mcppls.cmd\""), std::string::npos) << *u;
    EXPECT_NE(u->find("os.dirs(\"mcppls-0.0.18-openkal\")"), std::string::npos) << *u;

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
    ASSERT_TRUE(r) << r.error();
    EXPECT_NE(r->find("name = \"okrun\""), std::string::npos) << *r;
    EXPECT_NE(r->find("os.dirs(\"okrun-0.1.3-*\")"), std::string::npos) << *r;
    EXPECT_NE(r->find("path.join(pkginfo.install_dir(), \"bin\")"), std::string::npos) << *r;
}

// Packages of two systems that place their program apart -- a Windows one at
// the root, a POSIX one under bin/ -- give each system its own directory.
TEST(PackDistribution, XpkgDescriptorNamesEachSystemsProgramDirectory) {
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
    ASSERT_TRUE(r) << r.error();
    EXPECT_NE(r->find("local bindir = { linux = \"bin\", macosx = \"bin\", windows = \".\" }"),
              std::string::npos) << *r;
    EXPECT_NE(r->find("path.join(pkginfo.install_dir(), bindir[os.host()])"), std::string::npos) << *r;
    EXPECT_NE(r->find("name = \"okrun\""), std::string::npos) << *r;
}
