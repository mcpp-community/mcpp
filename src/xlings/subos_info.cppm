// mcpp.xlings.subos_info — read the `subos_info` block xlings writes into a
// subos's own `.xlings.json`.
//
// WHAT THIS IS FOR
//
// A program needs three things to run: bootstrap (PT_INTERP + CRT + libc),
// discovery (PATH + RPATH), and configuration (env vars). xlings had the
// first two — glibc + elfpatch, xvm + shims — and until it grew this block,
// nothing for the third. Its own module comment names the consequence by
// number: mcpp#352, a GLFW binary that links fine and exits 255 because
// nothing told it where the GL drivers are.
//
// mcpp is the consumer of the third one. A program mcpp launches gets the
// environment its subos declares, so when the ecosystem gains a Vulkan loader
// or a new driver bridge, this file does not change — the declaration does.
// That is the whole point of reading rather than knowing: mcpp must never
// contain the string "LIBGL_DRIVERS_PATH", because the moment it does, the
// graphics stack has two owners.
//
// SCOPE: read and resolve. This module never WRITES the block and never
// manages subos lifecycle — that is xlings's layer, and mcpp reaching into it
// is the layering inversion the three-tier ecosystem design calls out by name
// ("recipe 里塞 build 逻辑、mcpp 反过来管 subos 状态" — both symptoms of the
// same confusion).
//
// A NOTE ON SILENCE
//
// Every degradation here fills `note`, and callers are required to print it.
// "It did not happen" and "it succeeded" producing identical output is the
// property that made #352 expensive, and a subos with no block is not an
// exotic case: mcpp's own sandbox subos was measured in exactly that state,
// with 356 workspace entries and no self-description at all.
//
// Design: .agents/docs/2026-08-07-xlings-as-runtime-substrate-design.md §3-S3

export module mcpp.xlings.subos_info;

import std;
import mcpp.libs.json;
import mcpp.platform;

