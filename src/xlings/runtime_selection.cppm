// mcpp.xlings.runtime_selection — the sole project runtime-selection policy.
//
// This module chooses a name and an owner.  It deliberately does not inspect
// the process environment, xlings' active/current state, the compiler path, or
// dependency manifests.  Those are all ambient or transitive inputs; allowing
// any of them to choose the build OS would make one mcpp.toml mean different
// ABIs in different shells.

export module mcpp.xlings.runtime_selection;

import std;
import mcpp.manifest;

export namespace mcpp::xlings::runtime {

struct RuntimeSelection {
    enum class Mode { McppDefault, NamedSubos } mode = Mode::McppDefault;
    enum class Source { DefaultPolicy, Manifest } source = Source::DefaultPolicy;

    std::string subosName = "default";
    std::filesystem::path ownerRoot;
};

namespace detail {

bool valid_subos_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

} // namespace detail

// `workspaceManifest` is present only when this invocation is a workspace
// build.  Its declaration and root then own the environment even though the
// package manifest later switches to a selected member.  For an independently
// built member, pass nullopt and the member is the root by definition.
std::expected<RuntimeSelection, std::string>
select_runtime(
    const mcpp::manifest::Manifest& projectManifest,
    std::optional<std::reference_wrapper<const mcpp::manifest::Manifest>>
        workspaceManifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& workspaceRoot = {}) {
    const auto& owner = workspaceManifest ? workspaceManifest->get()
                                          : projectManifest;

    RuntimeSelection out;
    out.ownerRoot = workspaceManifest && !workspaceRoot.empty()
        ? workspaceRoot : projectRoot;

    if (!owner.xlings.subosDeclared) return out;

    if (!detail::valid_subos_name(owner.xlings.subos)) {
        return std::unexpected(std::format(
            "[xlings].subos must be a non-empty portable name containing only "
            "letters, digits, '.', '_' or '-' (got '{}')",
            owner.xlings.subos));
    }

    out.mode = RuntimeSelection::Mode::NamedSubos;
    out.source = RuntimeSelection::Source::Manifest;
    out.subosName = owner.xlings.subos;
    return out;
}

} // namespace mcpp::xlings::runtime
