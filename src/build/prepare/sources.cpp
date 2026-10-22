// sources.cpp -- where each thing a build uses comes from (mcpp#755): the
// decision record, payload overrides, on-request provisioning, the payload
// facts a build program receives, and `--managed-only`. Declared in `:state`.
//
// ONE RECORD, MANY READERS. A source is decided once, here, and the status
// line, the `Finished` summary, `mcpp why`, `resolution.json` and the
// `--managed-only` refusal all read the same entry. Each of them used to be a
// place where the answer could be re-derived and drift, which is the shape
// this file exists to remove.

module;
#include <cstdlib>

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.refusal;
import mcpp.build.build_program;
import mcpp.config;
import mcpp.diag;
import mcpp.wire;
import mcpp.manifest;
import mcpp.platform;
import mcpp.ui;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.provider.address_set;

namespace mcpp::build {

namespace fs = std::filesystem;
namespace addrset = mcpp::provider::addrset;

namespace {

// `mcpp.toml:22`, relative to the project when the file is inside it.
std::string origin_place(const SourceDecision& d, const fs::path& relativeTo) {
    if (d.originFile.empty()) return d.originKey;
    fs::path f(d.originFile);
    std::string shown = f.generic_string();
    if (!relativeTo.empty()) {
        std::error_code ec;
        auto rel = fs::relative(f, relativeTo, ec);
        if (!ec && !rel.empty() && !rel.generic_string().starts_with(".."))
            shown = rel.generic_string();
    }
    const char* home = std::getenv(mcpp::platform::is_windows ? "USERPROFILE" : "HOME");
    if (home && *home) {
        const auto h = fs::path(home).generic_string();
        if (shown.starts_with(h + "/")) shown = "~" + shown.substr(h.size());
    }
    return d.originLine > 0 ? std::format("{}:{}", shown, d.originLine) : shown;
}

// The thing a `Using` line names, by the subject's kind.
std::string subject_shown(const SourceDecision& d) {
    if (d.subject.starts_with("payload:")) return d.subject.substr(8);
    if (d.subject.starts_with("tool:")) {
        // `tool:<module>:<name>` -> `<name> (<module>)`
        const auto rest = std::string_view(d.subject).substr(5);
        const auto colon = rest.rfind(':');
        if (colon == std::string_view::npos) return std::string(rest);
        return std::format("{} ({})", rest.substr(colon + 1), rest.substr(0, colon));
    }
    if (d.subject == "toolchain.build") return "toolchain";
    if (d.subject == "toolchain.bootstrap") return "bootstrap toolchain";
    return d.subject;
}

// `MCPP_XLINGS_OVERRIDE_<NS>_<NAME>`, sanitised as `xpkg_dir`'s variables are.
std::string override_env_var(std::string_view key) {
    std::string out = "MCPP_XLINGS_OVERRIDE_";
    for (char c : key)
        out += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
             : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    return out;
}

// The root a program implies: its directory, or that directory's parent when
// it is a `bin/` -- so `<root>/bin/<program>`, the layout every payload
// plugin already reads, holds for `/usr/bin/cmake` as it does for a payload.
fs::path root_of_program(const fs::path& program) {
    auto dir = program.parent_path();
    if (dir.filename() == "bin") return dir.parent_path();
    return dir;
}

} // namespace

bool names_a_file_path(std::string_view v) {
    return v.contains('/') || v.contains('\\') || v.starts_with(".")
        || (v.size() > 1 && v[1] == ':');
}

std::string source_tag(const SourceDecision& d, const fs::path& relativeTo) {
    const auto cls = std::string(source_class_name(d.cls));
    if (d.originKind == "env")
        return std::format("{} · env {}", cls, d.originKey);
    const auto place = origin_place(d, relativeTo);
    if (place.empty()) return cls;
    return std::format("{} · {}", cls, place);
}

void record_source(PrepareState& state, SourceDecision d) {
    const bool announce = d.announce && !source_class_is_default(d.cls)
        && state.announcedSources.insert(d.subject + "\x1f" + d.value).second;
    if (announce) {
        const auto root = state.root ? *state.root : fs::path{};
        const auto what = d.detail.empty() ? subject_shown(d)
                                           : subject_shown(d) + " " + d.detail;
        mcpp::ui::source("Using", std::format("{} ← {}", what, d.value),
                         source_tag(d, root), d.cls == SourceClass::Host);
    }
    // A PREDICATE, NOT A PROJECTION BY POINTER-TO-MEMBER. The latter into a
    // type this module imports makes clang 20.1.7 crash while generating code,
    // and the report names an unrelated function (measured on windows-2022,
    // 2026-10-01; `mcpp.toolchain.model`'s `ToolOverride` records the same
    // hazard for an exported `std::pair`).
    auto by_subject = [](std::string_view subject) {
        return [subject](const SourceDecision& o) { return o.subject == subject; };
    };
    auto it = std::ranges::find_if(state.sources, by_subject(d.subject));
    if (it == state.sources.end()) state.sources.push_back(std::move(d));
    else *it = std::move(d);
}

std::expected<const PrepareState::PayloadOverride*, std::string>
payload_override(PrepareState& state, std::string_view key) {
    if (auto it = state.payloadOverrideCache.find(std::string(key));
        it != state.payloadOverrideCache.end())
        return it->second ? &*it->second : nullptr;

    // Three places may state it, nearest first. The environment outranks the
    // manifest so that CI and packaging can override without editing it, and
    // says so on the `Using` line; the manifest outranks the machine's
    // configuration because a project's statement is the more specific one.
    struct Stated {
        std::string kind = "path", value, version, originKind, originFile, originKey;
        int line = 0;
        fs::path base;   // what a relative path is relative to
    };
    std::optional<Stated> stated;
    const auto var = override_env_var(key);
    if (const char* v = std::getenv(var.c_str()); v && *v) {
        std::string value(v);
        Stated s{.originKind = "env", .originKey = var, .base = fs::current_path()};
        if (value.starts_with("path:")) { s.kind = "program"; value = value.substr(5); }
        s.value = std::move(value);
        stated = std::move(s);
    }
    auto from_manifest = [&](const mcpp::manifest::Manifest* man) {
        if (stated || !man) return;
        auto it = man->xlings.overrides.find(std::string(key));
        if (it == man->xlings.overrides.end()) return;
        const auto& o = it->second;
        Stated s;
        s.kind = o.kind == mcpp::manifest::XlingsOverride::Kind::Program ? "program"
               : o.kind == mcpp::manifest::XlingsOverride::Kind::Root    ? "root" : "path";
        s.value = o.value; s.version = o.version; s.line = o.line;
        s.originKind = "manifest";
        s.originFile = man->sourcePath.string();
        s.originKey  = "[xlings.overrides]";
        s.base = man->sourcePath.parent_path();
        stated = std::move(s);
    };
    from_manifest(state.m ? &*state.m : nullptr);
    from_manifest(state.runtimeOwnerManifest);
    if (!stated) {
        if (auto cfg = state.get_cfg(true)) {
            if (auto it = (*cfg)->payloadOverrides.find(std::string(key));
                it != (*cfg)->payloadOverrides.end()) {
                Stated s;
                s.kind = it->second.kind; s.value = it->second.value;
                s.version = it->second.version; s.line = it->second.line;
                s.originKind = "config";
                s.originFile = (*cfg)->configFile.string();
                s.originKey  = "[xlings.overrides] in config.toml";
                s.base = (*cfg)->configFile.parent_path();
                stated = std::move(s);
            }
        }
    }
    if (!stated) {
        state.payloadOverrideCache.emplace(std::string(key), std::nullopt);
        return nullptr;
    }

    const auto where = stated->originKind == "env" ? stated->originKey
        : stated->line > 0 ? std::format("{} ({}:{})", stated->originKey,
                                         stated->originFile, stated->line)
                           : std::format("{} ({})", stated->originKey, stated->originFile);
    auto refuse = [&](std::string why) -> std::unexpected<std::string> {
        refusal::record(refusal::Code::PayloadOverride);
        return std::unexpected(std::format(
            "`{}` is overridden by {}, and {}.\n"
            "       An override names a program (a file, or a name found on PATH)\n"
            "       or a root laid out like the payload; remove the entry to\n"
            "       install the payload instead.", key, where, why));
    };

    PrepareState::PayloadOverride out;
    out.version    = stated->version;
    out.originKind = stated->originKind;
    out.originFile = stated->originFile;
    out.originKey  = stated->originKey;
    out.originLine = stated->line;
    out.cls        = SourceClass::Custom;
    std::error_code ec;
    if (stated->kind == "program" && !names_a_file_path(stated->value)) {
        // A NAME, LOOKED UP ON PATH ONCE, HERE. The answer is the program a
        // later step runs and the one the `Using` line names, so a reader can
        // see which PATH entry served it. Without a stated version it is the
        // one class of source mcpp cannot describe beyond its location.
        auto found = mcpp::platform::fs::which(stated->value);
        if (!found) return refuse(std::format("'{}' is not found on PATH", stated->value));
        out.program = found->generic_string();
        out.root    = root_of_program(*found).generic_string();
        if (out.version.empty()) out.cls = SourceClass::Host;
    } else {
        fs::path p(stated->value);
        if (p.is_relative()) p = stated->base / p;
        p = p.lexically_normal();
        const bool isDir  = fs::is_directory(p, ec);
        const bool isFile = !isDir && fs::exists(p, ec);
        if (stated->kind == "root" || (stated->kind == "path" && isDir)) {
            if (!isDir) return refuse(std::format("'{}' is not a directory", p.generic_string()));
            out.root = p.generic_string();
        } else {
            // THE EXECUTABLE SUFFIX A HOST APPENDS ITSELF. A shell on Windows
            // answers `C:/Program Files/CMake/bin/cmake` for a `cmake.exe`, and
            // process creation there appends `.exe`, so a path stated without it
            // names a program the machine would run. Refusing it would be an
            // answer about spelling, not about the machine (the same rule the
            // plugin-side resolver applies to a stated path).
            if (!isFile) {
                auto withExe = p;
                withExe += ".exe";
                if (fs::is_regular_file(withExe, ec)) p = withExe;
                else return refuse(std::format("'{}' does not exist", p.generic_string()));
            }
            out.program = p.generic_string();
            out.root    = root_of_program(p).generic_string();
        }
    }
    auto [it, _] = state.payloadOverrideCache.emplace(std::string(key), std::move(out));
    return &*it->second;
}

std::expected<std::vector<std::string>, std::string>
payloads_to_provision(PrepareState& state,
                      const addrset::Resolution& unified,
                      std::span<const addrset::Claim> claims,
                      const std::vector<char>& onRequest) {
    std::vector<std::string> install;
    for (auto const& w : unified.winners) {
        const auto key = addrset::package_key(w.address);
        auto ov = payload_override(state, key);
        if (!ov) return std::unexpected(ov.error());
        if (*ov) {
            const auto& o = **ov;
            const auto statedBy = o.originKind == "env" ? o.originKey : o.originKey;
            if (auto why = addrset::override_violation(claims, key, o.version, statedBy)) {
                refusal::record(refusal::Code::PayloadOverride);
                return std::unexpected(*why);
            }
            if (o.version.empty())
                if (auto reqs = addrset::requirements_for(claims, key); !reqs.empty()) {
                    std::string list;
                    for (auto const& r : reqs) list += (list.empty() ? "" : ", ") + r;
                    mcpp::diag::note("xlings/override-unversioned", std::format(
                        "`{}` is overridden without a stated version, so the requirement "
                        "({}) is not checked; state `version` in the override to have it "
                        "checked", key, list));
                }
            state.xlingsOverridden.insert(key);
            state.xlingsSkipped.insert(w.address);
            SourceDecision d;
            d.subject    = "payload:" + key;
            d.value      = o.program.empty() ? o.root : o.program;
            d.cls        = o.cls;
            d.originKind = o.originKind;
            d.originFile = o.originFile;
            d.originLine = o.originLine;
            d.originKey  = o.originKey;
            d.decidedFor = w.claim < claims.size() ? claims[w.claim].declaredBy : std::string{};
            d.considered.push_back(std::format("payload {} (not installed: overridden)", w.address));
            record_source(state, std::move(d));
            continue;
        }
        // DEFERRED ONLY WHEN EVERY DECLARATION SAYS SO. A package one manifest
        // needs eagerly is installed eagerly, whoever else declared it on
        // request; and one a build program already asked for is installed.
        bool allOnRequest = false;
        for (std::size_t i = 0; i < claims.size(); ++i) {
            if (addrset::package_key(claims[i].address) != key) continue;
            if (i >= onRequest.size() || !onRequest[i]) { allOnRequest = false; break; }
            allOnRequest = true;
        }
        SourceDecision d;
        d.subject    = "payload:" + key;
        d.value      = w.address;
        d.originKind = "graph";
        d.decidedFor = w.claim < claims.size() ? claims[w.claim].declaredBy : std::string{};
        d.cls = (w.claim < claims.size() && claims[w.claim].distance == 0
                 && !addrset::version_of(w.address).empty())
            ? SourceClass::Pinned : SourceClass::Managed;
        if (d.cls == SourceClass::Pinned) d.originKind = "manifest";
        if (allOnRequest && !state.requestedPayloads.contains(key)) {
            state.xlingsDeferred.insert(key);
            state.xlingsSkipped.insert(w.address);
            d.considered.push_back("installed on request; not requested by this build");
            record_source(state, std::move(d));
            continue;
        }
        state.xlingsDeferred.erase(key);
        if (state.requestedPayloads.contains(key))
            d.considered.push_back("installed on request of a build program");
        record_source(state, std::move(d));
        install.push_back(w.address);
    }
    return install;
}

std::optional<std::string> dependency_override_refusal(const PrepareState& state) {
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        const auto& pkg = state.packages[i];
        if (pkg.selectedMember) continue;
        if (pkg.manifest.xlings.overrides.empty()) continue;
        std::string keys;
        for (auto const& [k, o] : pkg.manifest.xlings.overrides)
            keys += (keys.empty() ? "" : ", ") + o.key;
        refusal::record(refusal::Code::PayloadOverride);
        return std::format(
            "`{}` states [xlings.overrides] ({}), and it is a dependency of this build.\n"
            "       Where a payload comes from is stated by the project being built, its\n"
            "       environment (MCPP_XLINGS_OVERRIDE_<NS>_<NAME>) or config.toml; a\n"
            "       package states which payloads it needs, in [xlings.workspace] or\n"
            "       [feature-xlings.<feature>].",
            pkg.manifest.package.name, keys);
    }
    return std::nullopt;
}

