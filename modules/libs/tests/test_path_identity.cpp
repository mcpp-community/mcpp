import std;
import mcpplibs.mtest;
import mcpp.libs.path_identity;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: a given path keeps its spelling, and identity is a question of its own
// (design 2026-11-27 §4.6, mcpp#1024).

namespace pi = mcpp::libs::path_identity;

UTest the_given_spelling_keeps_a_link{"PathIdentity/TheGivenSpellingKeepsALink", [] {
    const auto base = std::filesystem::temp_directory_path() / std::format("mcpp-path-identity-{}", std::random_device{}());
    std::filesystem::create_directories(base / "real" / "x");
    std::error_code ec;
    std::filesystem::create_directory_symlink(base / "real", base / "link", ec);
    if (ec) { std::filesystem::remove_all(base); return; }   // no privilege for a link here
    const auto viaLink = base / "link" / "." / "x";
    expect(eq(pi::given(viaLink), (base / "link" / "x").lexically_normal()));
    expect(fatal(pi::identity(viaLink).has_value()));
    expect(eq(*pi::identity(viaLink), *pi::identity(base / "real" / "x")));
    expect(eq(pi::same(viaLink, base / "real" / "x"), std::optional<bool>{true}));
    expect(eq(pi::same(viaLink, base / "real"), std::optional<bool>{false}));
    // A path that does not exist yet is resolved as far as it exists.
    expect(eq(pi::same(base / "link" / "new", base / "real" / "new"), std::optional<bool>{true}));
    std::filesystem::remove_all(base);
}};
