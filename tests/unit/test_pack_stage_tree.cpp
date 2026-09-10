#include <gtest/gtest.h>

import std;
import mcpp.pack.stage_tree;

// The staged tree is what `mcpp pack` computes and, until `${mcpp.stage_dir}`,
// then threw away. Two things about it are the engine's contract with a
// distribution member, and both are asserted here rather than end to end,
// because the end-to-end criterion for either is "the distributable is
// rebuilt", which a wrong answer also satisfies.

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_stage_tree_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
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

} // namespace

TEST(PackStageTree, TheManifestIsASiblingAndNeverAMember) {
    Tmp t;
    auto stage = t.path / "app-1.0.0-x86_64-linux-gnu";
    std::filesystem::create_directories(stage);

    auto manifest = mcpp::pack::stage_manifest_path(stage);
    EXPECT_EQ(manifest.parent_path(), stage.parent_path());
    // A file INSIDE the tree would be collected by every format that packages
    // the directory wholesale, and would then ship inside the user's
    // installer. Asserted as a path relationship because that is the property,
    // and a spelling change that broke it would otherwise only show up as an
    // extra file in a released package.
    EXPECT_FALSE(manifest.string().starts_with(stage.string() + "/"));
    EXPECT_FALSE(manifest.string().starts_with(stage.string() + "\\"));

    ASSERT_TRUE(mcpp::pack::write_stage_manifest(stage));
    std::error_code ec;
    for (auto const& e : std::filesystem::recursive_directory_iterator(stage, ec))
        FAIL() << "the manifest landed inside the tree: " << e.path().string();
}

TEST(PackStageTree, TheManifestChangesWhenTheStagedSetDoes) {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "0123456789");
    ASSERT_TRUE(mcpp::pack::write_stage_manifest(stage));
    const auto first = read_file(mcpp::pack::stage_manifest_path(stage));
    EXPECT_NE(first.find("bin/app"), std::string::npos);
    EXPECT_NE(first.find("10 "), std::string::npos);

    // A DEPENDENCY'S SHARED LIBRARY JOINING THE CLOSURE. This is the case the
    // manifest exists for: the program's own bytes need not have changed, so an
    // edge that depended only on the link output would report the previous
    // distributable as up to date.
    write_file(stage / "lib" / "libdep.so.1", "xx");
    ASSERT_TRUE(mcpp::pack::write_stage_manifest(stage));
    const auto second = read_file(mcpp::pack::stage_manifest_path(stage));
    EXPECT_NE(first, second);
    EXPECT_NE(second.find("lib/libdep.so.1"), std::string::npos);

    // A staged file whose LENGTH changed, with no entry added or removed.
    write_file(stage / "bin" / "app", "0123456789abcdef");
    ASSERT_TRUE(mcpp::pack::write_stage_manifest(stage));
    EXPECT_NE(read_file(mcpp::pack::stage_manifest_path(stage)), second);
}

TEST(PackStageTree, StagingTheSameTreeTwiceLeavesTheManifestUntouched) {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "same");
    ASSERT_TRUE(mcpp::pack::write_stage_manifest(stage));
    auto manifest = mcpp::pack::stage_manifest_path(stage);
    const auto before = std::filesystem::last_write_time(manifest);

    // Rewriting identical bytes would move the mtime, and a moved mtime on an
    // input is indistinguishable from a changed input: a pack that staged the
    // same tree twice would rebuild the distributable both times. Same rule
    // `mcpp.build.stage` states at length for a staged BMI.
    ASSERT_TRUE(mcpp::pack::write_stage_manifest(stage));
    EXPECT_EQ(std::filesystem::last_write_time(manifest), before);
}

TEST(PackStageTree, TheOrderOfADirectoryWalkIsNotAPromise) {
    // Two trees with the same contents produce the same bytes. Without the
    // sort, an iteration order that differed between runs would make the dist
    // edge dirty on every pack for no reason -- which reads as "packaging is
    // slow" rather than as a defect.
    Tmp a, b;
    for (auto const& root : {a.path, b.path}) {
        write_file(root / "s" / "bin" / "app", "aa");
        write_file(root / "s" / "lib" / "z.so", "bbb");
        write_file(root / "s" / "share" / "doc" / "readme", "c");
        ASSERT_TRUE(mcpp::pack::write_stage_manifest(root / "s"));
    }
    EXPECT_EQ(read_file(mcpp::pack::stage_manifest_path(a.path / "s")),
              read_file(mcpp::pack::stage_manifest_path(b.path / "s")));
}

TEST(PackStageTree, TheEngineOwnsExactlyTwoFormatNames) {
    // `tar` and `dir` are the archive shapes `mcpp pack` owns; every other
    // value of `--format` is a name a package provides. The list lives beside
    // the staged tree because two layers need the same answer -- the parser,
    // which decides whether a value is a built-in or a dispatch, and the
    // declaration check, which refuses a package that claims one of them and
    // would therefore be silently unreachable.
    EXPECT_TRUE(mcpp::pack::is_builtin_pack_format("tar"));
    EXPECT_TRUE(mcpp::pack::is_builtin_pack_format("dir"));
    EXPECT_FALSE(mcpp::pack::is_builtin_pack_format("appimage"));
    EXPECT_FALSE(mcpp::pack::is_builtin_pack_format("msi"));
    EXPECT_FALSE(mcpp::pack::is_builtin_pack_format(""));
    EXPECT_EQ(mcpp::pack::kBuiltinPackFormats.size(), 2u);
}

TEST(PackStageTree, AMissingTreeIsRefusedRatherThanDescribedAsEmpty) {
    Tmp t;
    // An empty manifest for a directory that does not exist would say "nothing
    // is staged", which is what a correct pack of an empty bundle also says.
    // The two must not be spelled alike.
    EXPECT_FALSE(mcpp::pack::write_stage_manifest(t.path / "never-staged"));
    EXPECT_FALSE(std::filesystem::exists(
        mcpp::pack::stage_manifest_path(t.path / "never-staged")));
}
