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
import mcpp.platform.xlings.runtime_selection;
import mcpp.platform.xlings.subos_info;

export namespace mcpp::platform::runtime {

struct RuntimeBinding {
    int schema = 0;
    std::string providerId;
    std::string platform;
    std::string arch;
    // Provider-native runtime identity (`glibc@...`, `macos_sdk@...`,
    // `ucrt@...`). Only Linux additionally projects this into `libc`.
    //
    // THE PROVIDERS ARE NOT ISOMORPHIC, and treating them as such is the
    // mistake this note exists to prevent:
    //
    //   glibc@2.39   binds a PAYLOAD. The headers and the .so are both in it,
    //                and patchelf makes the artifact actually run on that
    //                copy. It is a runtime BINDING.
    //   ucrt@10.0…   declares a FLOOR. `ucrtbase.dll` is an OS component from
    //                Win10 on; it cannot be swapped and should not be
    //                shipped, so mcpp's `windows-sdk` payload deliberately
    //                carries only half of ucrt (headers + import libraries)
    //                and no `Universal CRT Redistributable`. The identity
    //                says which API surface was compiled against, not which
    //                binary will be loaded.
    //
    // Both belong in this field — they are the same QUESTION ("which C
    // runtime is this artifact built for") answered by different providers —
    // but a consumer that assumes the glibc shape will try to resolve a
    // payload that was never supposed to exist. Ask `runtime_provider()`
    // before acting on the value.
    std::string runtimeId;
    std::string contractHash;
    std::optional<std::filesystem::path> loader;
    std::optional<std::string> libc;
    std::optional<std::string> hostLibc;
    // IMMUTABLE payload directories (`<store>/xim-x-glibc/2.39/lib64`).
    std::vector<std::filesystem::path> libraryDirs;
    // The SubOS symlink farm (`<subos>/lib`) — a union view of everything
    // installed into this environment, rewritten on every re-resolution.
    //
    // Deliberately a SECOND field rather than more entries in `libraryDirs`:
    // merging them discards the immutability distinction, and that
    // distinction is the whole of `mcpp.platform.runtime_search`'s ordering
    // rule. A payload directory and a farm directory are not interchangeable
    // even when they currently resolve to the same file.
    std::vector<std::filesystem::path> searchDirs;
    std::vector<mcpp::xlings::subos::EnvDecl> environment;
    std::vector<std::string> providerBindings;
    std::vector<std::string> capabilities;
    std::vector<mcpp::xlings::subos::CapabilityProvider> runtimeProviders;
    std::vector<mcpp::xlings::subos::RuntimeArtifact> runtimeArtifacts;
    std::string provenance;
    std::filesystem::path subosDir;
    mcpp::xlings::runtime::RuntimeSelection selection;

    // Did the SubOS describe itself (does it carry a `subos_info` block)?
    //
    // FALSE IS NOT AN ERROR. A SubOS that says nothing leaves some facts
    // unknown — rules A/B become inconclusive, declared environment is
    // unavailable — and leaves everything else working. Treating absence as a
    // failure is what stopped every `mcpp build` and `mcpp test` on Windows
    // (openxlings/xlings#543), on a machine where the missing facts describe
    // concepts (ELF, PT_INTERP, a private libc) that do not exist there.
    //
    // A CONTRADICTION still fails: naming a SubOS that is not present cannot
    // be satisfied, so it is reported rather than degraded.
    bool declared = false;

    // Why something degraded. Non-empty ⇒ the caller MUST surface it. Never
    // an error: "it did not happen" and "it succeeded" producing identical
    // output is the property that made mcpp#352 expensive.
    std::string note;

