// mcpp.pm.package_manifest — the manifest a package of an index states for
// one version, read one way for every reader (design 2026-11-21, package
// features §3.2): the build's graph and the queries of a package's features.
//
// A descriptor states its manifest in one of two forms (mcpp.pm.descriptor):
// the inline table, or a manifest inside the payload. A manifest inside the
// payload that is a member of a workspace in the archive receives that
// workspace's inheritance, as it does from a git clone of the same commit
// (#690). A reader that skipped the inheritance, or read the table where the
// build reads the payload, would answer another package than the one built.
export module mcpp.pm.package_manifest;

import std;
import mcpp.manifest;
import mcpp.project;
import mcpp.pm.descriptor;
import mcpp.toolchain.platform;
import nlohmann.json;
import mcpplibs.os.fs;

export namespace mcpp::pm {

// A workspace whose `members` list a directory, and its root. A named struct
// rather than a `std::pair`: an exported pair truncates an importer's BMI under
// GCC 16 (measured with mcpp.pm.bundle).
struct WorkspaceListing {
    mcpp::manifest::Manifest manifest;
    std::filesystem::path    root;
};

std::optional<std::string>
inherit_as_workspace_member(mcpp::manifest::Manifest& member,
                            const mcpp::manifest::Manifest& workspace,
                            const std::filesystem::path& workspaceRoot,
                            const std::filesystem::path& memberDir,
                            bool reportPosition = true);

std::optional<WorkspaceListing>
workspace_listing(const std::filesystem::path& memberDir,
                  const std::filesystem::path& bound);

// THE MANIFEST OF ONE INSTALLED VERSION. `lua` is the descriptor, `versionDir`
// the version's directory in the store. `root` is the package's root: the
// directory of the manifest inside the payload, or `versionDir` for an inline
// table.
struct InstalledManifest {
    mcpp::manifest::Manifest manifest;
    std::filesystem::path    root;
    bool                     inlineForm = false;
};
// What could not be read, without the package's name: the caller says whose
// descriptor it is. `at` is the manifest that failed to load, when one did; a
// path found in the store, so it is written as UTF-8 and never narrowed through
// the code page (`path::string()` throws there for a name it cannot spell).
struct ManifestError {
    std::filesystem::path at;
    std::string           message;
    std::string format(std::string_view subject) const {
        return at.empty() ? std::format("{}: {}", subject, message)
                          : std::format("{} (at '{}'): {}", subject, mcpplibs::os::fs::utf8(at), message);
    }
};
std::expected<InstalledManifest, ManifestError>
installed_manifest(std::string_view lua, const std::filesystem::path& versionDir,
                   std::string_view name, std::string_view version,
                   const mcpp::toolchain::platform::PlatformKey& platform);

// THE FEATURES OF ONE VERSION OF AN INDEX PACKAGE (mcpp#992), answered from
// what is on this machine and never by installing (SPEC-010 §5):
//   declared       from the index (an inline table) or the installed payload
//   not-installed  the manifest is inside a payload this home does not hold
//   unreadable     the payload is here and its manifest cannot be read
// `list` is meaningful only when declared; `from` is `index` or `payload`.
struct FeatureAnswer {
    std::string                              version;
    std::string                              state;
    std::string                              from;
    std::vector<mcpp::manifest::FeatureInfo> list;
    std::string                              error;   // unreadable: why
};
// `versionDir` is the version's directory when the home holds it, empty
// otherwise.
FeatureAnswer package_features(std::string_view lua, const std::filesystem::path& versionDir,
                               std::string_view name, std::string_view version,
                               const mcpp::toolchain::platform::PlatformKey& platform);

// The JSON every output states a package's features in.
nlohmann::json feature_list_json(const std::vector<mcpp::manifest::FeatureInfo>& list);
nlohmann::json feature_answer_json(const FeatureAnswer& answer);

} // namespace mcpp::pm

