#include <gtest/gtest.h>

import std;
import mcpp.source_kind;
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

// `--verify size` does not make a readable destination the source's: a
// package's BMI is staged at a path named by the module, so after an upgrade
// the previous version's BMI is there, often at the same size.
TEST(BuildStage, SameSizeDifferentBytesIsCopiedUnderSizeVerify) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");

    auto r = stage_file(src, dst, no_retry(Verify::Size));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "AAAA");
}

// A destination that cannot be opened is taken as staged under `--verify size`
// when its size is the source's: a holder that denies reading it denies
// replacing it as well.
TEST(BuildStage, AnUnreadableSameSizeDestinationIsSkippedUnderSizeVerify) {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");
    std::filesystem::permissions(dst, std::filesystem::perms::none);
    if (std::ifstream(dst, std::ios::binary).is_open()) {
        std::filesystem::permissions(dst, std::filesystem::perms::owner_all);
        GTEST_SKIP() << "permissions do not deny reading here";
    }

    auto r = stage_file(src, dst, no_retry(Verify::Size));
    auto content = r.has_value() ? std::string() : r.error().message;
    std::filesystem::permissions(dst, std::filesystem::perms::owner_all);
    ASSERT_TRUE(r.has_value()) << content;
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

// Workspace design 2026-09-29 §5.3: a shared library placed in two product
// directories holds one copy of its bytes where the file system allows a link;
// any other file is copied, so a program that writes a file beside itself
// never writes through to its source.
TEST(BuildStage, ASharedLibraryIsPlacedByALinkAndADataFileByACopy) {
    Tmp tmp;
    auto lib  = tmp.path / "store" / "libfoo.so.1";
    auto data = tmp.path / "store" / "table.dat";
    write_file(lib, "library");
    write_file(data, "data");
    for (auto const& dir : {"cli", "gui"}) {
        ASSERT_TRUE(stage_file(lib, tmp.path / "bin" / dir / "libfoo.so.1", no_retry()));
        ASSERT_TRUE(stage_file(data, tmp.path / "bin" / dir / "table.dat", no_retry()));
    }
    std::error_code ec;
    const auto libLinks = std::filesystem::hard_link_count(lib, ec);
    ASSERT_FALSE(ec);
    // Three names on a file system with links (the source and two
    // placements), one where the link fell back to a copy.
    EXPECT_TRUE(libLinks == 3 || libLinks == 1) << libLinks;
    if (libLinks == 3)
        EXPECT_TRUE(std::filesystem::equivalent(tmp.path / "bin" / "cli" / "libfoo.so.1",
                                                tmp.path / "bin" / "gui" / "libfoo.so.1"));
    EXPECT_EQ(std::filesystem::hard_link_count(data), 1u);
    EXPECT_EQ(read_file(tmp.path / "bin" / "gui" / "table.dat"), "data");
}

// A linked placement is replaced out of place: a new source never rewrites the
// bytes the earlier source shares with the placement.
TEST(BuildStage, ReplacingALinkedPlacementLeavesTheEarlierSourceIntact) {
    Tmp tmp;
    auto first  = tmp.path / "v1" / "foo.dll";
    auto second = tmp.path / "v2" / "foo.dll";
    auto dst    = tmp.path / "bin" / "app" / "foo.dll";
    write_file(first, "version one");
    write_file(second, "version two!");
    ASSERT_TRUE(stage_file(first, dst, no_retry()));
    ASSERT_TRUE(stage_file(second, dst, no_retry()));
    EXPECT_EQ(read_file(dst), "version two!");
    EXPECT_EQ(read_file(first), "version one");
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
    EXPECT_NE(r.error().message.find("help:"), std::string::npos);
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
    EXPECT_NE(r.error().message.find("help:"), std::string::npos);
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

// ── SPEC-007 R4.2 (mcpp#723): a destination with more than one source ──────

TEST(BuildStageFiles, OneSourceIsIdenticalToStageFile) {
    // The whole point of `stage_files`: a project with exactly one source per
    // destination -- every project before this feature -- must not notice a
    // difference. This is not asserted by string-matching a ninja line (that
    // is `test_ninja_backend.cpp`'s job); here it is the outcome that must
    // match `stage_file`'s exactly.
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "payload");

    auto r = stage_files({src}, dst, no_retry());
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "payload");
}

TEST(BuildStageFiles, AgreeingSourcesArePlaced) {
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto c = tmp.path / "c.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "shared bytes");
    write_file(b, "shared bytes");
    write_file(c, "shared bytes");

    auto r = stage_files({a, b, c}, dst, no_retry());
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->copied);
    EXPECT_EQ(read_file(dst), "shared bytes");
}

TEST(BuildStageFiles, AnAlreadyEquivalentDestinationIsNotTouched) {
    // The #311 property (BuildStage.EquivalentDestinationIsNotTouched) must
    // survive going through the multi-source entry point too.
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "same-bytes");
    write_file(b, "same-bytes");
    write_file(dst, "same-bytes");
    auto before = std::filesystem::file_time_type::clock::now() - std::chrono::hours{2};
    std::filesystem::last_write_time(dst, before);
    auto recorded = std::filesystem::last_write_time(dst);

    auto r = stage_files({a, b}, dst, no_retry());
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->copied);
    EXPECT_EQ(std::filesystem::last_write_time(dst), recorded);
}

TEST(BuildStageFiles, DisagreeingSourcesFailNamingEveryOneAndTheDestination) {
    Tmp tmp;
    auto a = tmp.path / "qtbase_zh_CN.qm";
    auto b = tmp.path / "other" / "qtbase_zh_CN.qm";
    auto dst = tmp.path / "bin" / "translations" / "qt_zh_CN.qm";
    write_file(a, "catalog A");
    write_file(b, "catalog B");

    auto r = stage_files({a, b}, dst, no_retry());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find(a.string()), std::string::npos)
        << r.error().message;
    EXPECT_NE(r.error().message.find(b.string()), std::string::npos)
        << r.error().message;
    EXPECT_NE(r.error().message.find(dst.string()), std::string::npos)
        << r.error().message;
    // Refused, not silently resolved to either source: the destination is
    // untouched (no partial write from a rejected attempt).
    EXPECT_FALSE(std::filesystem::exists(dst));
}

TEST(BuildStageFiles, ThreeSourcesWhereOnlyTheLastDisagreesAreAllNamed) {
    // A pairwise comparison against only the first neighbour would miss a
    // mismatch between the second and third; every source must be compared.
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto c = tmp.path / "c.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "X");
    write_file(b, "X");
    write_file(c, "Y");

    auto r = stage_files({a, b, c}, dst, no_retry());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find(c.string()), std::string::npos)
        << r.error().message;
}

TEST(BuildStageFiles, AMissingSourceIsNamedAsMissingNotAsADisagreement) {
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "never-written.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "X");

    auto r = stage_files({a, b}, dst, no_retry());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("does not exist"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find(b.string()), std::string::npos) << r.error().message;
    EXPECT_EQ(r.error().message.find("disagree"), std::string::npos) << r.error().message;
    EXPECT_FALSE(std::filesystem::exists(dst));
}

TEST(BuildStageFiles, EmptySourceListIsAnError) {
    Tmp tmp;
    auto r = stage_files({}, tmp.path / "dst.bin", no_retry());
    ASSERT_FALSE(r.has_value());
}