    // Does this artifact run under a PRIVATE loader?
    //
    // The predicate the closure resolver needs: when PT_INTERP points into a
    // payload, the HOST loader's built-in default directories are not part of
    // the search path, and modelling them is how a binary that cannot start
    // was reported as valid.
    bool hermetic() const { return loader.has_value(); }
};

// The PROVIDER half of a runtime identity: "glibc" from "glibc@2.39", "ucrt"
// from "ucrt@10.0.26100.0". Empty when there is no identity at all.
//
// Exists so consumers DISPATCH instead of pattern-matching. Every reader of
// `runtimeId` used to spell `starts_with("glibc@")`, which reads as "is this
// glibc" and behaves as "is this the only provider I have ever seen" — so the
// day a second provider appeared, each of those sites silently classified it
// as "no runtime identity" rather than "an identity with no rules here". The
// two are not the same thing, and only one of them is worth reporting.
std::string_view runtime_provider(std::string_view runtimeId) {
    auto at = runtimeId.find('@');
    if (at == std::string_view::npos) return {};
    return runtimeId.substr(0, at);
}

// Attach the Windows C runtime identity to an already-resolved snapshot, and
// re-derive the contract hash so it takes effect.
//
// WHY IT IS A SECOND STEP rather than part of resolve_runtime_binding(): the
// SDK version is a property of the TOOLCHAIN, and the binding is deliberately
// resolved BEFORE any toolchain — the post-install fixup is itself a consumer
// of the binding, so resolving a toolchain first would let directory order
// choose a libc and only then discover what the project selected (#392).
// Windows is the one platform where a fact flows the other way, so it flows
// back explicitly instead of reordering the two.
//
// WHAT IT BUYS: the SDK version enters `contractHash`, which is part of the
// toolchain fingerprint, which keys the build cache. Two SDKs produce two
// caches. Without this the version axis simply stopped existing one layer
// below the compiler — the exact defect this identity was added to close.
//
// Idempotent, and a no-op for an empty version: a Windows box with no SDK
// found still builds (selection UX must work there), it just has nothing to
// declare.
void bind_windows_ucrt(RuntimeBinding& binding, std::string_view sdkVersion);

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
    append_field(out, binding.hostLibc.value_or(""));
    for (auto const& p : binding.libraryDirs)
        append_field(out, p.generic_string());
    // The farm participates in the hash because it participates in the
    // artifact: it lands in DT_RPATH, so a build made against one farm is not
    // interchangeable with a build made against another. `declared` is in for
    // the same reason — a SubOS that gains self-description changes what the
    // build knows, and the fast path must not reuse the older answer.
    append_field(out, binding.declared ? "declared" : "undeclared");
    for (auto const& p : binding.searchDirs)
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
    for (auto const& provider : binding.runtimeProviders) {
        append_field(out, provider.capability);
        append_field(out, provider.provider.namespace_);
        append_field(out, provider.provider.name);
        append_field(out, provider.provider.version);
        append_field(out, provider.provider.source);
    }
    for (auto const& artifact : binding.runtimeArtifacts) {
        append_field(out, artifact.role);
        append_field(out, artifact.provider.namespace_);
        append_field(out, artifact.provider.name);
        append_field(out, artifact.provider.version);
        append_field(out, artifact.provider.source);
        append_field(out, artifact.path.generic_string());
        append_field(out, artifact.provenance);
        append_field(out, artifact.abi);
        append_field(out, artifact.digest);
        append_field(out, artifact.hostFingerprint);
    }
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

// `subos_info.runtime` is the provider's declared identity, while the SubOS
// view is the provider's resolved result.  Older xlings states can retain the
// declaration after a package transaction has atomically repointed the view
// (observed in CI as runtime=glibc@2.39 with libc.so.6 resolving to the managed
// 2.44 payload).  When the physical target can be proven to be one exact
// provider payload, use that immutable identity.  This is not directory
// discovery: the selected SubOS link supplies the one path, and mcpp merely
// canonicalizes its provenance.
std::optional<std::string> managed_glibc_identity(
    const std::filesystem::path& libraryDir,
    const std::filesystem::path& xlingsRoot) {
    std::error_code bec, lec;
    auto base = std::filesystem::weakly_canonical(
        xlingsRoot / "data" / "xpkgs" / "xim-x-glibc", bec);
    auto real = std::filesystem::weakly_canonical(libraryDir, lec);
    if (bec || lec || base.empty() || real.empty()) return std::nullopt;

    auto relative = real.lexically_relative(base);
    if (relative.empty() || relative.is_absolute()) return std::nullopt;
    std::vector<std::string> parts;
    for (auto const& part : relative) parts.push_back(part.string());
    if (parts.size() != 2 || (parts[1] != "lib" && parts[1] != "lib64"))
        return std::nullopt;
    auto const& version = parts[0];
    if (version.empty() || version == "." || version == ".."
        || version.find('@') != std::string::npos
        || version.find('/') != std::string::npos
        || version.find('\\') != std::string::npos)
        return std::nullopt;
    return "glibc@" + version;
}

struct ManagedGlibcPayload {
    std::filesystem::path libraryDir;
    std::filesystem::path loader;
};

std::optional<ManagedGlibcPayload> exact_declared_glibc_payload(
    std::string_view runtimeId,
    const std::filesystem::path& xlingsRoot) {
    constexpr std::string_view prefix = "glibc@";
    if (!runtimeId.starts_with(prefix)) return std::nullopt;
    auto version = runtimeId.substr(prefix.size());
    if (version.empty() || version == "." || version == ".."
        || version.find('@') != std::string_view::npos
        || version.find('/') != std::string_view::npos
        || version.find('\\') != std::string_view::npos)
        return std::nullopt;

    auto payload = xlingsRoot / "data" / "xpkgs" / "xim-x-glibc"
                 / std::string(version);
    for (auto const& leaf : {"lib64", "lib"}) {
        auto lib = payload / leaf;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(lib / "libc.so.6", ec))
            continue;
        std::vector<std::filesystem::path> loaders;
        ec.clear();
        for (auto it = std::filesystem::directory_iterator(lib, ec);
             !ec && it != std::filesystem::directory_iterator{};
             it.increment(ec)) {
            auto name = it->path().filename().string();
            if (name.starts_with("ld-linux-") && name.find(".so") != std::string::npos
                && it->is_regular_file(ec))
                loaders.push_back(it->path());
        }
        std::sort(loaders.begin(), loaders.end());
        if (loaders.size() == 1)
            return ManagedGlibcPayload{lib, loaders.front()};
    }
    return std::nullopt;
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