void fill_xpkg_env(PrepareState& state, mcpp::build::BuildProgramEnv& e,
                   const mcpp::manifest::Manifest& owner, std::size_t consumer) {
    // `[feature-xlings.<f>]` is provisioned when `<f>` is active, so it has
    // to be answerable here too. The set is taken from the SAME env the caller
    // already computed, so "which features are on" is answered once.
    // Installation stays the filter below: a declared address whose payload is
    // absent answers "", which is what a `when = "dev"` entry looks like to a
    // consumer.
    std::vector<std::string> declared = owner.xlings.deps;
    for (auto const& f : e.features)
        if (auto it = owner.xlings.featureDeps.find(f); it != owner.xlings.featureDeps.end())
            for (auto const& address : it->second)
                if (std::ranges::find(declared, address) == declared.end())
                    declared.push_back(address);
    // …and what the rule packages compiled INTO this build program declared.
    // Their own active features, not the consumer's: the consumer asked for
    // `features = ["rules-cuda"]` on the edge, and that is what decides which
    // of the rule's `[feature-xlings]` tables apply.
    if (auto pit = state.buildProgramLibrariesByConsumer.find(consumer);
        pit != state.buildProgramLibrariesByConsumer.end()) {
        for (auto q : pit->second) {
            if (q >= state.packages.size()) continue;
            auto const& pm = state.packages[q].manifest;
            auto want = [&](const std::string& address) {
                if (std::ranges::find(declared, address) == declared.end())
                    declared.push_back(address);
            };
            for (auto const& address : pm.xlings.deps) want(address);
            const auto& pf = q < state.buildProgramFeaturesByPackage.size()
                ? state.buildProgramFeaturesByPackage[q] : std::vector<std::string>{};
            for (auto const& f : pf)
                if (auto it = pm.xlings.featureDeps.find(f); it != pm.xlings.featureDeps.end())
                    for (auto const& address : it->second) want(address);
        }
    }
    if (declared.empty()) return;
    auto cfg = state.get_cfg(true);
    if (!cfg) return;
    auto xlEnv = mcpp::config::make_xlings_env(**cfg);
    std::set<std::string> answered;
    // Namespaced first -- it is the exact spelling, and the bare form must not
    // shadow it (the receiver keeps the first value it is given for a name).
    auto put = [&](const mcpp::provider::PackageRef& ref, std::string_view suffix,
                   const std::string& value) {
        e.xpkgDirs.emplace_back(mcpp::build::xpkg_env_var(ref.ns, ref.name, suffix), value);
        e.xpkgDirs.emplace_back(mcpp::build::xpkg_env_var("", ref.name, suffix), value);
    };
    for (auto const& raw : declared) {
        // THE VERSION THIS BUILD INSTALLED, NOT THE ONE THIS MANIFEST WROTE.
        // Both statements are about one package, and only one version of it
        // exists on disk; answering from the local spelling is how a rule
        // package could declare `>=8.5.0`, have the project's exact pin
        // installed instead, and then be told nothing is there.
        const auto key = addrset::package_key(raw);
        if (!answered.insert(key).second) continue;
        auto wit = state.xlingsWinner.find(key);
        const std::string spec = wit == state.xlingsWinner.end() ? raw : wit->second;
        auto ref = mcpp::provider::parse_ref(spec);
        // AN OVERRIDE ANSWERS INSTEAD OF THE REGISTRY (mcpp#755): its root as
        // the directory, so a plugin that reads `<dir>/bin/<tool>` needs no
        // change, its program where it named one, and the source.
        if (state.xlingsOverridden.contains(key)) {
            if (auto ov = payload_override(state, key); ov && *ov) {
                put(ref, "DIR", (*ov)->root);
                if (!(*ov)->program.empty()) put(ref, "PROGRAM", (*ov)->program);
                put(ref, "SOURCE", "override");
            }
            continue;
        }
        auto dir = mcpp::provider::payload(*mcpp::provider::open(xlEnv), ref);
        if (dir) {
            put(ref, "DIR", dir->string());
            put(ref, "SOURCE", "payload");
            continue;
        }
        // Declared on request and not installed: the program may ask.
        if (state.xlingsDeferred.contains(key)) put(ref, "SOURCE", "pending");
    }
}