export namespace mcpp::xlings::subos {

// The schema this build understands. A HIGHER one on disk is still read — we
// take the fields we know and say so. Refusing outright would let a newer
// xlings break an older mcpp, which is the failure shape the index-floor
// incident already paid for once: publishing data must not invalidate the
// program that reads it.
inline constexpr int kSupportedSchema = 1;

inline constexpr std::string_view kBlock = "subos_info";

struct EnvDecl {
    std::string var;
    std::string op;      // "set" | "prepend"
    std::string value;   // may contain ${subosdir}
};

struct Provider {
    std::string          binding;   // "<name>@<version>"
    std::vector<EnvDecl> decls;
};

struct PackageIdentity {
    std::string namespace_;
    std::string name;
    std::string version;
    std::string source;
};

struct CapabilityProvider {
    std::string     capability;
    PackageIdentity provider;
};

struct RuntimeArtifact {
    std::string           role;
    PackageIdentity       provider;
    std::filesystem::path path;
    std::string           provenance;
    std::string           abi;
    std::string           digest;
    std::string           hostFingerprint;
};

struct Info {
    int                   schema = 0;
    std::string           runtime;    // "glibc@2.39"
    std::string           hostGlibc;  // optional creation-host floor, e.g. "2.43"
    std::vector<Provider> providers;  // sorted by binding
    // Optional generic contract exported by xlings after it resolves provider
    // recipes.  mcpp interprets these facts and never performs provider/hardware
    // discovery itself.
    std::vector<CapabilityProvider> runtimeProviders;
    std::vector<RuntimeArtifact>    runtimeArtifacts;
    bool                  present = false;
    // Non-empty ⇒ the caller MUST surface it. Never an error: a missing or
    // newer block degrades the experience; it does not invalidate a build.
    std::string           note;
};

// The runtime string is self-describing: "glibc@2.39" says Linux/glibc
// without a second field that could disagree with it.
//
// Mirrors xlings's `subos::manifest::family_of`. Two implementations of one
// mapping is a cost, and the alternative — asking the xlings binary — costs
// a subprocess in the build's hot path and fails on exactly the machines
// where it matters (a sandbox xlings that has not been updated). The mapping
// is five rows and stable, and test_subos_info.cpp pins every one of them, so
// a drift is a test failure rather than a silent ABI disagreement.
std::string family_of(std::string_view runtime,
                      std::string_view arch = "x86_64") {
    const auto at   = runtime.find('@');
    const auto name = runtime.substr(0, at == std::string_view::npos
                                            ? runtime.size() : at);
    if (name == "glibc")     return std::format("linux-{}-glibc", arch);
    if (name == "musl")      return std::format("linux-{}-musl", arch);
    if (name == "wasi-libc") return "wasm32-wasi";
    if (name == "macos_sdk") return std::format("darwin-{}", arch);
    if (name == "ucrt")      return std::format("windows-{}-ucrt", arch);
    return "unknown";
}

Info read(const std::filesystem::path& subosDir) {
    Info info;
    auto path = subosDir / ".xlings.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        info.note = std::format(
            "subos '{}' has no .xlings.json, so it cannot say which runtime it "
            "is or what environment its programs need",
            subosDir.string());
        return info;
    }

    std::ifstream is(path);
    auto doc = nlohmann::json::parse(is, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        info.note = std::format("subos manifest {} is not readable JSON",
                                path.string());
        return info;
    }

    auto it = doc.find(std::string(kBlock));
    if (it == doc.end() || !it->is_object()) {
        info.note = std::format(
            "subos '{}' does not describe itself (no `{}` block), so programs "
            "run from here get no environment it declares — a GL application "
            "will not find its drivers. A newer xlings writes this block; "
            "`xlings self update` adds it",
            subosDir.string(), kBlock);
        return info;
    }

    info.present = true;
    if (auto v = it->find("schema_version");
        v != it->end() && v->is_number_integer())
        info.schema = v->get<int>();
    if (auto v = it->find("runtime"); v != it->end() && v->is_string())
        info.runtime = v->get<std::string>();
    if (auto v = it->find("host_glibc"); v != it->end() && v->is_string())
        info.hostGlibc = v->get<std::string>();

    // `envs` is an OBJECT keyed by binding, whose values are arrays of
    // declarations:
    //
    //   "envs": { "mesa@25.0.7.1": [ {"var":…,"op":…,"value":…}, … ], … }
    //
    // Transcribed from xlings's own reader (core/subos/manifest.cppm), not
    // from a model of it. The first version of this file expected an array of
    // {binding, decls} objects — a shape xlings never writes — and its tests
    // hand-wrote JSON in that same invented shape, so both agreed and both
    // were wrong. Against a real subos the loop simply never ran and every
    // variable came back unset, silently. That is why the fixture below is a
    // verbatim capture of real output rather than something composed here.
    if (auto envs = it->find("envs"); envs != it->end() && envs->is_object()) {
        for (auto e = envs->begin(); e != envs->end(); ++e) {
            if (!e.value().is_array()) continue;
            Provider prov;
            prov.binding = e.key();
            for (auto const& d : e.value()) {
                if (!d.is_object()) continue;
                EnvDecl decl;
                if (auto x = d.find("var");   x != d.end() && x->is_string())
                    decl.var = x->get<std::string>();
                if (auto x = d.find("op");    x != d.end() && x->is_string())
                    decl.op = x->get<std::string>();
                if (auto x = d.find("value"); x != d.end() && x->is_string())
                    decl.value = x->get<std::string>();
                // xlings drops a declaration whose var is empty or whose op is
                // neither "set" nor "prepend". Matched exactly: a reader that
                // is more permissive than the writer will one day apply
                // something the writer considers malformed.
                if (decl.var.empty()) continue;
                if (decl.op != "set" && decl.op != "prepend") continue;
                prov.decls.push_back(std::move(decl));
            }
            info.providers.push_back(std::move(prov));
        }
    }

    // Generic runtime provider/artifact contract.  This block is additive to
    // schema 1 so older SubOS manifests remain valid and simply expose no
    // provider provenance.  All identities are structured on the wire; a bare
    // display name would reintroduce cross-namespace collisions.
    if (auto contract = it->find("runtime_contract");
        contract != it->end() && contract->is_object()) {
        auto read_identity = [](const nlohmann::json& value) {
            PackageIdentity id;
            if (!value.is_object()) return id;
            id.namespace_ = value.value("namespace", "");
            id.name = value.value("name", "");
            id.version = value.value("version", "");
            id.source = value.value("source", "");
            return id;
        };
        auto append_note = [&](std::string message) {
            if (!info.note.empty()) info.note += "\n";
            info.note += std::move(message);
        };
        if (auto providers = contract->find("providers");
            providers != contract->end() && providers->is_array()) {
            for (auto const& value : *providers) {
                if (!value.is_object()) continue;
                CapabilityProvider provider;
                provider.capability = value.value("capability", "");
                if (auto identity = value.find("provider"); identity != value.end())
                    provider.provider = read_identity(*identity);
                if (provider.capability.empty() || provider.provider.name.empty()) {
                    append_note("subos runtime_contract contains an incomplete provider entry");
                    continue;
                }
                info.runtimeProviders.push_back(std::move(provider));
            }
        }
        if (auto artifacts = contract->find("artifacts");
            artifacts != contract->end() && artifacts->is_array()) {
            for (auto const& value : *artifacts) {
                if (!value.is_object()) continue;
                RuntimeArtifact artifact;
                artifact.role = value.value("role", "");
                if (auto identity = value.find("provider"); identity != value.end())
                    artifact.provider = read_identity(*identity);
                auto path = value.value("path", "");
                artifact.provenance = value.value("provenance", "");
                artifact.abi = value.value("abi", "");
                artifact.digest = value.value("digest", "");
                artifact.hostFingerprint = value.value("host_fingerprint", "");
                constexpr std::string_view marker = "${subosdir}";
                for (auto pos = path.find(marker); pos != std::string::npos;
                     pos = path.find(marker, pos + subosDir.string().size()))
                    path.replace(pos, marker.size(), subosDir.string());
                artifact.path = path;
                if (artifact.path.is_relative()) artifact.path = subosDir / artifact.path;
                artifact.path = artifact.path.lexically_normal();
                if (artifact.role.empty() || artifact.provider.name.empty()
                    || path.empty() || artifact.provenance.empty()) {
                    append_note("subos runtime_contract contains an incomplete artifact entry");
                    continue;
                }
                info.runtimeArtifacts.push_back(std::move(artifact));
            }
        }
        std::ranges::sort(info.runtimeProviders, {}, [](auto const& value) {
            return std::tuple{value.capability, value.provider.namespace_,
                              value.provider.name, value.provider.version,
                              value.provider.source};
        });
        std::ranges::sort(info.runtimeArtifacts, {}, [](auto const& value) {
            return std::tuple{value.role, value.provider.namespace_,
                              value.provider.name, value.provider.version,
                              value.provider.source, value.path.generic_string(),
                              value.provenance, value.abi, value.digest,
                              value.hostFingerprint};
        });
    }

    // Sorted by binding, matching xlings's own ordering, so two reads of one
    // subos produce the same environment in the same order. Ordering is not
    // cosmetic here: it decides which provider wins a list variable, and
    // libglvnd resolves GL vendors by exactly that order.
    std::sort(info.providers.begin(), info.providers.end(),
              [](Provider const& a, Provider const& b) {
                  return a.binding < b.binding;
              });

    if (info.schema > kSupportedSchema) {
        if (!info.note.empty()) info.note += "\n";
        info.note += std::format(
            "subos '{}' declares schema {}, newer than the {} this mcpp "
            "understands; reading the fields it knows and ignoring the rest",
            subosDir.string(), info.schema, kSupportedSchema);
    }
    return info;
}