    // CONTRADICTION vs ABSENCE. The check above is a contradiction: the user
    // named a SubOS that is not there, and no amount of degrading makes that
    // request satisfiable. Everything below is absence — some facts are
    // unavailable, the rest of the build is unaffected — so it degrades.
    //
    // The distinction is not academic. Collapsing it is what made every
    // `mcpp build` and `mcpp test` on Windows fail with a message about GL
    // drivers (openxlings/xlings#543), and it is the same shape as the index
    // floor incident: DATA THAT IS MISSING OR NEWER MUST NOT INVALIDATE THE
    // PROGRAM THAT READS IT.
    auto note = [&](std::string message) {
        if (!out.note.empty()) out.note += "\n";
        out.note += std::move(message);
    };

    auto info = mcpp::xlings::subos::read(out.subosDir);
    out.declared = info.present;
    if (!info.present) {
        note(std::format(
            "SubOS '{}' does not describe itself: {}\n"
            "       Runtime facts (identity, loader, declared environment) are "
            "unavailable: runtime rules report `inconclusive` rather than a "
            "verdict, and a program launched from here gets no environment this "
            "SubOS declares.\n"
            "       Where the C runtime comes from a payload, there is now no "
            "declared runtime to bind to — mcpp declines to guess a version, so "
            "the link falls back to the host and the hermeticity check will say "
            "so. Run `xlings self doctor --fix` from inside this SubOS to have "
            "it described (openxlings/xlings#547).",
            selection.subosName,
            info.note.empty() ? "no `subos_info` block" : info.note));
    } else if (info.schema > mcpp::xlings::subos::kSupportedSchema) {
        // Mirrors `subos_info::read`, which already reads a HIGHER schema and
        // says so. A consumer stricter than its own reader is a time bomb:
        // the day xlings writes schema 2, an equality check stops every build
        // on every platform.
        note(std::format(
            "SubOS '{}' declares runtime contract schema {}, newer than the {} "
            "this mcpp understands; using the fields it knows",
            selection.subosName, info.schema,
            mcpp::xlings::subos::kSupportedSchema));
    }
    if (info.present && info.runtime.empty()) {
        note(std::format(
            "SubOS '{}' has no runtime identity in subos_info.runtime; runtime "
            "rules cannot be evaluated for artifacts built here",
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
    if constexpr (mcpp::platform::is_linux) {
        out.libc = info.runtime;
        if (!info.hostGlibc.empty()) out.hostLibc = info.hostGlibc;

        // ONE traversal, TWO answers.
        //
        //   searchDirs   the view directory itself — the farm, where every
        //                library this environment installed is reachable by
        //                SONAME (`-lGL` already resolves here, because
        //                `--sysroot=<subos>` makes it the linker's default).
        //   libraryDirs  the immutable payload the view's libc RESOLVES to.
        //
        // Deriving both here rather than in two places is the point: the
        // layout knowledge (`lib64` before `lib`) exists exactly once.
        //
        // The view already embodies RuntimeSelection, so following these exact
        // links is not payload discovery and cannot choose another installed
        // version.
        std::vector<std::filesystem::path> candidates{
            out.subosDir / "lib64", out.subosDir / "lib"};
        for (auto const& candidate : candidates) {
            std::error_code fec;
            if (std::filesystem::is_directory(candidate, fec))
                out.searchDirs.push_back(candidate.lexically_normal());
        }
        for (auto const& candidate : candidates) {
            std::error_code lec;
            auto libc = candidate / "libc.so.6";
            if (!std::filesystem::is_regular_file(libc, lec)) continue;
            auto realLibc = std::filesystem::canonical(libc, lec);
            auto libDir = lec ? candidate.lexically_normal()
                              : realLibc.parent_path();
            out.libraryDirs.push_back(libDir);
            if (auto physical = detail::managed_glibc_identity(
                    libDir, out.subosDir.parent_path().parent_path())) {
                out.runtimeId = *physical;
                out.libc = *physical;
            }

            std::vector<std::filesystem::path> loaders;
            lec.clear();
            for (auto it = std::filesystem::directory_iterator(candidate, lec);
                 !lec && it != std::filesystem::directory_iterator{};
                 it.increment(lec)) {
                auto name = it->path().filename().string();
                if (name.starts_with("ld-linux-") && name.find(".so") != std::string::npos)
                    loaders.push_back(it->path());
            }
            std::sort(loaders.begin(), loaders.end());
            if (loaders.size() == 1) {
                auto realLoader = std::filesystem::canonical(loaders.front(), lec);
                out.loader = lec ? loaders.front().lexically_normal() : realLoader;
            }
            break;
        }
        if (out.libraryDirs.empty()) {
            if (auto exact = detail::exact_declared_glibc_payload(
                    out.runtimeId, out.subosDir.parent_path().parent_path())) {
                out.libraryDirs.push_back(exact->libraryDir);
                out.loader = exact->loader;
            }
        }
    }

    for (auto const& provider : info.providers) {
        out.providerBindings.push_back(provider.binding);
        for (auto const& decl : provider.decls)
            out.environment.push_back(decl);
    }
    out.runtimeProviders = info.runtimeProviders;
    out.runtimeArtifacts = info.runtimeArtifacts;
    for (auto const& provider : out.runtimeProviders)
        out.capabilities.push_back(provider.capability);
    std::sort(out.capabilities.begin(), out.capabilities.end());
    out.capabilities.erase(
        std::unique(out.capabilities.begin(), out.capabilities.end()),
        out.capabilities.end());
    out.contractHash = detail::hash_contract(detail::canonical_contract(out));
    return out;
}

void bind_windows_ucrt(RuntimeBinding& binding, std::string_view sdkVersion) {
    if (sdkVersion.empty()) return;
    auto identity = std::format("ucrt@{}", sdkVersion);
    if (binding.runtimeId == identity) return;   // idempotent
    binding.runtimeId = std::move(identity);
    // Deliberately NOT projected into `libc`: that field is the Linux-only
    // private-libc payload, read by the loader/patchelf machinery, and there
    // is no ucrt payload for it to name. `runtime_binding.cppm`'s own comment
    // on `libc` says "Only Linux additionally projects this" — this is that
    // sentence being true rather than merely written down.
    binding.contractHash =
        detail::hash_contract(detail::canonical_contract(binding));
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
    j["host_libc"] = binding.hostLibc.value_or("");
    j["library_dirs"] = nlohmann::json::array();
    for (auto const& path : binding.libraryDirs)
        j["library_dirs"].push_back(path.generic_string());
    j["declared"] = binding.declared;
    j["note"] = binding.note;
    j["search_dirs"] = nlohmann::json::array();
    for (auto const& path : binding.searchDirs)
        j["search_dirs"].push_back(path.generic_string());
    j["environment"] = nlohmann::json::array();
    for (auto const& decl : binding.environment)
        j["environment"].push_back({
            {"var", decl.var}, {"op", decl.op}, {"value", decl.value}});
    j["provider_bindings"] = binding.providerBindings;
    j["capabilities"] = binding.capabilities;
    j["runtime_providers"] = nlohmann::json::array();
    for (auto const& provider : binding.runtimeProviders) {
        j["runtime_providers"].push_back({
            {"capability", provider.capability},
            {"provider", {
                {"namespace", provider.provider.namespace_},
                {"name", provider.provider.name},
                {"version", provider.provider.version},
                {"source", provider.provider.source},
            }},
        });
    }
    j["runtime_artifacts"] = nlohmann::json::array();
    for (auto const& artifact : binding.runtimeArtifacts) {
        j["runtime_artifacts"].push_back({
            {"role", artifact.role},
            {"provider", {
                {"namespace", artifact.provider.namespace_},
                {"name", artifact.provider.name},
                {"version", artifact.provider.version},
                {"source", artifact.provider.source},
            }},
            {"path", artifact.path.generic_string()},
            {"provenance", artifact.provenance},
            {"abi", artifact.abi},
            {"digest", artifact.digest},
            {"host_fingerprint", artifact.hostFingerprint},
        });
    }
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
        if (auto v = j.value("host_libc", ""); !v.empty()) out.hostLibc = v;
        if (auto it = j.find("library_dirs"); it != j.end() && it->is_array())
            for (auto const& v : *it) if (v.is_string())
                out.libraryDirs.emplace_back(v.get<std::string>());
        out.declared = j.value("declared", false);
        out.note = j.value("note", "");
        if (auto it = j.find("search_dirs"); it != j.end() && it->is_array())
            for (auto const& v : *it) if (v.is_string())
                out.searchDirs.emplace_back(v.get<std::string>());
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
        auto read_identity = [](const nlohmann::json& value) {
            mcpp::xlings::subos::PackageIdentity id;
            if (!value.is_object()) return id;
            id.namespace_ = value.value("namespace", "");
            id.name = value.value("name", "");
            id.version = value.value("version", "");
            id.source = value.value("source", "");
            return id;
        };
        if (auto it = j.find("runtime_providers");
            it != j.end() && it->is_array()) {
            for (auto const& value : *it) {
                if (!value.is_object()) continue;
                mcpp::xlings::subos::CapabilityProvider provider;
                provider.capability = value.value("capability", "");
                if (auto identity = value.find("provider"); identity != value.end())
                    provider.provider = read_identity(*identity);
                if (!provider.capability.empty() && !provider.provider.name.empty())
                    out.runtimeProviders.push_back(std::move(provider));
            }
        }
        if (auto it = j.find("runtime_artifacts");
            it != j.end() && it->is_array()) {
            for (auto const& value : *it) {
                if (!value.is_object()) continue;
                mcpp::xlings::subos::RuntimeArtifact artifact;
                artifact.role = value.value("role", "");
                if (auto identity = value.find("provider"); identity != value.end())
                    artifact.provider = read_identity(*identity);
                artifact.path = value.value("path", "");
                artifact.provenance = value.value("provenance", "");
                artifact.abi = value.value("abi", "");
                artifact.digest = value.value("digest", "");
                artifact.hostFingerprint = value.value("host_fingerprint", "");
                if (!artifact.role.empty() && !artifact.provider.name.empty()
                    && !artifact.path.empty() && !artifact.provenance.empty())
                    out.runtimeArtifacts.push_back(std::move(artifact));
            }
        }
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
        // Completeness is conditional on `declared`. An UNDECLARED binding
        // legitimately has schema 0 and no runtime identity — that is what
        // "the SubOS said nothing" looks like — so demanding those fields
        // would make every cached degraded binding undecodable and send the
        // build back down the slow path forever. The hash still has to match,
        // which is what actually proves the record was not tampered with.
        if (out.contractHash.empty() || out.subosDir.empty())
            return std::unexpected("cached RuntimeBinding is incomplete");
        if (out.declared && (out.schema == 0 || out.runtimeId.empty()))
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