std::expected<bool, std::string>
answer_payload_requests(PrepareState& state, mcpp::manifest::Manifest& m,
                        mcpp::build::BuildProgramEnv& e, std::size_t consumer,
                        std::string_view who) {
    auto requests = std::move(m.buildConfig.xpkgRequests);
    m.buildConfig.xpkgRequests.clear();
    if (requests.empty()) return false;
    std::vector<std::string> addresses, keys;
    for (auto const& r : requests) {
        const auto key = addrset::package_key(r);
        if (std::ranges::find(keys, key) != keys.end()) continue;
        if (!state.xlingsDeferred.contains(key) && !state.requestedPayloads.contains(key)) {
            refusal::record(refusal::Code::PayloadRequest);
            return std::unexpected(std::format(
                "the build program of `{}` asked for `{}`, which no manifest of this build "
                "declares `provision = \"on-request\"` for this build.\n"
                "       A build program asks only for a payload its package (or a host\n"
                "       module compiled into it) declares on request; `xpkg_dir` answers\n"
                "       for one declared without it.", who, key));
        }
        keys.push_back(key);
        auto wit = state.xlingsWinner.find(key);
        addresses.push_back(wit == state.xlingsWinner.end() ? key : wit->second);
    }
    if (state.overrides.plan_only) {
        std::string list;
        for (auto const& a : addresses) list += (list.empty() ? "" : ", ") + a;
        state.planNotes.push_back({"MCPP_BUILD_DATABASE_PAYLOAD_DEFERRED",
            std::format("the build program of `{}` asked for {}, declared `provision = "
                        "\"on-request\"`; planning installs nothing, so the program is "
                        "described without them, and `mcpp build` installs them", who, list),
            mcpp::wire::Severity::Note, {}});
        return false;
    }
    auto cfg = state.get_cfg(true);
    if (!cfg) return std::unexpected(cfg.error());
    if (auto pv = provision_xlings_addresses(
            **cfg, addresses, *state.root,
            std::format("payloads requested by the build program of `{}`", who));
        !pv) return std::unexpected(pv.error());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        state.requestedPayloads.insert(keys[i]);
        state.xlingsDeferred.erase(keys[i]);
        state.xlingsSkipped.erase(addresses[i]);
        const auto subject = "payload:" + keys[i];
        auto it = std::ranges::find_if(state.sources, [&](const SourceDecision& o) {
            return o.subject == subject; });
        if (it != state.sources.end()) {
            it->considered.clear();
            it->considered.push_back(std::format("installed on request of `{}`", who));
        }
    }
    // The program runs again with the directories it asked for.
    std::erase_if(e.xpkgDirs, [](const auto& kv) { return kv.first.starts_with("MCPP_XPKG_"); });
    fill_xpkg_env(state, e, m, consumer);
    return true;
}

