
import std;
import mcpplibs.mtest;
import mcpp.build.source_digest;

using namespace mcpplibs::mtest;

// THE CONTENT DIGEST OF A SOURCE TREE, RE-READ ONLY WHERE A STAT MOVED
// (mcpp#797, R6). A build program depends on every source of the packages in
// its graph, an included header among them.

namespace {
struct Tree {
    std::filesystem::path root, cache;
    Tree() : root(std::filesystem::temp_directory_path()
                  / std::format("mcpp-digest-{}", std::random_device{}())) {
        std::filesystem::create_directories(root / "src");
        cache = root.string() + ".tsv";
    }
    ~Tree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove(cache, ec);
    }
    void write(std::string_view rel, std::string_view text) {
        std::filesystem::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary | std::ios::trunc) << text;
    }
    std::string digest() { return mcpp::build::source_tree_digest(root, cache); }
};
}  // namespace

UTest an_edited_header_changes_it{"SourceDigest/AnEditedHeaderChangesIt", [] {
    Tree t;
    t.write("src/lib.cppm", "module;\n#include \"v.h\"\nexport module lib;\n");
    t.write("src/v.h", "#define V 1\n");
    const auto a = t.digest();
    expect(eq(t.digest(), a)) << "a tree that did not change keeps its digest";
    t.write("src/v.h", "#define V 2\n");
    expect(ne(t.digest(), a));
}};

UTest a_touch_without_an_edit_keeps_it{"SourceDigest/ATouchWithoutAnEditKeepsIt", [] {
    Tree t;
    t.write("src/v.h", "#define V 1\n");
    const auto a = t.digest();
    std::filesystem::last_write_time(t.root / "src/v.h",
        std::filesystem::last_write_time(t.root / "src/v.h") + std::chrono::seconds(5));
    expect(eq(t.digest(), a));
}};

UTest build_output_and_nested_packages_are_not_part_of_it{"SourceDigest/BuildOutputAndNestedPackagesAreNotPartOfIt", [] {
    Tree t;
    t.write("src/v.h", "#define V 1\n");
    const auto a = t.digest();
    t.write("target/obj/x.o", "binary");
    t.write(".git/HEAD", "ref");
    t.write("member/mcpp.toml", "[package]\n");
    t.write("member/src/m.cppm", "export module m;\n");
    expect(eq(t.digest(), a));
}};

UTest an_added_or_removed_file_changes_it{"SourceDigest/AnAddedOrRemovedFileChangesIt", [] {
    Tree t;
    t.write("src/a.cppm", "export module a;\n");
    const auto a = t.digest();
    t.write("src/b.cppm", "export module b;\n");
    const auto b = t.digest();
    expect(ne(b, a));
    std::filesystem::remove(t.root / "src/b.cppm");
    expect(eq(t.digest(), a));
}};
