// The tool store's stamp for a source tree that has no version of its own.
//
// A `path` tool package changes under an unchanged version, and the store
// keyed on the version alone served a binary built from sources that no longer
// existed (measured 2026-09-08, examples/12). The stamp is what ninja itself
// trusts: every regular file's relative path, size and modification time. The
// tests state the three properties the key needs: the same tree gives the same
// stamp, a change to a source moves it, and a change under the build products
// does not.

#include <gtest/gtest.h>
#include <fstream>

import std;
import mcpp.build.tool_store;

namespace fs = std::filesystem;

namespace {

struct Tree {
    fs::path root;
    explicit Tree(std::string_view name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root / "src");
        fs::create_directories(root / "target" / "x");
        write("mcpp.toml", "[package]\nname = \"t\"\nversion = \"0.1.0\"\n");
        write("src/gen.cpp", "int main() { return 41; }\n");
        write("target/x/gen.o", "object");
    }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }
    void write(std::string_view rel, std::string_view content) {
        std::ofstream(root / rel) << content;
    }
};

// Two writes in one second have equal mtimes at second resolution on some
// filesystems; a size change is the part of the stamp that cannot be
// masked by that, so every edit below also changes the length.
} // namespace

TEST(ToolStoreStamp, TheSameTreeGivesTheSameStamp) {
    Tree t("mcpp_tool_store_stamp_same");
    const auto a = mcpp::build::tool_store::tree_stamp(t.root);
    const auto b = mcpp::build::tool_store::tree_stamp(t.root);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 16u) << "sixteen hex digits, the fingerprint module's shape";
}

TEST(ToolStoreStamp, AnEditToASourceMovesTheStampInBothDirections) {
    Tree t("mcpp_tool_store_stamp_edit");
    const auto before = mcpp::build::tool_store::tree_stamp(t.root);
    t.write("src/gen.cpp", "int main() { return 42; } // edited\n");
    const auto edited = mcpp::build::tool_store::tree_stamp(t.root);
    EXPECT_NE(before, edited);
    // The reversal is a new state too: the file's length is back, its
    // modification time is not, and a key that only ever grew would miss
    // exactly this step.
    t.write("src/gen.cpp", "int main() { return 41; }\n");
    const auto reverted = mcpp::build::tool_store::tree_stamp(t.root);
    EXPECT_NE(edited, reverted);
}

TEST(ToolStoreStamp, BuildProductsAndTheVersionControlDirectoryDoNotCount) {
    Tree t("mcpp_tool_store_stamp_target");
    const auto before = mcpp::build::tool_store::tree_stamp(t.root);
    t.write("target/x/gen.o", "a different object of another length");
    fs::create_directories(t.root / ".git");
    t.write(".git/HEAD", "ref: refs/heads/main\n");
    fs::create_directories(t.root / ".mcpp");
    t.write(".mcpp/stamp", "x");
    t.write("compile_commands.json", "[]");
    const auto after = mcpp::build::tool_store::tree_stamp(t.root);
    EXPECT_EQ(before, after) << "build products, .git, .mcpp and the compile database are excluded";
}

// The sub-build scratch (mcpp#641, item 3). Every object path the sub-build
// writes is appended to it, so it spends no length on names the entry already
// records; it still separates two consumers of one entry, and a re-run by the
// same consumer finds the same directory.
TEST(ToolStoreScratch, IsShortPerConsumerAndStable) {
    const fs::path cache{"/home/u/.mcpp/build-cache/v1"};
    const fs::path entry = cache / "tool" / "mcpplibs"
        / "installer@0.1.0+path.e285b2f02cb1c429" / "7005eaf9bd8c80ef";
    const auto a1 = mcpp::build::tool_store::scratch_dir(cache, entry, "/work/app");
    const auto a2 = mcpp::build::tool_store::scratch_dir(cache, entry, "/work/app");
    const auto b  = mcpp::build::tool_store::scratch_dir(cache, entry, "/work/other");
    const auto other = mcpp::build::tool_store::scratch_dir(
        cache, entry.parent_path() / "0000000000000000", "/work/app");

    EXPECT_EQ(a1, a2);
    EXPECT_NE(a1, b);
    EXPECT_NE(a1, other);
    EXPECT_EQ(a1.parent_path(), cache / "tool" / ".build");
    EXPECT_EQ(a1.filename().string().size(), 16u) << a1;
    // The old scratch lived inside the entry, as `<entry>/build-<16 hex>`.
    EXPECT_LT(a1.generic_string().size(), (entry / "build-0123456789abcdef").generic_string().size());
}