std::expected<void, std::string>
run_answering_requests(PrepareState& state, mcpp::manifest::Manifest& m,
                       mcpp::build::BuildProgramEnv& e, std::size_t consumer,
                       std::string_view who,
                       const std::function<std::expected<void, std::string>()>& run) {
    // At most three rounds: a program may learn what it needs only from what
    // it received, but a program that asks for something new every time is a
    // loop, and a named refusal beats an unbounded one.
    e.keepRequestingRun = state.overrides.plan_only;
    for (int round = 0;; ++round) {
        auto r = run();
        if (!r) return r;
        auto again = answer_payload_requests(state, m, e, consumer, who);
        if (!again) return std::unexpected(again.error());
        if (!*again) return {};
        if (round == 2) {
            refusal::record(refusal::Code::PayloadRequest);
            return std::unexpected(std::format(
                "the build program of `{}` asked for payloads in three consecutive runs; "
                "a program asks for every payload it needs in one run, and runs again "
                "with all of them installed", who));
        }
    }
}

void record_tool_decisions(PrepareState& state) {
    auto take = [&](const mcpp::manifest::Manifest& man) {
        for (auto const& line : man.buildConfig.toolDecisions) {
            // `<subject>\t<from>\t<value>\t<file>\t<line>[\t<payload>]`
            std::vector<std::string> f;
            std::size_t at = 0;
            while (true) {
                auto tab = line.find('\t', at);
                f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
                if (tab == std::string::npos) break;
                at = tab + 1;
            }
            if (f.size() < 3 || f[0].empty()) continue;
            SourceDecision d;
            d.subject    = f[0];
            d.value      = f[2];
            d.decidedFor = man.package.name;
            const auto& from = f[1];
            if (from == "choice") {
                d.cls = SourceClass::Program;
                d.originKind = "build-program";
                d.originFile = f.size() > 3 ? f[3] : std::string{};
                d.originLine = f.size() > 4 ? std::atoi(f[4].c_str()) : 0;
                if (d.originFile.empty()) d.originKey = "build.mcpp";
            } else if (from == "env") {
                d.cls = SourceClass::Custom;
                d.originKind = "env";
                d.originKey  = f.size() > 3 && !f[3].empty() ? f[3] : std::string("environment");
            } else if (from == "override") {
                // THE PAYLOAD'S OWN STATEMENT, AND ITS OWN LINE. The override
                // was already reported where the download was skipped, so the
                // tool takes that source and is recorded without a second line.
                const auto payload = f.size() > 5 ? f[5] : std::string{};
                d.payload  = payload;
                d.announce = false;
                const auto psubject = "payload:" + payload;
                auto it = std::ranges::find_if(state.sources, [&](const SourceDecision& o) {
                    return o.subject == psubject; });
                if (it != state.sources.end()) {
                    d.cls = it->cls; d.originKind = it->originKind;
                    d.originFile = it->originFile; d.originLine = it->originLine;
                    d.originKey = it->originKey;
                } else {
                    d.cls = SourceClass::Custom; d.originKind = "env";
                }
            } else if (from == "path") {
                d.cls = SourceClass::Host;
                d.originKind = "host";
                d.originKey  = "PATH";
            } else {
                // The payload: its own entry carries the source, so this one
                // records which member runs it and states no line of its own.
                d.cls = SourceClass::Managed;
                d.originKind = "graph";
                d.announce = false;
                if (f.size() > 5) {
                    d.payload = f[5];
                    const auto psubject = "payload:" + f[5];
                    auto it = std::ranges::find_if(state.sources, [&](const SourceDecision& o) {
                        return o.subject == psubject; });
                    if (it != state.sources.end()) d.cls = it->cls;
                }
            }
            d.considered.push_back(std::format("stated by the build program of `{}` ({})",
                                               man.package.name, from));
            record_source(state, std::move(d));
        }
    };
    if (state.m) take(*state.m);
    for (std::size_t i = 1; i < state.packages.size(); ++i) take(state.packages[i].manifest);
}