namespace mcpp::pm {

// WHAT A MEMBER RECEIVES FROM ITS WORKSPACE WHEN IT IS REACHED AS A DEPENDENCY.
//
// Three parts of the inheritance matter to a dependency: `[workspace.package]`
// (a member may omit `version`), `x.workspace = true` dependency entries
// (without the merge the entry reaches resolution with neither version nor
// path), `[workspace.build]`, `[workspace.xlings]` and the rows of
// `[workspace.target.<sel>]`. They are applied at the dependency's LOAD
// site, before the conditional merge and the `defines` fold, which is the
// order the root follows; `makePackageRoot` only captures the result (#690).
// `[workspace.toolchain]`, the scalar rows of `[workspace.target.<triple>]`,
// `[workspace.indices]` and `[workspace.profile.*]` describe the build, which
// the root decides for the whole graph; they are not applied to a dependency.
//
// One function for every way a member is reached: a sibling `path`
// dependency, a member of a git-hosted workspace, and a member inside an
// index package's archive. The same commit then compiles the same way in its
// own checkout and in every consumer's graph.
//
// `reportPosition` is false for a member of the workspace the build itself is
// rooted in: the root received the same position-read entries and was told
// about them, so telling the dependency too reports a use that changes
// nothing (mcpp#797, R5). `[xlings.overrides]` never travels here: it belongs
// to the root of a build (R4).
std::optional<std::string>
inherit_as_workspace_member(mcpp::manifest::Manifest& member,
                            const mcpp::manifest::Manifest& workspace,
                            const std::filesystem::path& workspaceRoot,
                            const std::filesystem::path& memberDir,
                            bool reportPosition) {
    mcpp::project::inherit_workspace_package(member, workspace);
    mcpp::project::merge_workspace_deps(member, workspace, workspaceRoot);
    mcpp::project::inherit_workspace_build(member, workspace, workspaceRoot);
    mcpp::project::inherit_workspace_xlings(member, workspace, reportPosition);
    mcpp::project::inherit_workspace_layer_rows(member, workspace, workspaceRoot);
    return mcpp::project::workspace_inheritance_error(member, memberDir);
}

// The workspace whose `members` list `memberDir`, searched upward from its
// parent and never above `bound` (an index package's install root: the
// archive is the only tree the package's author wrote).
std::optional<WorkspaceListing>
workspace_listing(const std::filesystem::path& memberDir,
                  const std::filesystem::path& bound) {
    auto inside = [&](const std::filesystem::path& p) {
        auto rel = p.lexically_normal().lexically_relative(bound.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    };
    for (auto p = memberDir.parent_path(); inside(p); p = p.parent_path()) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            if (auto ws = mcpp::manifest::load(p / "mcpp.toml");
                ws && ws->workspace.present
                && mcpp::project::is_workspace_member(*ws, p, memberDir))
                return WorkspaceListing{std::move(*ws), p};
        }
        if (p == p.parent_path()) break;
    }
    return std::nullopt;
}

std::expected<InstalledManifest, ManifestError>
installed_manifest(std::string_view lua, const std::filesystem::path& versionDir,
                   std::string_view name, std::string_view version,
                   const mcpp::toolchain::platform::PlatformKey& platform) {
    auto stated = descriptor_manifest(lua, versionDir, name, version, platform);
    if (!stated) return std::unexpected(ManifestError{.message = stated.error()});
    InstalledManifest out;
    if (stated->form == DescriptorManifest::Form::Inline) {
        out.manifest = std::move(*stated->manifest);
        out.root = versionDir;
        out.inlineForm = true;
    } else {
        const auto& toml = stated->manifestPath;
        auto repoWorkspace = workspace_listing(toml.parent_path(), versionDir);
        auto dm = mcpp::manifest::load(toml, {.insideWorkspace = repoWorkspace.has_value()});
        if (!dm) return std::unexpected(ManifestError{.at = toml, .message = dm.error().format()});
        if (repoWorkspace)
            if (auto bad = inherit_as_workspace_member(*dm, repoWorkspace->manifest, repoWorkspace->root,
                                                       toml.parent_path()))
                return std::unexpected(ManifestError{.message = *bad});
        if (auto bad = mcpp::project::unresolved_workspace_dependency_error(*dm, toml.parent_path()))
            return std::unexpected(ManifestError{.message = *bad});
        out.manifest = std::move(*dm);
        out.root = toml.parent_path();
    }
    // The descriptor's namespace, for a manifest that states none (one whose
    // `mcpp.toml` predates the field), unless its name already carries it.
    const auto luaNs = mcpp::manifest::extract_xpkg_namespace(lua);
    if (auto& pkg = out.manifest.package; pkg.namespace_.empty() && !luaNs.empty()
                                          && !pkg.name.starts_with(luaNs + "."))
        pkg.namespace_ = luaNs;
    return out;
}

FeatureAnswer package_features(std::string_view lua, const std::filesystem::path& versionDir,
                               std::string_view name, std::string_view version,
                               const mcpp::toolchain::platform::PlatformKey& platform) {
    FeatureAnswer a{.version = std::string(version)};
    const bool inlineForm = descriptor_is_inline(lua);
    a.from = inlineForm ? "index" : "payload";
    std::error_code ec;
    if (!inlineForm && (versionDir.empty() || !std::filesystem::is_directory(versionDir, ec)
                        || !descriptor_payload_present(lua, versionDir, name, version, platform))) {
        a.state = "not-installed";
        return a;
    }
    auto m = installed_manifest(lua, versionDir, name, version, platform);
    if (!m) {
        a.state = "unreadable";
        a.error = m.error().format("the manifest");
        return a;
    }
    a.state = "declared";
    a.list = mcpp::manifest::feature_catalog(m->manifest);
    return a;
}

nlohmann::json feature_list_json(const std::vector<mcpp::manifest::FeatureInfo>& list) {
    nlohmann::json out = nlohmann::json::array();
    for (auto const& f : list) {
        nlohmann::json e{{"name", f.name}, {"default", f.isDefault}, {"implies", f.implies},
                         {"enables", f.enables}};
        if (!f.description.empty()) e["description"] = f.description;
        out.push_back(std::move(e));
    }
    return out;
}

nlohmann::json feature_answer_json(const FeatureAnswer& a) {
    nlohmann::json out{{"version", a.version}, {"state", a.state}, {"from", a.from}};
    out["list"] = a.state == "declared" ? feature_list_json(a.list) : nlohmann::json(nullptr);
    if (!a.error.empty()) out["error"] = a.error;
    return out;
}

} // namespace mcpp::pm
