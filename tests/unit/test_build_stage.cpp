#include <gtest/gtest.h>

import std;
import mcpp.build.stage;

using namespace mcpp::build::stage;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_stage_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& p, std::string_view body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::binary);
    os << body;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(is), {}};
}

// Retries would only slow the failure tests down; the behaviour under test is
// the decision, not the backoff.
StageOptions no_retry(Verify v = Verify::Content) {
    return StageOptions{.verify = v, .retries = 0, .backoff = std::chrono::milliseconds{0}};
}

}  // namespace

TEST(BuildStage, CopiesWhenDestinationMissing) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "nested" / "dir" / "dst.bin";
    write_file(src, "payload");

    auto r = stage_file(src, dst, no_retry());
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "payload");
}

// The core of #311: an already-equivalent destination must not be written at
// all — that is what keeps a clangd-mapped std.pcm from failing the build.
TEST(BuildStage, EquivalentDestinationIsNotTouched) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "same-bytes");
    write_file(dst, "same-bytes");

    // Age the destination so any write (or timestamp bump) is observable.
    auto before = std::filesystem::file_time_type::clock::now() - std::chrono::hours{2};
    std::filesystem::last_write_time(dst, before);
    auto recorded = std::filesystem::last_write_time(dst);

    auto r = stage_file(src, dst, no_retry());
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->copied);
    // Not "not bumped to now" — not changed at all. Any mtime change defeats
    // ninja's `restat = 1` and re-triggers the downstream rebuild cascade.
    EXPECT_EQ(std::filesystem::last_write_time(dst), recorded);
}

// Same size, different bytes must be copied by DEFAULT. Staging also carries
// .dll payloads, where PE section padding makes equal sizes across a real
// rebuild ordinary — so size-only equivalence cannot be the default.
TEST(BuildStage, SameSizeDifferentBytesIsCopiedByDefault) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");

    auto r = stage_file(src, dst, no_retry());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "AAAA");
}

// `--verify size` is the opt-in shortcut for callers that know the source is
// fingerprint-scoped; it accepts the same-size destination as already staged.
TEST(BuildStage, SameSizeDifferentBytesIsSkippedUnderSizeVerify) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");

    auto r = stage_file(src, dst, no_retry(Verify::Size));
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->copied);
    EXPECT_EQ(read_file(dst), "BBBB");
}

TEST(BuildStage, DifferentSizeIsAlwaysCopied) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "longer-payload");
    write_file(dst, "short");

    auto r = stage_file(src, dst, no_retry());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "longer-payload");
}

TEST(BuildStage, MissingSourceIsAnError) {
    Tmp tmp;
    auto r = stage_file(tmp.path / "nope.bin", tmp.path / "dst.bin", no_retry());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("nope.bin"), std::string::npos);
}

// A read-only destination that must actually be replaced: POSIX can do it
// (the directory entry is what gets rewritten, so temp-file + rename wins),
// Windows cannot (replacing a read-only file is denied however you spell it) —
// and there the contract is the loud failure, not a silent skip. Asserted per
// platform rather than skipped, so neither side can rot unnoticed.
TEST(BuildStage, ReadOnlyDestinationOutcomeIsPlatformDefined) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "new-content");
    write_file(dst, "old");
    std::filesystem::permissions(dst, std::filesystem::perms::owner_read);

    auto r = stage_file(src, dst, no_retry());
#if defined(_WIN32)
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("hint:"), std::string::npos);
    EXPECT_EQ(read_file(dst), "old");
#else
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "new-content");
#endif
}

// When staging genuinely cannot proceed, the failure must name the file and
// carry the actionable hint — never be downgraded to a warning.
TEST(BuildStage, UnwritableDestinationFailsWithActionableHint) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    write_file(src, "payload");
    // A directory in the destination's place: neither rename nor copy can win.
    auto dst = tmp.path / "occupied";
    std::filesystem::create_directories(dst / "child");
    write_file(dst / "child" / "keep.txt", "x");

    auto r = stage_file(src, dst, no_retry());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("hint:"), std::string::npos);
    EXPECT_NE(r.error().message.find("clangd"), std::string::npos);
    EXPECT_NE(r.error().message.find(dst.string()), std::string::npos);
}

TEST(BuildStage, SameContentComparesBytesNotJustSize) {
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto c = tmp.path / "c.bin";
    write_file(a, std::string(300000, 'x'));
    write_file(b, std::string(300000, 'x'));
    write_file(c, std::string(300000, 'x').replace(299999, 1, "y"));

    EXPECT_TRUE(same_content(a, b));
    EXPECT_FALSE(same_content(a, c));
    EXPECT_FALSE(same_content(a, tmp.path / "missing.bin"));
}

TEST(BuildStage, VerifyModeParsing) {
    EXPECT_EQ(parse_verify("content"), Verify::Content);
    EXPECT_EQ(parse_verify("size"), Verify::Size);
    // Anything unrecognized must land on the SAFE mode, not the fast one.
    EXPECT_EQ(parse_verify("nonsense"), Verify::Content);
    EXPECT_EQ(parse_verify(""), Verify::Content);
}