std::expected<void, std::string> step13_sources(PrepareState& state, BuildContext& ctx) {
    // THE BUILD TOOLCHAIN, when nothing stated it as a source of its own: a
    // managed payload, and how it was chosen. A toolchain named by path or by
    // the build program was recorded where it was resolved, and announced
    // there, next to the `Resolving toolchain` line.
    if (std::ranges::none_of(state.sources, [](const SourceDecision& d) {
            return d.subject == "toolchain.build"; })) {
        SourceDecision d;
        d.subject = "toolchain.build";
        d.value   = state.tcSpec.value_or(ctx.tc.label());
        d.detail  = ctx.tc.label();
        switch (state.tcOrigin) {
            case TcOrigin::ManifestToolchain:
            case TcOrigin::TargetSection:
                d.cls = SourceClass::Pinned;
                if (state.tcFromCommandLine) {
                    d.originKind = "env"; d.originKey = "MCPP_TOOLCHAIN";
                } else if (state.tcFromConsumer) {
                    d.originKind = "graph"; d.originKey = state.overrides.tool_chain;
                } else {
                    d.originKind = "manifest";
                    d.originFile = state.m->sourcePath.string();
                    d.originLine = state.m->toolchain.line_for(kCurrentPlatform);
                    d.originKey  = state.tcOrigin == TcOrigin::TargetSection
                        ? std::format("[target.{}] toolchain", state.overrides.target_triple)
                        : std::string("[toolchain]");
                }
                break;
            case TcOrigin::GlobalDefault:
                d.cls = SourceClass::Pinned;
                d.originKind = "config";
                d.originKey  = "`mcpp toolchain default`";
                break;
            default:
                d.cls = SourceClass::Managed;
                d.originKind = state.tcOrigin == TcOrigin::GraphRequirement ? "graph" : "default";
                d.originKey  = std::string(tc_origin_name(state.tcOrigin));
                break;
        }
        d.considered.push_back(ctx.tc.binaryPath.generic_string());
        record_source(state, std::move(d));
    }
    record_tool_decisions(state);
    if (auto why = managed_only_refusal(state)) return std::unexpected(*why);
    ctx.sources = state.sources;
    return {};
}

