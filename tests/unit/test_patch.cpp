
import std;
import mcpplibs.mtest;
import mcpp.pm.patch;

using namespace mcpplibs::mtest;

// A dependency's patches (design 2026-11-02 part 3 §14.1). The applier is
// exact: a hunk applies where its header says, every line of context and every
// line it removes byte for byte, or the patch is refused naming the hunk.

namespace fs = std::filesystem;
namespace pt = mcpp::pm::patch;

namespace {

fs::path fresh(std::string_view name) {
    auto d = fs::temp_directory_path() / std::format("mcpp_patch_{}_{}", name, std::random_device{}());
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void put(const fs::path& p, std::string_view bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

std::string get(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

UTest a_hunk_applies_where_its_header_says{"Patch/AHunkAppliesWhereItsHeaderSays", [] {
    auto root = fresh("apply");
    put(root / "src/a.c", "one\ntwo\nthree\nfour\n");
    const std::string diff =
        "--- a/src/a.c\n"
        "+++ b/src/a.c\n"
        "@@ -2,2 +2,3 @@\n"
        " two\n"
        "-three\n"
        "+THREE\n"
        "+three and a half\n";
    expect(fatal(bool(pt::apply(root, diff, "p.patch"))));
    expect(eq(get(root / "src/a.c"), "one\ntwo\nTHREE\nthree and a half\nfour\n"));
}};

UTest a_hunk_that_does_not_match_is_refused_naming_it{"Patch/AHunkThatDoesNotMatchIsRefusedNamingIt", [] {
    auto root = fresh("refuse");
    put(root / "src/a.c", "one\ntwo\nthree\n");
    const std::string diff =
        "--- a/src/a.c\n"
        "+++ b/src/a.c\n"
        "@@ -2,1 +2,1 @@\n"
        "-TWO\n"
        "+2\n";
    auto r = pt::apply(root, diff, "p.patch");
    expect(fatal(!r));
    if (auto&& failed = expect(ne(r.error().find("hunk 1"), std::string::npos)); !failed) failed << r.error();
    if (auto&& failed = expect(ne(r.error().find("line 2"), std::string::npos)); !failed) failed << r.error();
    expect(eq(get(root / "src/a.c"), "one\ntwo\nthree\n"));   // untouched
}};

UTest nothing_is_searched_for_elsewhere{"Patch/NothingIsSearchedForElsewhere", [] {
    // The text the hunk expects is in the file, one line later than its
    // header says: an exact applier refuses rather than finding it.
    auto root = fresh("nofuzz");
    put(root / "a.c", "zero\none\ntwo\n");
    const std::string diff = "--- a/a.c\n+++ b/a.c\n@@ -1,1 +1,1 @@\n-one\n+ONE\n";
    expect(!pt::apply(root, diff, "p.patch"));
}};

UTest bytes_are_kept_as_they_are{"Patch/BytesAreKeptAsTheyAre", [] {
    // CRLF lines stay CRLF, and a last line without a newline stays without.
    auto root = fresh("bytes");
    put(root / "w.txt", "a\r\nb\r\nc");
    const std::string diff =
        "--- a/w.txt\n+++ b/w.txt\n@@ -2,2 +2,2 @@\n b\r\n-c\n\\ No newline at end of file\n+C\n\\ No newline at end of file\n";
    expect(fatal(bool(pt::apply(root, diff, "p.patch"))));
    expect(eq(get(root / "w.txt"), "a\r\nb\r\nC"));
}};

UTest files_are_created_and_removed{"Patch/FilesAreCreatedAndRemoved", [] {
    auto root = fresh("create");
    put(root / "old.c", "x\n");
    const std::string diff =
        "diff --git a/new.c b/new.c\n"
        "new file mode 100644\n"
        "--- /dev/null\n+++ b/new.c\n@@ -0,0 +1,2 @@\n+int n;\n+int m;\n"
        "diff --git a/old.c b/old.c\n"
        "deleted file mode 100644\n"
        "--- a/old.c\n+++ /dev/null\n@@ -1,1 +0,0 @@\n-x\n";
    expect(fatal(bool(pt::apply(root, diff, "p.patch"))));
    expect(eq(get(root / "new.c"), "int n;\nint m;\n"));
    expect(!fs::exists(root / "old.c"));
}};

UTest the_set_digest_is_the_contents_in_order{"Patch/TheSetDigestIsTheContentsInOrder", [] {
    auto d = fresh("digest");
    put(d / "one.patch", "A");
    put(d / "two.patch", "B");
    put(d / "renamed.patch", "A");
    auto ab = pt::set_digest({(d / "one.patch").string(), (d / "two.patch").string()});
    auto ba = pt::set_digest({(d / "two.patch").string(), (d / "one.patch").string()});
    auto ab2 = pt::set_digest({(d / "renamed.patch").string(), (d / "two.patch").string()});
    expect(fatal(ab && ba && ab2));
    expect(ne(*ab, *ba));
    expect(eq(*ab, *ab2));
    expect(eq(ab->size(), 64u));
    expect(eq(pt::variant_dir_name("3.6.1", *ab), std::format("3.6.1+p{}", ab->substr(0, 12))));
}};

UTest a_variant_is_made_once_and_whole{"Patch/AVariantIsMadeOnceAndWhole", [] {
    auto d = fresh("variant");
    put(d / "src/inc/m.h", "#define M 1\n");
    put(d / "fix.patch", "--- a/inc/m.h\n+++ b/inc/m.h\n@@ -1 +1 @@\n-#define M 1\n+#define M 2\n");
    auto digest = pt::set_digest({(d / "fix.patch").string()});
    expect(fatal(bool(digest)));
    const auto variant = d / "variants" / pt::variant_dir_name("1.0", *digest);
    expect(fatal(bool(pt::materialize(d / "src", {(d / "fix.patch").string()}, *digest, variant))));
    expect(eq(get(variant / "inc/m.h"), "#define M 2\n"));
    expect(eq(get(d / "src/inc/m.h"), "#define M 1\n"));      // the source is untouched
    // A second call finds it made.
    put(variant / "inc/m.h", "#define M 3\n");
    expect(fatal(bool(pt::materialize(d / "src", {(d / "fix.patch").string()}, *digest, variant))));
    expect(eq(get(variant / "inc/m.h"), "#define M 3\n"));
    // A patch that does not apply leaves no variant.
    put(d / "bad.patch", "--- a/inc/m.h\n+++ b/inc/m.h\n@@ -1 +1 @@\n-#define M 9\n+#define M 2\n");
    auto bad = pt::set_digest({(d / "bad.patch").string()});
    const auto badVariant = d / "variants" / pt::variant_dir_name("1.0", *bad);
    expect(!(pt::materialize(d / "src", {(d / "bad.patch").string()}, *bad, badVariant)));
    expect(!fs::exists(badVariant));
}};

UTest a_variant_never_writes_through_a_link{"Patch/AVariantNeverWritesThroughALink", [] {
    // A store may hold a version directory, or a file in it, as a link into
    // another store (a test's home linking the developer's payloads, measured:
    // the first version of this patched the developer's own zlib.h through
    // it). The variant's tree is copied as files, and a patch writes there.
    auto d = fresh("links");
    put(d / "real/inc/m.h", "#define M 1\n");
    put(d / "real/inc/other.h", "#define O 1\n");
    std::error_code ec;
    fs::create_directory_symlink(d / "real", d / "linked", ec);
    if (ec) skip([&] { std::ostringstream text; text << "no symbolic links here: " << ec.message(); return text.str(); }());
    fs::create_symlink(d / "real/inc/other.h", d / "real/inc/o-link.h", ec);
    put(d / "fix.patch", "--- a/inc/m.h\n+++ b/inc/m.h\n@@ -1 +1 @@\n-#define M 1\n+#define M 2\n"
                         "--- a/inc/o-link.h\n+++ b/inc/o-link.h\n@@ -1 +1 @@\n-#define O 1\n+#define O 2\n");
    auto digest = pt::set_digest({(d / "fix.patch").string()});
    expect(fatal(bool(digest)));
    const auto variant = d / "variants" / pt::variant_dir_name("1.0", *digest);
    expect(fatal(bool(pt::materialize(d / "linked", {(d / "fix.patch").string()}, *digest, variant))));
    expect(eq(get(variant / "inc/m.h"), "#define M 2\n"));
    expect(eq(get(variant / "inc/o-link.h"), "#define O 2\n"));
    expect(!fs::is_symlink(variant));
    expect(eq(get(d / "real/inc/m.h"), "#define M 1\n"));
    expect(eq(get(d / "real/inc/other.h"), "#define O 1\n"));
}};

}  // namespace