// Resolve the declarations into concrete (var, value) pairs with
// `${subosdir}` expanded.
//
// `prepend` joins in provider order and de-duplicates; `set` replaces. That
// is xlings's own precedence, and the de-duplication matters because these
// variables are inherited: without it a nested invocation grows the list
// every time.
//
// The separator is the PLATFORM's, never a literal ':'. Hardcoding one is a
// mistake this repository has made before and it is not cosmetic: on Windows
// the list separator is ';' and ':' appears INSIDE every absolute path, so a
// ':'-keyed split cuts "C:\\x" into "C" and "\\x" -- the de-duplication then
// never matches and the joined value is a corrupt list. Caught by CI on
// Windows, not by any amount of reading.
std::vector<std::pair<std::string, std::string>>
resolve_env(const Info& info, const std::filesystem::path& subosDir,
            const std::function<std::optional<std::string>(std::string_view)>&
                ambient = {}) {
    std::vector<std::pair<std::string, std::string>> out;

    // What the caller's environment already says about a variable.
    //
    // The declarations are merged against this, not in a vacuum, because the
    // result REPLACES the variable in the child (it goes in as extraEnv). A
    // resolution that ignores the ambient value silently discards it.
    auto ambient_of = [&](std::string_view var) -> std::optional<std::string> {
        if (!ambient) return std::nullopt;
        return ambient(var);
    };

    const std::string subos = subosDir.string();
    auto expand = [&](std::string v) {
        constexpr std::string_view kPh = "${subosdir}";
        for (auto pos = v.find(kPh); pos != std::string::npos;
             pos = v.find(kPh, pos + subos.size()))
            v.replace(pos, kPh.size(), subos);
        return v;
    };

    const std::string sep = mcpp::platform::env::path_list_separator();

    // Does the list already contain `value` as a WHOLE element? Compared
    // element-wise rather than by substring: a plain `find` would consider
    // "/a/bc" already present in "/a/bcd".
    auto contains_element = [&](std::string_view list, std::string_view value) {
        for (std::size_t i = 0; i <= list.size();) {
            auto end = list.find(sep, i);
            auto piece = list.substr(i, end == std::string_view::npos
                                            ? std::string_view::npos : end - i);
            if (piece == value) return true;
            if (end == std::string_view::npos) break;
            i = end + sep.size();
        }
        return false;
    };

    // An explicit loop rather than std::ranges::find with a member-pointer
    // projection into std::pair. The projection form reads well and crashed
    // the clang 20.1.7 frontend outright when this module was compiled for
    // the MSVC target -- a segfault with no diagnostic beyond "clang frontend
    // command failed due to signal". Nothing here needs the fancier spelling.
    for (auto const& p : info.providers) {
        for (auto const& d : p.decls) {
            auto value = expand(d.value);
            std::pair<std::string, std::string>* hit = nullptr;
            for (auto& kv : out)
                if (kv.first == d.var) { hit = &kv; break; }
            if (!hit) {
                auto amb = ambient_of(d.var);
                if (d.op == "set") {
                    // xlings's presence semantics: `set` fills an absent
                    // variable, but any caller-provided value wins.  An
                    // explicitly empty value is PRESENT and must stay empty.
                    out.emplace_back(d.var, amb ? *amb : value);
                    continue;
                }
                // `prepend` against the ambient value, not instead of it.
                // These entries replace the variable in the child, so
                // emitting the declared value alone DROPS whatever the caller
                // had -- for a PATH-shaped variable that is the user's whole
                // search path.
                if (amb && !amb->empty() && !contains_element(*amb, value))
                    out.emplace_back(d.var, value + sep + *amb);
                else if (amb && !amb->empty())
                    out.emplace_back(d.var, *amb);
                else
                    out.emplace_back(d.var, value);
                continue;
            }
            if (d.op == "set") {
                auto amb = ambient_of(d.var);
                hit->second = amb ? *amb : value;
                continue;
            }
            if (!contains_element(hit->second, value))
                hit->second = value + sep + hit->second;
        }
    }
    return out;
}

}  // namespace mcpp::xlings::subos
