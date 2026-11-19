// mcpp.pm.descriptor --- the two forms of a descriptor's `mcpp` field, decided
// once (SPEC-001 §3.5).
//
// An implementation unit: with these definitions in the interface, GCC 16.1
// ended with an internal error (a segmentation fault) at every import of a
// module that reaches it (`import mcpp.cli;` in src/main.cpp).
module mcpp.pm.descriptor;

import std;
import mcpp.manifest;
import mcpp.modgraph.scanner;   // expand_glob
import mcpp.toolchain.platform;

namespace mcpp::pm {

bool descriptor_is_inline(std::string_view lua) {
    return mcpp::manifest::extract_mcpp_field(lua).kind == mcpp::manifest::McppField::TableBody;
}

bool descriptor_payload_present(std::string_view lua, const std::filesystem::path& versionDir,
                                std::string_view name, std::string_view version,
                                const mcpp::toolchain::platform::PlatformKey& platform) {
    const auto field = mcpp::manifest::extract_mcpp_field(lua);
    if (field.kind == mcpp::manifest::McppField::StringPath)
        return !mcpp::modgraph::expand_glob(versionDir, field.value).empty();
    if (field.kind == mcpp::manifest::McppField::TableBody) {
        auto inlined = mcpp::manifest::synthesize_from_xpkg_lua(lua, name, version, platform);
        if (!inlined) return false;
        for (auto const& [generatedPath, _] : inlined->buildConfig.generatedFiles)
            if (!generatedPath.empty()) return true;
        for (auto const& glob : inlined->modules.sources) {
            if (!glob.empty() && glob.front() == '!') continue;
            if (!mcpp::modgraph::expand_glob(versionDir, glob).empty()) return true;
        }
        return false;
    }
    for (auto pattern : { "mcpp.toml", "*/mcpp.toml" })
        if (!mcpp::modgraph::expand_glob(versionDir, pattern).empty()) return true;
    return false;
}

std::expected<DescriptorManifest, std::string>
descriptor_manifest(std::string_view lua, const std::filesystem::path& versionDir,
                    std::string_view name, std::string_view version,
                    const mcpp::toolchain::platform::PlatformKey& platform) {
    const auto field = mcpp::manifest::extract_mcpp_field(lua);
    DescriptorManifest out;
    if (field.kind == mcpp::manifest::McppField::TableBody) {
        auto inlined = mcpp::manifest::synthesize_from_xpkg_lua(lua, name, version, platform);
        if (!inlined) return std::unexpected(inlined.error().format());
        out.form = DescriptorManifest::Form::Inline;
        out.packageRoot = versionDir;
        out.manifest = std::move(*inlined);
        return out;
    }
    std::vector<std::filesystem::path> matches;
    if (field.kind == mcpp::manifest::McppField::StringPath) {
        matches = mcpp::modgraph::expand_glob(versionDir, field.value);
        if (matches.empty()) return std::unexpected(std::format(
            "mcpp pointer '{}' did not match any file under '{}'", field.value, versionDir.string()));
        if (matches.size() > 1) return std::unexpected(std::format(
            "mcpp pointer '{}' matched {} files under '{}' (expected exactly one)",
            field.value, matches.size(), versionDir.string()));
    } else {
        for (auto pattern : { "mcpp.toml", "*/mcpp.toml" }) {
            matches = mcpp::modgraph::expand_glob(versionDir, pattern);
            if (!matches.empty()) break;
        }
        // The directory searched is named: a version directory that belongs to
        // another package reads the same as a descriptor without the field,
        // and only the path tells the two apart.
        if (matches.empty()) return std::unexpected(std::format(
            "the index entry has no `mcpp = ...` field, and no mcpp.toml was found at "
            "'{}/mcpp.toml' or '{}/*/mcpp.toml'. A payload that carries its manifest is named by "
            "`mcpp = \"<glob>\"`; one that carries none states it inline, `mcpp = {{ sources = ... }}`. "
            "(If that directory belongs to a different package, the install resolved the wrong "
            "version directory.)", versionDir.string(), versionDir.string()));
        if (matches.size() > 1) return std::unexpected(std::format(
            "the default mcpp.toml lookup matched {} files under '{}'; name one with `mcpp = \"<glob>\"`",
            matches.size(), versionDir.string()));
    }
    out.form = DescriptorManifest::Form::Pointer;
    out.manifestPath = matches.front();
    out.packageRoot = out.manifestPath.parent_path();
    return out;
}

}  // namespace mcpp::pm