std::optional<std::string> managed_only_refusal(const PrepareState& state) {
    const char* env = std::getenv("MCPP_MANAGED_ONLY");
    const bool on = state.overrides.managed_only || (env && *env && std::string_view(env) != "0");
    if (!on) return std::nullopt;
    std::string list;
    const auto root = state.root ? *state.root : fs::path{};
    for (auto const& d : state.sources) {
        if (source_class_is_default(d.cls)) continue;
        // Where an artifact is executed is not a source of the build.
        if (d.subject == "run_under") continue;
        list += std::format("\n         {} ← {}  [{}]", subject_shown(d), d.value,
                            source_tag(d, root));
    }
    if (list.empty()) return std::nullopt;
    refusal::record(refusal::Code::ManagedOnly);
    return std::format(
        "--managed-only: this build uses sources that are not the ecosystem's:{}\n"
        "       Remove the statements above, or build without --managed-only\n"
        "       (MCPP_MANAGED_ONLY).", list);
}

std::string sources_summary(const std::vector<SourceDecision>& sources) {
    std::map<SourceClass, std::vector<std::string>> byClass;
    for (auto const& d : sources) {
        if (source_class_is_default(d.cls)) continue;
        if (d.subject == "run_under") continue;
        // A tool whose source is its payload's is already summarised by that
        // payload: one statement, one entry.
        if (!d.payload.empty() && std::ranges::any_of(sources, [&](auto const& o) {
                return o.subject == "payload:" + d.payload; }))
            continue;
        byClass[d.cls].push_back(subject_shown(d));
    }
    std::string out;
    for (auto const& [cls, names] : byClass) {
        if (!out.empty()) out += "; ";
        out += std::format("{}: ", source_class_name(cls));
        for (std::size_t i = 0; i < names.size(); ++i)
            out += (i ? ", " : "") + names[i];
    }
    return out;
}

} // namespace mcpp::build
