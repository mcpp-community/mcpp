// mcpp.platform.runtime_binding — immutable runtime contract snapshot.
//
// RuntimeSelection decides WHICH local development OS is authoritative.  This
// module reads that exact SubOS once and turns its xlings contract into a value
// carried by the build plan, fingerprint and fast-path cache.  No later phase
// is allowed to follow `current`, inspect an active shell, or re-read the file.

export module mcpp.platform.runtime_binding;

import std;
import mcpp.config;
import mcpp.libs.json;
import mcpp.platform;
import mcpp.xlings.runtime_selection;
import mcpp.xlings.subos_info;

export namespace mcpp::platform::runtime {

struct RuntimeBinding {
    int schema = 0;
    std::string providerId;
    std::string platform;
    std::string arch;
    // Provider-native runtime identity (`glibc@...`, `macos_sdk@...`,
    // `ucrt@...`). Only Linux additionally projects this into `libc`.
    std::string runtimeId;
    std::string contractHash;
    std::optional<std::filesystem::path> loader;
    std::optional<std::string> libc;
    std::vector<std::filesystem::path> libraryDirs;
    std::vector<mcpp::xlings::subos::EnvDecl> environment;
    std::vector<std::string> providerBindings;
    std::vector<std::string> capabilities;
    std::string provenance;
    std::filesystem::path subosDir;
    mcpp::xlings::runtime::RuntimeSelection selection;
};

namespace detail {

std::string hash_contract(std::string_view data) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    static constexpr char hex[] = "0123456789abcdef";
    char buf[16];
    for (int i = 15; i >= 0; --i) {
        buf[i] = hex[h & 0xf];
        h >>= 4;
    }
    return std::string(buf, sizeof(buf));
}

void append_field(std::string& out, std::string_view value) {
    out += std::to_string(value.size());
    out.push_back(':');
    out.append(value);
    out.push_back(';');
}

std::string canonical_contract(const RuntimeBinding& binding) {
    std::string out;
    append_field(out, std::to_string(binding.schema));
    append_field(out, binding.providerId);
    append_field(out, binding.platform);
    append_field(out, binding.arch);
    append_field(out, binding.runtimeId);
    append_field(out, binding.selection.mode
        == mcpp::xlings::runtime::RuntimeSelection::Mode::McppDefault
            ? "mcpp-default" : "named");
    append_field(out, binding.selection.subosName);
    append_field(out, binding.provenance);
    append_field(out, binding.loader ? binding.loader->generic_string() : "");
    append_field(out, binding.libc.value_or(""));
    for (auto const& p : binding.libraryDirs)
        append_field(out, p.generic_string());
    for (auto const& provider : binding.providerBindings)
        append_field(out, provider);
    for (auto const& d : binding.environment) {
        append_field(out, d.var);
        append_field(out, d.op);
        append_field(out, d.value);
    }
    for (auto const& capability : binding.capabilities)
        append_field(out, capability);
    return out;
}

std::filesystem::path subos_path(
    const mcpp::xlings::runtime::RuntimeSelection& selection,
    const mcpp::config::GlobalConfig& cfg) {
    using Mode = mcpp::xlings::runtime::RuntimeSelection::Mode;
    if (selection.mode == Mode::McppDefault || selection.subosName == "default")
        return cfg.xlingsHome() / "subos" / "default";
    return selection.ownerRoot / ".mcpp" / ".xlings" / "subos"
         / selection.subosName;
}

} // namespace detail

std::expected<RuntimeBinding, std::string>
resolve_runtime_binding(
    const mcpp::xlings::runtime::RuntimeSelection& selection,
    const std::filesystem::path& /*compiler*/,
    const mcpp::config::GlobalConfig& cfg) {
    RuntimeBinding out;
    out.selection = selection;
    out.subosDir = detail::subos_path(selection, cfg).lexically_normal();

    std::error_code ec;
    if (!std::filesystem::is_directory(out.subosDir, ec)) {
        return std::unexpected(std::format(
            "selected SubOS '{}' does not exist at {}; create/bootstrap that "
            "environment instead of falling back to active/default",
            selection.subosName, out.subosDir.string()));
    }

    auto info = mcpp::xlings::subos::read(out.subosDir);
    if (!info.present) {
        return std::unexpected(std::format(
            "selected SubOS '{}' cannot provide a RuntimeBinding: {}",
            selection.subosName, info.note));
    }
    if (info.schema != mcpp::xlings::subos::kSupportedSchema) {
        return std::unexpected(std::format(
            "selected SubOS '{}' uses runtime contract schema {}, but this "
            "mcpp requires schema {}; update xlings/mcpp before building",
            selection.subosName, info.schema,
            mcpp::xlings::subos::kSupportedSchema));
    }
    if (info.runtime.empty()) {
        return std::unexpected(std::format(
            "selected SubOS '{}' has no runtime identity in subos_info.runtime",
            selection.subosName));
    }

    out.schema = info.schema;
    out.providerId = "xlings";
    out.platform = std::string(mcpp::platform::name);
    out.arch = std::string(mcpp::platform::host_arch);
    out.runtimeId = info.runtime;
    out.provenance = selection.mode
        == mcpp::xlings::runtime::RuntimeSelection::Mode::McppDefault
            ? "mcpp_default" : "named_subos";

    // libc/ELF are Linux concepts.  macOS and Windows still carry the same
    // provider/environment contract without inventing a glibc field.
    if constexpr (mcpp::platform::is_linux) out.libc = info.runtime;

    for (auto const& provider : info.providers) {
        out.providerBindings.push_back(provider.binding);
        for (auto const& decl : provider.decls)
            out.environment.push_back(decl);
    }
    std::sort(out.capabilities.begin(), out.capabilities.end());
    out.capabilities.erase(
        std::unique(out.capabilities.begin(), out.capabilities.end()),
        out.capabilities.end());
    out.contractHash = detail::hash_contract(detail::canonical_contract(out));
    return out;
}

