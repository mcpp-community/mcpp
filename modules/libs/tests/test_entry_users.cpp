import std;
import mcpplibs.mtest;
import mcpp.libs.entry_users;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the trees that use a stored entry (design 2026-11-27 §4.5, mcpp#1025). That the
// dependency cache and the tool store keep the entries another checkout's plan names is stated by
// the e2e suite.

namespace {

namespace fs = std::filesystem;

fs::path fresh(std::string_view name) {
    auto root = fs::temp_directory_path() / std::format("mcpp-entry-users-{}-{}", name,
                                                        std::random_device{}());
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// `<store>/pkg@0.1.0+path.<digest>/<key>/`
fs::path entry(const fs::path& store, std::string_view digest, std::string_view key = "k") {
    auto dir = store / std::format("pkg@0.1.0+path.{}", digest) / key;
    fs::create_directories(dir);
    std::ofstream(dir / "entry.json") << "{}";
    return dir;
}

constexpr std::string_view kPrefix = "pkg@0.1.0+path.";

} // namespace

UTest an_entry_another_tree_uses_stays{"EntryUsers/AnEntryAnotherTreeUsesStays", [] {
    const auto store = fresh("stays");
    const auto shared = entry(store, "aaaa");
    mcpp::libs::entry_users::supersede(shared, kPrefix, "/w/a/tool");   // a builds it
    mcpp::libs::entry_users::add(shared, "/w/b/tool");                  // b's plan hits it
    expect(eq(mcpp::libs::entry_users::users(shared).size(), 2u));

    const auto edited = entry(store, "bbbb");                           // a is edited and built
    mcpp::libs::entry_users::supersede(edited, kPrefix, "/w/a/tool");
    expect(fs::exists(shared / "entry.json")) << "the entry b uses was removed";
    expect(eq(mcpp::libs::entry_users::users(shared), std::vector<std::string>{"/w/b/tool"}));

    const auto alsoEdited = entry(store, "cccc");                       // b is edited and built
    mcpp::libs::entry_users::supersede(alsoEdited, kPrefix, "/w/b/tool");
    expect(!fs::exists(shared)) << "an entry no tree uses is kept";
    expect(!fs::exists(shared.parent_path())) << "an empty version directory is kept";
    expect(fs::exists(edited / "entry.json"));
    fs::remove_all(store);
}};

UTest a_tree_is_recorded_once{"EntryUsers/ATreeIsRecordedOnce", [] {
    const auto store = fresh("once");
    const auto e = entry(store, "aaaa");
    mcpp::libs::entry_users::add(e, "/w/a/tool");
    mcpp::libs::entry_users::add(e, "/w/a/./tool");
    expect(eq(mcpp::libs::entry_users::users(e), std::vector<std::string>{"/w/a/tool"}));
    fs::remove_all(store);
}};

// An entry an earlier release wrote names its one tree in `source`: it is read as a user, kept as
// one when another tree hits it, and removed as before when its tree supersedes it.
UTest the_former_record_is_read_as_a_user{"EntryUsers/TheFormerRecordIsReadAsAUser", [] {
    const auto store = fresh("former");
    const auto old = entry(store, "aaaa");
    std::ofstream(old / "source") << "/w/a/tool";
    expect(eq(mcpp::libs::entry_users::users(old), std::vector<std::string>{"/w/a/tool"}));

    const auto alone = entry(store, "aaaa", "k2");
    std::ofstream(alone / "source") << "/w/a/tool";
    mcpp::libs::entry_users::add(old, "/w/b/tool");
    expect(eq(mcpp::libs::entry_users::users(old).size(), 2u));

    mcpp::libs::entry_users::supersede(entry(store, "bbbb"), kPrefix, "/w/a/tool");
    expect(fs::exists(old / "entry.json")) << "the entry b hit was removed";
    expect(!fs::exists(alone)) << "an entry of the former record with no other user is kept";
    expect(!fs::exists(old / "source") || fs::exists(old / "users"));
    fs::remove_all(store);
}};

UTest another_package_is_not_a_sibling{"EntryUsers/AnotherPackageIsNotASibling", [] {
    const auto store = fresh("other");
    auto other = store / "pkgx@0.1.0+path.aaaa" / "k";
    fs::create_directories(other);
    mcpp::libs::entry_users::add(other, "/w/a/tool");
    mcpp::libs::entry_users::supersede(entry(store, "bbbb"), kPrefix, "/w/a/tool");
    expect(fs::exists(other / "users"));
    fs::remove_all(store);
}};
