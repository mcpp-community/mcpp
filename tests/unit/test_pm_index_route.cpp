
import std;
import mcpplibs.mtest;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.index_route;
import mcpp.pm.index_spec;

using namespace mcpplibs::mtest;

namespace {

struct LocalIndex {
    std::filesystem::path root;

    explicit LocalIndex(std::string_view name) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-index-route-{}", name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "pkgs" / "a");
        std::ofstream(root / "pkgs" / "a" / "acme.util.lua") << R"(
package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    type = "package",
}
)";
    }
    ~LocalIndex() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

mcpp::pm::IndexMap local_map(const LocalIndex& idx) {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .path = idx.root };
    return indices;
}

} // namespace

UTest find_for_ns_resolves_nested_namespace_to_its_root_index{"PmIndexRoute/FindForNsResolvesNestedNamespaceToItsRootIndex", [] {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    expect(ne(route.find_for_ns("acme"), nullptr));
    expect(ne(route.find_for_ns("acme.nested"), nullptr));
    expect(eq(route.find_for_ns("other"), nullptr));
}};

UTest lazy_git_index_cannot_refute_a_miss{"PmIndexRoute/LazyGitIndexCannotRefuteAMiss", [] {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    expect(bool(route.lazy_git("acme")));
    expect(!route.authoritative_for("acme"));
}};

UTest local_path_index_is_authoritative{"PmIndexRoute/LocalPathIndexIsAuthoritative", [] {
    LocalIndex idx("authoritative");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    expect(!route.lazy_git("acme"));
    expect(bool(route.authoritative_for("acme")));
}};

UTest route_description_does_not_expose_its_local_path{"PmIndexRoute/RouteDescriptionDoesNotExposeItsLocalPath", [] {
    LocalIndex idx("diagnostic-privacy");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    const auto description = route.describe("acme");
    expect(eq(description, "local index 'acme': root present, pkgs present"));
    expect(eq(description.find(idx.root.string()), std::string::npos));
}};

UTest route_description_distinguishes_a_missing_local_root{"PmIndexRoute/RouteDescriptionDistinguishesAMissingLocalRoot", [] {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{
        .name = "acme", .path = "missing-local-index" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    expect(eq(route.describe("acme"), "local index 'acme': root absent, pkgs absent"));
}};

// The regression behind #305/#307: a dotted selector is one exact NAMESPACE
// PATH, so it resolves through the coordinate the manifest parser derives. Probing the
// literal short name can never match — `package.name` is a single atomic
// segment, so nothing in any index is named "acme.util".
UTest dotted_selector_resolves_through_its_exact_coordinate{"PmIndexRoute/DottedSelectorResolvesThroughItsExactCoordinate", [] {
    LocalIndex idx("dotted");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    auto selector = mcpp::pm::resolve_dependency_selector("acme.util");
    expect(fatal(eq(selector.candidates.size(), 1u)));
    auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);

    expect(fatal(found.hit.has_value()));
    expect(eq(found.hit->coord.namespace_, "acme"));
    expect(eq(found.hit->coord.shortName, "util"));
}};

UTest name_only_descriptor_inherits_owning_local_index_namespace{"PmIndexRoute/NameOnlyDescriptorInheritsOwningLocalIndexNamespace", [] {
    LocalIndex idx("name-only");
    std::ofstream(idx.root / "pkgs" / "a" / "acme.util.lua") << R"(
package = {
    spec = "1",
    name = "util",
    type = "package",
}
)";
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    auto selector = mcpp::pm::resolve_dependency_selector("acme.util");
    auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);

    expect(fatal(found.hit.has_value()));
    expect(eq(found.hit->coord.namespace_, "acme"));
    expect(eq(found.hit->coord.shortName, "util"));
}};

UTest malformed_candidate_is_an_error_not_a_not_found_miss{"PmIndexRoute/MalformedCandidateIsAnErrorNotANotFoundMiss", [] {
    LocalIndex idx("malformed");
    std::ofstream(idx.root / "pkgs" / "a" / "acme.util.lua") << R"(
package = {
    spec = "1",
    namespace = "acme",
    name = "nested.util",
    type = "package",
}
)";
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    // The filename matches no candidate for this exact coordinate, so the
    // identity scan must attribute the malformed dotted name diagnostically.
    auto selector = mcpp::pm::resolve_dependency_selector("acme.nested.util");
    auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);

    expect(!found.hit.has_value());
    expect(ne(found.error.find("single atomic segment"), std::string::npos));
}};

UTest literal_dotted_short_name_never_matches{"PmIndexRoute/LiteralDottedShortNameNeverMatches", [] {
    LocalIndex idx("literal");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    std::vector<mcpp::pm::DependencyCoordinate> literal{
        { .namespace_ = "mcpplibs", .shortName = "acme.util" },
    };
    expect(!(mcpp::pm::lookup_descriptor(route, literal).hit.has_value()));
}};

UTest miss_in_a_readable_index_is_conclusive{"PmIndexRoute/MissInAReadableIndexIsConclusive", [] {
    LocalIndex idx("conclusive");
    auto indices = local_map(idx);
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    std::vector<mcpp::pm::DependencyCoordinate> missing{
        { .namespace_ = "acme", .shortName = "nope" },
    };
    auto found = mcpp::pm::lookup_descriptor(route, missing);
    expect(!found.hit.has_value());
    expect(bool(found.conclusive));
}};

