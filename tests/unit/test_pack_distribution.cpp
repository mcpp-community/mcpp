// The distribution half of `mcpp pack` (SPEC-014): the codecs, the zip writer's
// deflated and aligned members, SHA-256, the launchers, the package metadata
// and the single file's payload.
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

std::uint32_t le32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at]))
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
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
    auto raw = c::deflate_raw(text, 0);
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
    const auto sh = d::posix_launcher(l);
    EXPECT_TRUE(sh.starts_with("#!/bin/sh\n"));
    EXPECT_NE(sh.find("x86_64|amd64|AMD64) arch=x86_64"), std::string::npos);
    EXPECT_NE(sh.find("image=\"$top/lib/aarch64/hello.oke\""), std::string::npos);
    EXPECT_NE(sh.find("runner=\"$top/libexec/okrun/$os-$arch/okrun\""), std::string::npos);
    EXPECT_NE(sh.find("exec \"$runner\" \"$image\" \"$@\""), std::string::npos);
    const auto cmd = d::windows_launcher(l);
    EXPECT_NE(cmd.find("PROCESSOR_ARCHITEW6432"), std::string::npos);
    EXPECT_NE(cmd.find("libexec\\okrun\\windows-%arch%\\okrun.exe"), std::string::npos);
    EXPECT_EQ(d::arch_dir("arm64"), "aarch64");
    EXPECT_EQ(d::arch_dir("AMD64"), "x86_64");
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