std::string serialize_runtime_binding(const RuntimeBinding& binding) {
    nlohmann::json j;
    j["schema"] = binding.schema;
    j["provider_id"] = binding.providerId;
    j["platform"] = binding.platform;
    j["arch"] = binding.arch;
    j["runtime_id"] = binding.runtimeId;
    j["contract_hash"] = binding.contractHash;
    j["loader"] = binding.loader ? binding.loader->generic_string() : "";
    j["libc"] = binding.libc.value_or("");
    j["library_dirs"] = nlohmann::json::array();
    for (auto const& path : binding.libraryDirs)
        j["library_dirs"].push_back(path.generic_string());
    j["environment"] = nlohmann::json::array();
    for (auto const& decl : binding.environment)
        j["environment"].push_back({
            {"var", decl.var}, {"op", decl.op}, {"value", decl.value}});
    j["provider_bindings"] = binding.providerBindings;
    j["capabilities"] = binding.capabilities;
    j["provenance"] = binding.provenance;
    j["subos_dir"] = binding.subosDir.generic_string();
    j["selection"] = {
        {"mode", binding.selection.mode
            == mcpp::xlings::runtime::RuntimeSelection::Mode::McppDefault
                ? "mcpp_default" : "named_subos"},
        {"source", binding.selection.source
            == mcpp::xlings::runtime::RuntimeSelection::Source::DefaultPolicy
                ? "default_policy" : "manifest"},
        {"name", binding.selection.subosName},
        {"owner_root", binding.selection.ownerRoot.generic_string()},
    };
    return j.dump();
}

std::expected<RuntimeBinding, std::string>
deserialize_runtime_binding(std::string_view encoded) {
    auto j = nlohmann::json::parse(encoded, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::unexpected("cached RuntimeBinding is not valid JSON");
    try {
        RuntimeBinding out;
        out.schema = j.value("schema", 0);
        out.providerId = j.value("provider_id", "");
        out.platform = j.value("platform", "");
        out.arch = j.value("arch", "");
        out.runtimeId = j.value("runtime_id", "");
        out.contractHash = j.value("contract_hash", "");
        if (auto v = j.value("loader", ""); !v.empty()) out.loader = v;
        if (auto v = j.value("libc", ""); !v.empty()) out.libc = v;
        if (auto it = j.find("library_dirs"); it != j.end() && it->is_array())
            for (auto const& v : *it) if (v.is_string())
                out.libraryDirs.emplace_back(v.get<std::string>());
        if (auto it = j.find("environment"); it != j.end() && it->is_array()) {
            for (auto const& v : *it) {
                if (!v.is_object()) continue;
                mcpp::xlings::subos::EnvDecl decl;
                decl.var = v.value("var", "");
                decl.op = v.value("op", "");
                decl.value = v.value("value", "");
                if (!decl.var.empty() && (decl.op == "set" || decl.op == "prepend"))
                    out.environment.push_back(std::move(decl));
            }
        }
        out.providerBindings = j.value(
            "provider_bindings", std::vector<std::string>{});
        out.capabilities = j.value(
            "capabilities", std::vector<std::string>{});
        out.provenance = j.value("provenance", "");
        out.subosDir = j.value("subos_dir", "");
        auto s = j.at("selection");
        out.selection.mode = s.value("mode", "") == "named_subos"
            ? mcpp::xlings::runtime::RuntimeSelection::Mode::NamedSubos
            : mcpp::xlings::runtime::RuntimeSelection::Mode::McppDefault;
        out.selection.source = s.value("source", "") == "manifest"
            ? mcpp::xlings::runtime::RuntimeSelection::Source::Manifest
            : mcpp::xlings::runtime::RuntimeSelection::Source::DefaultPolicy;
        out.selection.subosName = s.value("name", "default");
        out.selection.ownerRoot = s.value("owner_root", "");
        if (out.schema == 0 || out.runtimeId.empty()
            || out.contractHash.empty() || out.subosDir.empty())
            return std::unexpected("cached RuntimeBinding is incomplete");
        if (detail::hash_contract(detail::canonical_contract(out))
            != out.contractHash)
            return std::unexpected("cached RuntimeBinding contract hash does not match payload");
        return out;
    } catch (const std::exception& e) {
        return std::unexpected(std::format(
            "cached RuntimeBinding cannot be decoded: {}", e.what()));
    }
}

std::vector<std::pair<std::string, std::string>>
resolve_runtime_environment(
    const RuntimeBinding& binding,
    const std::function<std::optional<std::string>(std::string_view)>& ambient = {}) {
    mcpp::xlings::subos::Info info;
    info.present = true;
    mcpp::xlings::subos::Provider snapshot;
    snapshot.binding = "runtime-binding-snapshot";
    snapshot.decls = binding.environment;
    info.providers.push_back(std::move(snapshot));
    return mcpp::xlings::subos::resolve_env(info, binding.subosDir, ambient);
}

} // namespace mcpp::platform::runtime