UTest miss_behind_an_unreadable_index_is_not_conclusive{"PmIndexRoute/MissBehindAnUnreadableIndexIsNotConclusive", [] {
    mcpp::pm::IndexMap indices;
    indices["acme"] = mcpp::pm::IndexSpec{ .name = "acme", .url = "https://x/y" };
    mcpp::pm::IndexRoute route{ &indices, "/nowhere", nullptr };

    std::vector<mcpp::pm::DependencyCoordinate> missing{
        { .namespace_ = "acme", .shortName = "nope" },
    };
    auto found = mcpp::pm::lookup_descriptor(route, missing);
    expect(!found.hit.has_value());
    expect(!found.conclusive);
}};

UTest workspace_member_reads_root_anchored_relative_index{"PmIndexRoute/WorkspaceMemberReadsRootAnchoredRelativeIndex", [] {
    auto root = std::filesystem::temp_directory_path()
              / "mcpp-index-route-workspace-relative";
    std::filesystem::remove_all(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};

    std::filesystem::create_directories(root / "index" / "pkgs" / "a");
    std::filesystem::create_directories(root / "member");
    std::ofstream(root / "index" / "pkgs" / "a" / "acme.util.lua") << R"(
package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    type = "package",
}
)";
    std::ofstream(root / "mcpp.toml") << R"(
[workspace]
members = ["member"]

[indices]
acme = { path = "index" }
)";
    std::ofstream(root / "member" / "mcpp.toml") << R"(
[package]
name = "member"
version = "0.1.0"
)";

    auto indices = mcpp::pm::effective_indices(root / "member");
    expect(fatal(indices.contains("acme")));

    // Assert the CAPABILITY (the member can read the root's index), not the
    // path SPELLING. The first version of this test compared the stored path
    // against `(root / "index").lexically_normal()`, which passes for any
    // anchoring rule that happens to produce that string and says nothing
    // about whether the index is reachable — which is why it stayed green
    // while the Windows suite was red.
    mcpp::pm::IndexRoute route{ &indices, root / "member", nullptr };
    expect(eq(route.describe("acme"), "local index 'acme': root present, pkgs present"));
    auto selector = mcpp::pm::resolve_dependency_selector("acme.util");
    auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);
    expect(fatal(found.hit.has_value()));
    expect(eq(found.hit->coord.namespace_, "acme"));
    expect(eq(found.hit->coord.shortName, "util"));
}};

// Why `inherit_workspace_indices` anchors with `lexically_normal` and not
// `weakly_canonical`.
//
// Anchoring answers "WHICH directory did the workspace author mean", and the
// answer must stay inside the tree the author addressed. `weakly_canonical`
// answers a different question — "what is this path after every symlink is
// resolved" — and a workspace reached through a symlinked parent therefore
// gets relocated into a tree the author never wrote. That is observable:
// `prepare` reports a missing descriptor as "not found in local index at
// '<path>'", and this project deliberately keeps such diagnostics inside the
// user's own declared locations.
//
// (Recorded because the original motivation given for this change — a Windows
// short-name alias — turned out NOT to be the cause of the red Windows suite;
// that was a fixture writing an MSYS path into mcpp.toml. The change survives
// on this invariant, which is why the invariant is now a test.)
UTest inherited_index_stays_inside_the_declared_workspace_tree{"PmIndexRoute/InheritedIndexStaysInsideTheDeclaredWorkspaceTree", [] {
#ifdef _WIN32
    skip("directory symlinks need elevation on Windows");
#else
    auto base = std::filesystem::temp_directory_path()
              / "mcpp-index-route-workspace-symlink";
    std::filesystem::remove_all(base);
    struct Cleanup {
        std::filesystem::path base;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
    } cleanup{base};

    const auto real = base / "real";
    const auto link = base / "link";
    std::filesystem::create_directories(real / "index" / "pkgs" / "a");
    std::filesystem::create_directories(real / "member");
    std::ofstream(real / "index" / "pkgs" / "a" / "acme.util.lua") << R"(
package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    type = "package",
}
)";
    std::ofstream(real / "mcpp.toml") << R"(
[workspace]
members = ["member"]

[indices]
acme = { path = "index" }
)";
    std::ofstream(real / "member" / "mcpp.toml") << R"(
[package]
name = "member"
version = "0.1.0"
)";
    std::error_code ec;
    std::filesystem::create_directory_symlink(real, link, ec);
    if (ec) skip([&] { std::ostringstream text; text << "symlinks unavailable here: " << ec.message(); return text.str(); }());

    auto indices = mcpp::pm::effective_indices(link / "member");
    expect(fatal(indices.contains("acme")));

    // The declared tree is preserved: the anchored path is still addressed
    // through `link`, not rewritten to `real`.
    const auto anchored = indices.at("acme").path.string();
    expect(anchored.starts_with(link.string()))
        << "anchoring relocated the index out of the declared tree: "
        << anchored;

    // …and it is still readable through that spelling. Preserving identity is
    // only correct if the index remains reachable.
    mcpp::pm::IndexRoute route{ &indices, link / "member", nullptr };
    expect(eq(route.describe("acme"), "local index 'acme': root present, pkgs present"));
    auto selector = mcpp::pm::resolve_dependency_selector("acme.util");
    auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);
    expect(fatal(found.hit.has_value()));
    expect(eq(found.hit->coord.namespace_, "acme"));
#endif
}};
