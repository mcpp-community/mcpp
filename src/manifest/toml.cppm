// mcpp.manifest:toml — load and validate mcpp.toml.

export module mcpp.manifest.toml;

import mcpp.manifest.types;
import std;
import mcpp.source_kind;
import mcpp.libs.toml;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.index_spec;
import mcpp.platform;

export namespace mcpp::manifest {

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin = "mcpp.toml");
std::expected<Manifest, ManifestError> load(const std::filesystem::path& path);

// For `mcpp new` scaffolding.
std::string default_template(std::string_view packageName);

// Shared source-preserving editor used by both `mcpp add` and scaffold
// self-dependency injection. Identity is structured; formatting is emitted in
// the canonical default table / namespace-subtable form.
struct DependencyTextEdit {
    std::string              namespace_;
    std::string              shortName;
    std::string              version;
    std::vector<std::string> features;
    bool                     dev = false;
};

std::expected<std::string, std::string>
upsert_dependency_text(std::string_view source,
                       const DependencyTextEdit& edit);

} // namespace mcpp::manifest

namespace mcpp::manifest {

namespace t = mcpp::libs::toml;

namespace {

ManifestError error(const std::filesystem::path& origin,
                    const std::string& msg,
                    t::Position pos = {0, 0}) {
    return ManifestError{msg, origin, pos.line, pos.column};
}

// #227 follow-up: libs/toml.cppm's `[[dotted.path]]` support is intentionally
// schema-agnostic — it accepts an array-of-tables at ANY dotted path, so a
// doubled-bracket typo like `[[dependencies]]` (single brackets meant) or
// `[[toolchain]]` parses cleanly as an Array Value there. Every mcpp consumer
// reads such sections via get_table(), which returns nullptr for a non-table
// Value, so the section silently reads as ABSENT (e.g. all dependencies
// silently dropped) with no parse error and no warning. Close the grammar
// here, at the manifest layer, instead of hardcoding mcpp section names into
// the generic TOML layer: the only legitimate array-of-tables in mcpp.toml
// today is `[[build.flags]]`.
bool is_array_of_tables(const t::Value& v) {
    if (!v.is_array()) return false;
    auto& arr = v.as_array();
    if (arr.empty()) return false;
    for (auto& e : arr) if (!e.is_table()) return false;
    return true;
}

// #253: shared parser for the per-glob flags array shape
// `[{ glob = "...", cflags/cxxflags/asmflags/defines = [...] }, ...]` —
// one entry grammar for `[build].flags` and `[features].<name>.flags`.
// `ctxLabel` names the anchoring key in error messages. Entries append to
// `dst` in declaration order (order is the override semantics). Returns an
// error message, or nullopt on success.
std::optional<std::string> parse_glob_flags_value(
    const t::Value& fv, std::string_view ctxLabel, std::vector<GlobFlags>& dst)
{
    if (!fv.is_array()) {
        return std::format(
            "{} must be an array of inline tables "
            "(flags = [{{ glob = \"...\", cxxflags = [...] }}, ...])", ctxLabel);
    }
    for (auto& ev : fv.as_array()) {
        if (!ev.is_table()) {
            return std::format(
                "{} entries must be inline tables with a `glob` key", ctxLabel);
        }
        auto& et = ev.as_table();
        GlobFlags gf;
        for (auto& [k, v] : et) {
            auto read_list = [&](std::vector<std::string>& out) -> bool {
                if (!v.is_array()) return false;
                for (auto& s : v.as_array())
                    if (s.is_string()) out.push_back(s.as_string());
                return true;
            };
            bool ok = false;
            if      (k == "glob")     { ok = v.is_string(); if (ok) gf.glob = v.as_string(); }
            else if (k == "cflags")   ok = read_list(gf.cflags);
            else if (k == "cxxflags") ok = read_list(gf.cxxflags);
            else if (k == "asmflags") ok = read_list(gf.asmflags);
            else if (k == "defines")  ok = read_list(gf.defines);
            if (!ok) {
                return std::format(
                    "{}: invalid key '{}' (expected glob = \"...\" "
                    "plus cflags/cxxflags/asmflags/defines arrays)", ctxLabel, k);
            }
        }
        if (gf.glob.empty()) {
            return std::format("{} entry is missing its `glob` key", ctxLabel);
        }
        dst.push_back(std::move(gf));
    }
    return std::nullopt;
}

// Allowlist entries are dotted paths whose segments may be the wildcard `*`,
// matching exactly one path segment — needed for #253's `features.<name>.flags`,
// whose middle segment (the feature name) is author-chosen.
bool aot_path_matches(std::string_view pattern, std::string_view path) {
    while (true) {
        auto pDot = pattern.find('.');
        auto sDot = path.find('.');
        auto pSeg = pattern.substr(0, pDot);
        auto sSeg = path.substr(0, sDot);
        if (pSeg != "*" && pSeg != sSeg) return false;
        if (pDot == std::string_view::npos || sDot == std::string_view::npos)
            return pDot == std::string_view::npos && sDot == std::string_view::npos;
        pattern.remove_prefix(pDot + 1);
        path.remove_prefix(sDot + 1);
    }
}

std::optional<std::string> find_disallowed_array_of_tables(
    const t::Table& tbl, const std::string& prefix,
    std::span<const std::string_view> allowlist)
{
    for (auto& [k, v] : tbl) {
        std::string path = prefix.empty() ? k : std::format("{}.{}", prefix, k);
        if (is_array_of_tables(v)) {
            bool allowed = false;
            for (auto a : allowlist) if (aot_path_matches(a, path)) { allowed = true; break; }
            if (!allowed) return path;
        } else if (v.is_table()) {
            if (auto found = find_disallowed_array_of_tables(v.as_table(), path, allowlist))
                return found;
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin) {
    auto doc = t::parse(content);
    if (!doc) {
        return std::unexpected(error(origin, doc.error().message, doc.error().where));
    }

    // Closed-grammar guard: reject any array-of-tables whose dotted path
    // isn't explicitly allowlisted, BEFORE any section is read. See
    // find_disallowed_array_of_tables above.
    static constexpr std::string_view kAllowedArraysOfTables[] = {
        "build.flags",
        "features.*.flags",   // #253 — the middle segment is the feature name
        "target.*.build.flags",  // #258 — middle segment is the cfg predicate
        "runtime.requirements",
        "runtime.artifacts",
    };
    if (auto badPath = find_disallowed_array_of_tables(doc->root(), "", kAllowedArraysOfTables)) {
        return std::unexpected(error(origin, std::format(
            "[[{}]] (array-of-tables) is not allowed for section '{}'; "
            "array-of-tables syntax is only supported for [[build.flags]], "
            "[[features.<name>.flags]], [[runtime.requirements]], and "
            "[[runtime.artifacts]]",
            *badPath, *badPath)));
    }

    Manifest m;
    m.sourcePath = origin;

    // [package] — required unless [workspace] is present (virtual workspace).
    auto* pkg_t = doc->get_table("package");
    bool has_workspace = (doc->get_table("workspace") != nullptr);
    if (!pkg_t && !has_workspace)
        return std::unexpected(error(origin, "missing required [package] section"));

    auto name = doc->get_string("package.name");
    if (!name && !has_workspace)
        return std::unexpected(error(origin, "missing required field 'package.name'"));
    if (name) m.package.name = *name;

    // 0.0.6+: explicit namespace field (xpkg V1 style).
    // If present, [package].name is the short name.
    // If absent, compat.cppm::resolve_package_name infers from dotted name.
    if (auto v = doc->get_string("package.namespace")) m.package.namespace_ = *v;

    auto version = doc->get_string("package.version");
    if (!version && !has_workspace)
        return std::unexpected(error(origin, "missing required field 'package.version'"));
    if (version) m.package.version = *version;

    if (auto v = doc->get_string("package.description")) m.package.description = *v;
    if (auto v = doc->get_string("package.license"))     m.package.license     = *v;
    if (auto v = doc->get_string("package.repo"))        m.package.repo        = *v;
    if (auto v = doc->get_string_array("package.authors")) m.package.authors  = *v;
    if (auto v = doc->get_string_array("package.platforms")) m.package.platforms = *v;

    // [package].standard (M5.0 new home)
    if (auto v = doc->get_string("package.standard"))    m.package.standard    = *v;

    // [language] (M5.0: deprecated, kept for backward compat — drop in M6)
    // Reads to old fields AND mirrors to new package.standard if [package].standard not set.
    bool had_language_section = (doc->get_table("language") != nullptr);
    if (auto v = doc->get_string("language.standard")) {
        m.language.standard = *v;
        // mirror to new home only if [package].standard wasn't explicitly set
        if (!doc->get_string("package.standard")) m.package.standard = *v;
    } else {
        m.language.standard = m.package.standard;   // keep old field consistent with new
    }
    if (auto v = doc->get_bool("language.modules"))      m.language.modules    = *v;
    if (auto v = doc->get_bool("language.import_std"))   m.language.importStd = *v;

    // Validation on the unified standard. Store the canonical spelling so all
    // downstream build surfaces consume one active value.
    auto stdCfg = normalize_cpp_standard(m.package.standard);
    if (!stdCfg) return std::unexpected(error(origin, stdCfg.error()));
    m.cppStandard = *stdCfg;
    m.package.standard = m.cppStandard.canonical;
    m.language.standard = m.cppStandard.canonical;
    if (had_language_section && !m.language.modules) {
        return std::unexpected(error(origin,
            "language.modules must be true (mcpp is modules-only)"));
    }

    // [build].sources (M5.0 new home) + [modules].sources (deprecated, compat)
    if (auto v = doc->get_string_array("build.sources"))   m.buildConfig.sources = *v;
    if (auto v = doc->get_string_array("modules.sources")) {
        m.modules.sources = *v;
        // If [build].sources wasn't set, mirror legacy field into new field.
        if (m.buildConfig.sources.empty()) m.buildConfig.sources = *v;
    }
    // Mirror new → legacy so existing code reading manifest.modules.sources keeps working.
    if (m.modules.sources.empty()) m.modules.sources = m.buildConfig.sources;

    if (auto v = doc->get_string_array("modules.exports")) m.modules.exports_ = *v;
    if (auto v = doc->get_bool("modules.strict"))          m.modules.strict   = *v;

    // [build].include_dirs (M5.0 new field)
    if (auto v = doc->get_string_array("build.include_dirs")) {
        for (auto& s : *v) m.buildConfig.includeDirs.emplace_back(s);
    }
    // [build].include_dirs_after (#249) — searched after system dirs (-idirafter).
    if (auto v = doc->get_string_array("build.include_dirs_after")) {
        for (auto& s : *v) m.buildConfig.includeDirsAfter.emplace_back(s);
    }

    // [targets.*] — M5.0: now optional. If absent, defer to auto-inference (in load()).
    // [profile.<name>] — bundled build settings.
    if (auto* profile_table = doc->get_table("profile");
        profile_table && !profile_table->empty()) {
        for (auto& [pname, pval] : *profile_table) {
            if (!pval.is_table()) continue;
            auto& tt = pval.as_table();
            Profile pr;
            if (auto it = tt.find("opt"); it != tt.end()) {
                if      (it->second.is_string()) pr.optLevel = it->second.as_string();
                else if (it->second.is_int())    pr.optLevel = std::to_string(it->second.as_int());
            }
            if (auto it = tt.find("debug"); it != tt.end() && it->second.is_bool()) pr.debug = it->second.as_bool();
            if (auto it = tt.find("lto");   it != tt.end() && it->second.is_bool()) pr.lto   = it->second.as_bool();
            if (auto it = tt.find("strip"); it != tt.end() && it->second.is_bool()) pr.strip = it->second.as_bool();
            auto read_list = [&](const char* key, std::vector<std::string>& out) {
                if (auto it = tt.find(key); it != tt.end() && it->second.is_array())
                    for (auto& v : it->second.as_array())
                        if (v.is_string()) out.push_back(v.as_string());
            };
            read_list("cflags",   pr.cflags);
            read_list("cxxflags", pr.cxxflags);
            read_list("ldflags",  pr.ldflags);
            m.profiles[pname] = pr;
        }
    }

    // [features] — feature name → implied features. "default" lists the
    // default-active set. Two accepted shapes (Feature System v2):
    //   array form (shorthand):  name = ["implied", ...]
    //   table form (full):       name = { implies = [...], defines = [...] }
    // The table form lets a feature contribute package-owned defines (Stage 1);
    // `requires`/`provides`/`deps` keys are reserved for later stages.
    if (auto* features_table = doc->get_table("features");
        features_table && !features_table->empty()) {
        auto read_str_array = [](const auto& tbl, std::string_view key,
                                 std::vector<std::string>& out) {
            if (auto it = tbl.find(std::string(key));
                it != tbl.end() && it->second.is_array())
                for (auto& v : it->second.as_array())
                    if (v.is_string()) out.push_back(v.as_string());
        };
        for (auto& [fname, fval] : *features_table) {
            std::vector<std::string> implied;
            std::vector<std::string> forwardTokens;
            if (fval.is_array()) {
                for (auto& v : fval.as_array())
                    if (v.is_string()) implied.push_back(v.as_string());
            } else if (fval.is_table()) {
                auto& ft = fval.as_table();
                read_str_array(ft, "implies", implied);
                // #243: a feature may forward features to its dependencies
                // (Cargo `dep/feat`). Two equivalent spellings, one data model:
                // `dep/feat` tokens mixed into `implies` (Cargo parity), or a
                // dedicated self-documenting `forward = ["dep/feat", ...]` key.
                read_str_array(ft, "forward", forwardTokens);
                std::vector<std::string> defs;
                read_str_array(ft, "defines", defs);
                if (!defs.empty()) m.buildConfig.featureDefines[fname] = std::move(defs);
                // Feature-gated source globs — same semantics as the index
                // descriptor's `sources` key (one data model, two grammars):
                // listed globs leave the default build and compile only when
                // the feature is active.
                std::vector<std::string> fsrcs;
                read_str_array(ft, "sources", fsrcs);
                if (!fsrcs.empty()) m.buildConfig.featureSources[fname] = std::move(fsrcs);
                std::vector<std::string> reqs, provs;
                read_str_array(ft, "requires", reqs);
                read_str_array(ft, "provides", provs);
                if (!reqs.empty())  m.featureRequires[fname] = std::move(reqs);
                if (!provs.empty()) m.featureProvides[fname] = std::move(provs);
                // #253: per-feature per-glob compile flags — same entry grammar
                // as [build].flags (shared parse_glob_flags_value), gated by
                // this feature and folded in AFTER base globFlags at activation
                // so feature rules win via "last flag wins". Both spellings
                // reach here: the inline array and [[features.X.flags]] AOT
                // (allowlisted via the features.*.flags pattern, mirroring
                // #227's build.flags decision — libs/toml builds one shape).
                if (auto it = ft.find(std::string("flags")); it != ft.end()) {
                    if (auto err = parse_glob_flags_value(
                            it->second,
                            std::format("[features].{}.flags", fname),
                            m.buildConfig.featureFlags[fname])) {
                        return std::unexpected(error(origin, *err));
                    }
                }
            }
            // #243: split `dep/feat` tokens out of `implies` into featureForwards
            // (raw depKey shares the `dependencies`/`featureDeps` keyspace); the
            // dedicated `forward` key is always forwards. Plain names stay implies.
            std::vector<std::string> localImplies;
            for (auto& tok : implied) {
                if (auto fwd = mcpp::pm::split_feature_forward_token(tok))
                    m.featureForwards[fname].push_back(std::move(*fwd));
                else
                    localImplies.push_back(std::move(tok));
            }
            for (auto& tok : forwardTokens)
                if (auto fwd = mcpp::pm::split_feature_forward_token(tok))
                    m.featureForwards[fname].push_back(std::move(*fwd));
            m.featuresMap[fname] = std::move(localImplies);
        }
    }

    // [package] provides — package-level capabilities (Feature System v2 S3).
    if (auto v = doc->get_string_array("package.provides")) m.provides = *v;

    // [capabilities] cap = "provider" — root-only provider pins.
    if (auto* caps = doc->get_table("capabilities"); caps && !caps->empty()) {
        for (auto& [cap, cval] : *caps)
            if (cval.is_string()) m.capabilityPins[cap] = cval.as_string();
    }

    // [tools.overrides] "<pkg>:<tool>" = "<path>" — #355 escape hatch. Use an
    // existing host binary instead of building the dependency's tool target.
    // Root-only, like [capabilities]: it is the consumer's environment being
    // described, and a dependency has no business overriding it.
    if (auto* tovr = doc->get_table("tools.overrides"); tovr && !tovr->empty()) {
        for (auto& [k, v] : *tovr)
            if (v.is_string()) m.toolOverrides[k] = v.as_string();
    }

    // [generated_files] — "relative/path" = "file contents" (multiline
    // strings supported). Same mechanism as the index descriptor's
    // generated_files key: materialized into the package root before glob
    // expansion, content folded into the package fingerprint. Paths are
    // validated again at materialize time; checking here gives the error a
    // manifest location.
    if (auto* gf = doc->get_table("generated_files"); gf && !gf->empty()) {
        for (auto& [rel, val] : *gf) {
            if (!val.is_string()) {
                return std::unexpected(error(origin, std::format(
                    "[generated_files].\"{}\" must be a string (file contents)", rel)));
            }
            std::filesystem::path p(rel);
            // has_root_path, not is_absolute: on Windows "/x" is root-relative
            // (not absolute) yet still escapes the project root.
            bool escapes = rel.empty() || p.has_root_path();
            // const&: libc++'s path iterator dereferences to a temporary
            // path (libstdc++ hands out a reference) — auto& won't bind.
            for (auto const& part : p.lexically_normal())
                if (part == "..") { escapes = true; break; }
            if (escapes) {
                return std::unexpected(error(origin, std::format(
                    "[generated_files] path '{}' must be relative and stay "
                    "inside the project root", rel)));
            }
            m.buildConfig.generatedFiles.emplace(std::move(p), val.as_string());
        }
    }

    // [scan_overrides."<glob>"] — author-asserted scan results (see
    // manifest:types ScanOverride). provides/imports are string arrays.
    if (auto* so_table = doc->get_table("scan_overrides");
        so_table && !so_table->empty()) {
        for (auto& [glob, val] : *so_table) {
            if (!val.is_table()) {
                return std::unexpected(error(origin,
                    std::format("[scan_overrides.\"{}\"] must be a table", glob)));
            }
            manifest::ScanOverride ov;
            auto& st = val.as_table();
            auto read_names = [&](const char* key, std::vector<std::string>& out)
                -> std::optional<std::string> {
                auto it = st.find(key);
                if (it == st.end()) return std::nullopt;
                if (!it->second.is_array())
                    return std::format("scan_overrides.\"{}\".{} must be an array", glob, key);
                for (auto& v : it->second.as_array()) {
                    if (!v.is_string() || v.as_string().empty())
                        return std::format("scan_overrides.\"{}\".{} entries must be non-empty strings", glob, key);
                    out.push_back(v.as_string());
                }
                return std::nullopt;
            };
            if (auto msg = read_names("provides", ov.provides))
                return std::unexpected(error(origin, *msg));
            if (auto msg = read_names("imports", ov.imports))
                return std::unexpected(error(origin, *msg));
            if (ov.provides.empty() && ov.imports.empty()) {
                return std::unexpected(error(origin, std::format(
                    "scan_overrides.\"{}\" declares neither provides nor imports", glob)));
            }
            m.modules.scanOverrides.emplace(glob, std::move(ov));
        }
    }

    auto* targets_table = doc->get_table("targets");
    if (targets_table && !targets_table->empty()) {
    for (auto& [tname, tval] : *targets_table) {
        if (!tval.is_table()) {
            return std::unexpected(error(origin,
                std::format("[targets.{}] must be a table", tname)));
        }
        Target t;
        t.name = tname;
        auto& tt = tval.as_table();

        auto kit = tt.find("kind");
        if (kit == tt.end() || !kit->second.is_string()) {
            return std::unexpected(error(origin,
                std::format("targets.{}.kind missing or not a string", tname)));
        }
        const auto& kind_s = kit->second.as_string();
        if      (kind_s == "lib"    || kind_s == "library")  t.kind = Target::Library;
        else if (kind_s == "bin"    || kind_s == "binary")   t.kind = Target::Binary;
        else if (kind_s == "shared" || kind_s == "dylib"
              || kind_s == "so"     || kind_s == "shlib")    t.kind = Target::SharedLibrary;
        else return std::unexpected(error(origin,
            std::format("targets.{}.kind must be 'bin', 'lib' or 'shared'; got '{}'", tname, kind_s)));

        if (t.kind == Target::Binary) {
            auto mit = tt.find("main");
            if (mit == tt.end() || !mit->second.is_string()) {
                return std::unexpected(error(origin,
                    std::format("targets.{} (kind=bin) requires 'main' field", tname)));
            }
            t.main = mit->second.as_string();
        }
        if (auto sit = tt.find("soname"); sit != tt.end()) {
            if (!sit->second.is_string()) {
                return std::unexpected(error(origin,
                    std::format("targets.{}.soname must be a string", tname)));
            }
            t.soname = sit->second.as_string();
        }
        if (auto msg = validate_target_soname(t, std::format("targets.{}.", tname))) {
            return std::unexpected(error(origin, *msg));
        }

        // Per-target flags (entry-scoped) + required-features gate.
        auto read_list = [&](const char* key, std::vector<std::string>& out) {
            if (auto it = tt.find(key); it != tt.end() && it->second.is_array())
                for (auto& v : it->second.as_array())
                    if (v.is_string()) out.push_back(v.as_string());
        };
        read_list("cflags",            t.cflags);
        read_list("cxxflags",          t.cxxflags);
        read_list("defines",           t.defines);
        read_list("required_features", t.requiredFeatures);
        // Guard: -std=... belongs to [package].standard, not per-target flags
        // (same rule as [build].cxxflags). Reject early with a clear message.
        for (auto const& flag : t.cxxflags) {
            if (starts_with_std_flag(flag)) {
                return std::unexpected(error(origin, std::format(
                    "targets.{}.cxxflags contains '{}'; use [package].standard to "
                    "configure the C++ language standard", tname, flag)));
            }
        }

        // Surface unsupported keys instead of silently dropping them — the
        // historic footgun behind issue #131 (a `[targets.x] cxxflags` typo on
        // an older mcpp just vanished). Per-target arbitrary build config that
        // must reach SHARED code is intentionally not a target key; point users
        // at the right axis (workspace / features / profile).
        static constexpr std::string_view kKnownTargetKeys[] = {
            "kind", "main", "soname",
            "cflags", "cxxflags", "defines", "required_features",
        };
        for (auto& [key, _] : tt) {
            bool known = false;
            for (auto k : kKnownTargetKeys) if (key == k) { known = true; break; }
            if (!known) {
                m.schemaWarnings.push_back(std::format(
                    "[targets.{}] has unsupported key '{}' (ignored). Per-target keys: "
                    "kind, main, soname, cflags, cxxflags, defines, required_features. "
                    "For config that must affect shared code, split into a workspace "
                    "member or use [features]; for a whole-build mode use [profile.*].",
                    tname, key));
            }
        }
        m.targets.push_back(std::move(t));
    }
    } // close `if (targets_table && !targets_table->empty())`

    // [dependencies] / [dev-dependencies]
    //
    // Three accepted forms (M5.x):
    //
    //   (1) flat / default-ns
    //         [dependencies]
    //         gtest = "1.15.2"             ⇒ (mcpp, gtest)
    //         frob  = { path = "..." }     ⇒ (mcpp, frob) inline spec
    //
    //   (2) namespaced subtable (TOML-native, no quotes)
    //         [dependencies.mcpplibs]
    //         cmdline = "0.0.2"            ⇒ (mcpplibs, cmdline)
    //         tmpl    = { version = "0.0.1", features = [...] }
    //
    //   (3) legacy quoted dotted form (deprecated, still parsed)
    //         [dependencies]
    //         "mcpplibs.cmdline" = "0.0.2" ⇒ (mcpplibs, cmdline) + warning
    //
    // The map key remains the fully-qualified `<ns>.<name>` for non-default
    // namespaces (so existing fetcher / lockfile lookups by composite name
    // keep working) and the bare `<name>` for the default namespace (so the
    // common case stays unchanged).
    // MUST list every key `fill_inline_spec` below reads.
    // `Manifest.EveryDependencySpecKeyIsAccepted` holds the two in sync.
    auto is_dep_spec_key = [](std::string_view k) {
        return k == "path"   || k == "version" || k == "git"
            || k == "rev"    || k == "tag"     || k == "branch"
            || k == "features" || k == "default-features"
            || k == "workspace" || k == "visibility"
            || k == "backend"  || k == "tools"
            || k == "host-module" || k == "reexport";
    };
    // What makes a table an inline dep spec is that it names a SOURCE. This
    // used to be "every key is known", which quietly coupled two unrelated
    // things: the discriminator (spec vs nested namespace table) and the
    // vocabulary (which keys mean something).
    //
    // The coupling is a compatibility hazard, not a style problem. A manifest
    // using a key introduced after the reader was built did not get "unknown
    // option" — the table failed the discriminator, was taken for a NAMESPACE,
    // and the user was told their `reexport = true` "must be a string, inline
    // dep table, or nested table". Worse, a published package cannot adopt a
    // new key at all, because every older client fails to load it outright
    // rather than ignoring what it does not understand. That is the same
    // property #349 established for the index floor: data must not be able to
    // decide whether the program works.
    //
    // An identity key is an unambiguous discriminator: a nested namespace
    // table's keys are PACKAGE names, and no package is named `version` /
    // `path` / `git` / `workspace`.
    auto looks_like_inline_dep_spec = [](const t::Table& sub) {
        if (sub.empty()) return false;
        for (auto& [sk, sv] : sub)
            if (sk == "path" || sk == "version" || sk == "git" || sk == "workspace")
                return true;
        return false;
    };

    auto fill_inline_spec = [&](DependencySpec& spec,
                                std::string_view section,
                                std::string_view fqName,
                                const t::Table& sub) -> std::expected<void, ManifestError>
    {
        // Now that the discriminator no longer doubles as the vocabulary, an
        // unrecognized key can be REPORTED — as a degradation, so `--strict`
        // still refuses it, while an ordinary build of a package written for a
        // newer mcpp proceeds with the part this one understands. Same
        // discipline as the xpkg reader's `xpkgUnknownKeys`: record rather
        // than swallow, and never fail the whole load over it.
        for (auto& [sk, sv] : sub) {
            if (is_dep_spec_key(sk)) continue;
            m.schemaWarnings.push_back(std::format(
                "[{}.\"{}\"] has unrecognized key '{}' (ignored). It may be a "
                "typo, or a field a newer mcpp understands.", section, fqName, sk));
        }
        if (auto it = sub.find("path");    it != sub.end() && it->second.is_string()) spec.path    = it->second.as_string();
        if (auto it = sub.find("version"); it != sub.end() && it->second.is_string()) spec.version = it->second.as_string();
        if (auto it = sub.find("git");     it != sub.end() && it->second.is_string()) spec.git     = it->second.as_string();
        if (auto it = sub.find("visibility"); it != sub.end() && it->second.is_string()) {
            spec.visibility = it->second.as_string();
            if (spec.visibility != "public"
                && spec.visibility != "private"
                && spec.visibility != "interface") {
                return std::unexpected(error(origin, std::format(
                    "[{}.\"{}\"] visibility must be 'public', 'private', or 'interface'",
                    section, fqName)));
            }
        }
        if (auto it = sub.find("features"); it != sub.end() && it->second.is_array()) {
            for (auto& fv : it->second.as_array())
                if (fv.is_string()) spec.features.push_back(fv.as_string());
        }
        // `default-features = false` — consumer opts out of the dependency's
        // own `[features].default` seed (Cargo parity). Explicitly requested
        // `features = [...]` still activate. Threaded into feature_closure so
        // the default pseudo-feature is not seeded for this dependency.
        if (auto it = sub.find("default-features"); it != sub.end() && it->second.is_bool()) {
            spec.defaultFeatures = it->second.as_bool();
        }
        // #355: `tools = ["protoc"]` — HOST binaries this consumer wants from
        // the dependency. Same shape as `features`, and deliberately on the
        // dependency edge: requesting an extra artifact from the graph is a
        // graph-level request, so it stays declarative in mcpp.toml.
        if (auto it = sub.find("tools"); it != sub.end() && it->second.is_array()) {
            for (auto& tv : it->second.as_array())
                if (tv.is_string()) spec.tools.push_back(tv.as_string());
        }
        // #355 step 5: `host-module = true` — make this dependency's lib-root
        // module importable from build.mcpp (reusable rules as packages).
        if (auto it = sub.find("host-module"); it != sub.end() && it->second.is_bool()) {
            spec.hostModule = it->second.as_bool();
        }
        // #359: `reexport = true` — hand this edge's build-time provisions
        // (tools, host module, dependency dir) on to THIS package's consumers.
        // Off by default; see DependencySpec::reexport for why it is not the
        // edge's `visibility`.
        if (auto it = sub.find("reexport"); it != sub.end() && it->second.is_bool()) {
            spec.reexport = it->second.as_bool();
        }
        // `backend = "<impl>"` — sugar for requesting the dependency's
        // `backend-<impl>` feature (library-level backend selection knob).
        if (auto it = sub.find("backend"); it != sub.end() && it->second.is_string()) {
            spec.features.push_back("backend-" + it->second.as_string());
        }
        if (auto it = sub.find("rev");     it != sub.end() && it->second.is_string()) {
            spec.gitRev     = it->second.as_string();
            spec.gitRefKind = "rev";
        } else if (auto it = sub.find("tag");    it != sub.end() && it->second.is_string()) {
            spec.gitRev     = it->second.as_string();
            spec.gitRefKind = "tag";
        } else if (auto it = sub.find("branch"); it != sub.end() && it->second.is_string()) {
            spec.gitRev     = it->second.as_string();
            spec.gitRefKind = "branch";
        }
        if (auto it = sub.find("workspace"); it != sub.end() && it->second.is_bool() && it->second.as_bool()) {
            spec.inheritWorkspace = true;
            return {};  // version will be filled in by workspace merge
        }
        if (spec.path.empty() && spec.version.empty() && spec.git.empty()) {
            return std::unexpected(error(origin, std::format(
                "[{}.\"{}\"] must specify 'path', 'version', or 'git'", section, fqName)));
        }
        if (!spec.git.empty() && spec.gitRev.empty()) {
            return std::unexpected(error(origin, std::format(
                "[{}.\"{}\"] git dep requires one of: rev / tag / branch", section, fqName)));
        }
        return {};
    };

    auto parse_dep_selector = [&](std::string_view section,
                                  std::string_view selectorText,
                                  std::string_view stableMapKey)
        -> std::expected<mcpp::pm::DependencySelector, ManifestError>
    {
        auto parsed = mcpp::pm::parse_package_selector(selectorText);
        if (!parsed) {
            return std::unexpected(error(origin, std::format(
                "[{}] {}", section, parsed.error().message)));
        }
        auto coordinate = mcpp::pm::normalize_package_selector(*parsed);
        return mcpp::pm::make_direct_dependency_selector(
            coordinate.namespace_, coordinate.shortName, stableMapKey,
            /*namespaceOmitted=*/!parsed->namespace_.has_value());
    };

    auto assign_dep = [&](std::string_view section,
                          std::map<std::string, DependencySpec>& out,
                          const mcpp::pm::DependencySelector& selector,
                          const t::Value& value,
                          bool legacyDottedKey,
                          bool legacyCandidateSearch)
        -> std::expected<void, ManifestError>
    {
        if (selector.candidates.empty()) {
            return std::unexpected(error(origin, std::format(
                "[{}] dependency selector '{}' has no candidates",
                section, selector.stableMapKey)));
        }

        DependencySpec spec;
        spec.namespace_ = selector.candidates.front().namespace_;
        spec.shortName = selector.candidates.front().shortName;
        spec.candidates = selector.candidates;
        spec.legacyDottedKey = legacyDottedKey;
        spec.legacyCandidateSearch = legacyCandidateSearch;
        spec.namespaceOmitted = selector.namespaceOmitted;

        auto key = selector.stableMapKey;
        if (value.is_string()) {
            spec.version = value.as_string();
        } else if (value.is_table()) {
            auto& sub = value.as_table();
            if (!looks_like_inline_dep_spec(sub)) {
                return std::unexpected(error(origin, std::format(
                    "[{}.{}] must be a version string, or a table naming a "
                    "source (one of path/version/git/workspace) alongside any "
                    "of rev/tag/branch/features/default-features/visibility/"
                    "backend/tools/host-module/reexport",
                    section, key)));
            }
            if (auto r = fill_inline_spec(spec, section, key, sub); !r) return r;
        } else {
            return std::unexpected(error(origin, std::format(
                "[{}].{} must be a string (version) or table (path/version/...)",
                section, key)));
        }

        out[key] = std::move(spec);
        return {};
    };

    auto is_namespace_table = [&](std::string_view section,
                                  std::string_view key) {
        auto path = std::format("{}.{}", section, key);
        return doc->has_explicit_table(path)
            || key == kDefaultNamespace;
    };

    std::function<std::expected<void, ManifestError>(
        std::string_view,
        std::map<std::string, DependencySpec>&,
        std::string,
        std::string,
        const t::Table&)> load_nested_dep_table;

    load_nested_dep_table =
        [&](std::string_view section,
            std::map<std::string, DependencySpec>& out,
            std::string ns,
            std::string mapPrefix,
            const t::Table& table) -> std::expected<void, ManifestError>
    {
        for (auto& [k, v] : table) {
            if (v.is_string() ||
                (v.is_table() && looks_like_inline_dep_spec(v.as_table()))) {
                auto mapKey = mapPrefix.empty()
                    ? k
                    : std::format("{}.{}", mapPrefix, k);
                auto selectorText = std::format("{}.{}", ns, k);
                auto selector = parse_dep_selector(
                    section, selectorText, mapKey);
                if (!selector) return std::unexpected(selector.error());
                if (auto r = assign_dep(
                        section, out, *selector, v, false, false); !r)
                    return r;
                continue;
            }
            if (!v.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "[{}].{}.{} must be a string, inline dep table, or nested table",
                    section, ns, k)));
            }
            auto childNs = std::format("{}.{}", ns, k);
            auto childMapPrefix = mapPrefix.empty()
                ? k
                : std::format("{}.{}", mapPrefix, k);
            if (auto r = load_nested_dep_table(
                    section, out, childNs, childMapPrefix, v.as_table()); !r)
                return r;
        }
        return {};
    };

    std::function<std::expected<void, ManifestError>(
        std::string_view,
        std::map<std::string, DependencySpec>&,
        std::string,
        const t::Table&)> load_selector_dep_table;

    load_selector_dep_table =
        [&](std::string_view section,
            std::map<std::string, DependencySpec>& out,
            std::string selectorPrefix,
            const t::Table& table) -> std::expected<void, ManifestError>
    {
        for (auto& [k, v] : table) {
            auto selectorText = selectorPrefix.empty()
                ? k
                : std::format("{}.{}", selectorPrefix, k);
            if (v.is_string() ||
                (v.is_table() && looks_like_inline_dep_spec(v.as_table()))) {
                auto selector = parse_dep_selector(
                    section, selectorText, selectorText);
                if (!selector) return std::unexpected(selector.error());
                if (auto r = assign_dep(
                        section, out, *selector, v, false, true); !r)
                    return r;
                continue;
            }
            if (!v.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "[{}].{} must be a string, inline dep table, or nested table",
                    section, selectorText)));
            }
            if (auto r = load_selector_dep_table(
                    section, out, selectorText, v.as_table()); !r)
                return r;
        }
        return {};
    };

    // Parse a dependency table (already obtained) into `out`. Factored out of
    // load_deps so the same logic serves both [dependencies] (via doc->get_table)
    // and [target.'cfg(...)'.dependencies] (a nested table the dotted getter
    // can't address). `section` is the logical section name, used for error
    // messages and namespace/selector resolution.
    auto load_deps_table = [&](std::string_view section, auto& tt,
                               std::map<std::string, DependencySpec>& out)
        -> std::expected<void, ManifestError>
    {
        for (auto& [k, v] : tt) {
            // A string value is either a bare default-namespace selector or a
            // quoted dotted selector. Both go through the same exact parser;
            // quoted dotted syntax is retained only as a source-shape marker.
            if (v.is_string()) {
                auto selector = parse_dep_selector(section, k, k);
                if (!selector) return std::unexpected(selector.error());
                const bool quotedDotted = k.find('.') != std::string::npos;
                if (auto r = assign_dep(
                        section, out, *selector, v, quotedDotted, false); !r)
                    return r;
                continue;
            }

            if (!v.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "[{}].{} must be a string (version) or table (path/version/...)", section, k)));
            }

            auto& sub = v.as_table();

            // (1') inline dep spec under the default namespace, e.g.
            //         frob = { path = "..." }     or
            //         "mcpplibs.cmdline" = { version = "0.0.2" }
            // The latter is the legacy quoted source shape, but carries the
            // same exact PackageId as every other selector surface.
            if (looks_like_inline_dep_spec(sub)) {
                auto selector = parse_dep_selector(section, k, k);
                if (!selector) return std::unexpected(selector.error());
                const bool quotedDotted = k.find('.') != std::string::npos;
                if (auto r = assign_dep(
                        section, out, *selector, v, quotedDotted, false); !r)
                    return r;
                continue;
            }

            // (2) namespaced or nested subtable.
            //
            // Explicit tables such as `[dependencies.acme]` are namespace
            // roots. Dotted keys inside the single dependency table are exact
            // selectors too: `capi.lua` means only `(capi, lua)`.
            if (is_namespace_table(section, k)) {
                if (auto r = load_nested_dep_table(section, out, k, k, sub); !r)
                    return r;
            } else if (auto r = load_selector_dep_table(section, out, k, sub); !r) {
                return r;
            }
        }
        return {};
    };
    auto load_deps = [&](std::string_view section, std::map<std::string, DependencySpec>& out)
        -> std::expected<void, ManifestError>
    {
        auto* tt = doc->get_table(section);
        if (!tt) return {};
        return load_deps_table(section, *tt, out);
    };
    if (auto r = load_deps("dependencies",       m.dependencies);       !r) return std::unexpected(r.error());
    if (auto r = load_deps("dev-dependencies",   m.devDependencies);    !r) return std::unexpected(r.error());
    if (auto r = load_deps("build-dependencies", m.buildDependencies);  !r) return std::unexpected(r.error());

    // [feature-deps.<feature>] — optional dependencies activated by a feature
    // (Stage 2a). Each sub-table is loaded with the same dependency loader as
    // [dependencies], keyed by the feature name.
    if (auto* fdeps = doc->get_table("feature-deps")) {
        for (auto& [fname, fval] : *fdeps) {
            if (!fval.is_table()) continue;
            if (auto r = load_deps("feature-deps." + std::string(fname),
                                   m.featureDeps[fname]); !r)
                return std::unexpected(r.error());
            m.featuresMap.try_emplace(fname, std::vector<std::string>{}); // register
        }
    }

    // [toolchain] — platform → "pkg@version" map (docs/21)
    if (auto* tt = doc->get_table("toolchain")) {
        for (auto& [platform, val] : *tt) {
            if (!val.is_string()) {
                return std::unexpected(error(origin,
                    std::format("[toolchain].{} must be a string like \"gcc@15.1.0\"", platform)));
            }
            m.toolchain.byPlatform[platform] = val.as_string();
        }
    }

    // [build] — backend tunables
    if (auto v = doc->get_bool("build.static_stdlib")) m.buildConfig.staticStdlib = *v;
    // #336 — [build] cxx_runtime. Two spellings for one field, cargo-style:
    //   cxx_runtime = "host-coupled"                                  (all roles)
    //   cxx_runtime = { default = "...", tests = "...", shared = "..." }
    //                                                                 (per role)
    // Rejecting an unknown value here is what lets flags.cppm parse it later
    // with `value_or` and no second validation path.
    {
        auto check = [&](std::string_view where, const std::string& v)
            -> std::optional<ManifestError>
        {
            static constexpr std::string_view kOk[] = {
                "self-contained", "toolchain-coupled", "host-coupled" };
            for (auto k : kOk) if (v == k) return std::nullopt;
            return error(origin, std::format(
                "{} = '{}' is invalid; expected one of \"self-contained\", "
                "\"toolchain-coupled\", \"host-coupled\"", where, v));
        };
        if (auto* val = doc->get("build.cxx_runtime")) {
            if (val->is_string()) {
                auto s = val->as_string();
                if (auto e = check("[build].cxx_runtime", s)) return std::unexpected(*e);
                m.buildConfig.cxxRuntime = s;
            } else if (val->is_table()) {
                for (auto& [key, v] : val->as_table()) {
                    if (key != "default" && key != "tests" && key != "shared")
                        return std::unexpected(error(origin, std::format(
                            "[build].cxx_runtime has unsupported key '{}'; "
                            "expected 'default', 'tests' or 'shared'", key)));
                    if (!v.is_string())
                        return std::unexpected(error(origin, std::format(
                            "[build].cxx_runtime.{} must be a string", key)));
                    auto s = v.as_string();
                    if (auto e = check(std::format("[build].cxx_runtime.{}", key), s))
                        return std::unexpected(*e);
                    if (key == "tests")       m.buildConfig.cxxRuntimeTests  = s;
                    else if (key == "shared") m.buildConfig.cxxRuntimeShared = s;
                    else                      m.buildConfig.cxxRuntime       = s;
                }
            } else {
                return std::unexpected(error(origin,
                    "[build].cxx_runtime must be a string or a table"));
            }
        }
    }
    if (auto v = doc->get_bool("build.allow_host_libs")) m.buildConfig.allowHostLibs = *v;
    if (auto v = doc->get_string_array("build.cflags"))   m.buildConfig.cflags   = *v;
    if (auto v = doc->get_string_array("build.cxxflags")) m.buildConfig.cxxflags = *v;
    if (auto v = doc->get_string_array("build.defines"))  m.buildConfig.defines  = *v;
    // Module-graph-global dialect flags (issue #210) — see types.cppm
    // dialect_flags(); this key is the explicit escape hatch for flags the
    // known-list doesn't recognize yet.
    if (auto v = doc->get_string_array("build.dialect_cxxflags"))
        m.buildConfig.dialectCxxflags = *v;
    if (auto v = doc->get_string_array("build.ldflags"))  m.buildConfig.ldflags  = *v;
    // [build] flags = [{ glob = "...", cflags/cxxflags/asmflags/defines }]
    // Per-glob compile flags (G4). An ARRAY of inline tables — TOML tables
    // are sorted maps, so only the array form can carry declaration order,
    // and order is the override semantics (later entries win via "last flag
    // wins"). Unknown keys are errors: closed grammar.
    //
    // #227: `[[build.flags]]` array-of-tables is accepted here too, with no
    // extra branching needed — libs/toml.cppm's AOT support
    // (open_array_of_tables) builds the exact same shape at this dotted path
    // (an Array of Table Values) that the inline form `flags = [{...}, ...]`
    // does, so `doc->get("build.flags")` and the loop below see one
    // representation regardless of which spelling was used in the source.
    // Declaration order is preserved by both (Array is a vector).
    if (auto* fv = doc->get("build.flags")) {
        if (!fv->is_array()) {
            return std::unexpected(error(origin,
                "[build].flags must be an array of inline tables "
                "(flags = [{ glob = \"...\", cxxflags = [...] }, ...]) "
                "or an array of tables ([[build.flags]] glob = \"...\")"));
        }
        if (auto err = parse_glob_flags_value(
                *fv, "[build].flags", m.buildConfig.globFlags)) {
            return std::unexpected(error(origin, *err));
        }
    }
    // [build] module_extensions — extra module-interface extensions. Parsed
    // BEFORE apply_defaults_and_infer runs, because the convention default for
    // `sources` is derived from it.
    if (auto v = doc->get_string_array("build.module_extensions")) {
        if (auto err = mcpp::validate_module_extensions(*v))
            return std::unexpected(error(origin, *err));
        m.buildConfig.moduleExtensions = *v;
    }
    // [build] build_program_timeout — seconds a build.mcpp may run; 0 = no
    // limit. `optional` is load-bearing: with a plain int, "absent" and
    // "explicitly 0" would be the same value, and every project that never
    // mentions the key would silently lose its run bound.
    if (auto* tv = doc->get("build.build_program_timeout")) {
        if (!tv->is_int()) {
            return std::unexpected(error(origin,
                "[build].build_program_timeout must be an integer number of "
                "seconds (0 = no limit)"));
        }
        auto secs = tv->as_int();
        if (secs < 0) {
            return std::unexpected(error(origin, std::format(
                "[build].build_program_timeout is {} seconds; it cannot be "
                "negative (0 = no limit)", secs)));
        }
        if (secs > std::numeric_limits<int>::max()) {
            return std::unexpected(error(origin, std::format(
                "[build].build_program_timeout is {} seconds, which does not "
                "fit in the deadline; use 0 for no limit", secs)));
        }
        m.buildConfig.buildProgramTimeoutSecs = static_cast<int>(secs);
    }
    if (auto v = doc->get_string("build.c_standard"))     m.buildConfig.cStandard = *v;
    if (auto v = doc->get_string("build.target"))         m.buildConfig.target = *v;
    // `jobs` accepts a number or "auto"; both arrive as text and are validated
    // where they are used, so a bad value warns at build time instead of making
    // the whole manifest unloadable. (A published package carrying an unknown
    // key must never break an older mcpp — same rule the dependency keys follow.)
    if (auto v = doc->get_string("build.bmi_schedule")) m.buildConfig.bmiSchedule = *v;
    if (auto v = doc->get_string("build.jobs")) m.buildConfig.jobs = *v;
    else if (auto n = doc->get_int("build.jobs")) m.buildConfig.jobs = std::to_string(*n);
    if (auto v = doc->get_string("build.default-profile")) m.buildConfig.defaultProfile = *v;
    else if (auto v = doc->get_string("build.profile"))   m.buildConfig.defaultProfile = *v;  // accepted alias
    if (auto v = doc->get_string("build.cache"))          m.buildConfig.cacheMode = *v;

    // [xlings] — build environment (L-1). Subsections mirror .xlings.json 1:1.
    if (auto v = doc->get_string_array("xlings.deps"))  m.xlings.deps = *v;
    if (doc->get("xlings.subos")) {
        m.xlings.subosDeclared = true;
        if (auto v = doc->get_string("xlings.subos")) m.xlings.subos = *v;
    }
    if (auto* wt = doc->get_table("xlings.workspace"))
        for (auto& [k, val] : *wt)
            if (val.is_string()) m.xlings.workspace[k] = val.as_string();
    if (auto* et = doc->get_table("xlings.envs"))
        for (auto& [k, val] : *et)
            if (val.is_string()) m.xlings.envs[k] = val.as_string();
    if (auto v = doc->get_string("build.macos_deployment_target"))
        m.buildConfig.macosDeploymentTarget = *v;

    // Surface unsupported [build] keys instead of silently dropping them.
    // #296 is #131's footgun one section over: `[build] defines` on an mcpp
    // that did not know the key simply vanished, and the build then failed
    // much later with a module-graph divergence naming neither the key nor
    // the manifest. An unknown key in a section this central is always a typo
    // or a version mismatch, never an intentional no-op — so say so. Same
    // policy as [targets.<name>] above: a warning, an error under --strict.
    //
    // MUST stay in sync with the `doc->get_*("build.<key>")` reads above.
    static constexpr std::string_view kKnownBuildKeys[] = {
        "allow_host_libs", "bmi_schedule", "build_program_timeout", "c_standard",
        "cache", "cflags", "cxxflags", "cxx_runtime", "default-profile", "defines",
        "dialect_cxxflags", "flags", "include_dirs", "include_dirs_after",
        "jobs", "ldflags", "macos_deployment_target", "module_extensions", "profile",
        "sources", "static_stdlib", "target",
    };
    if (auto* bt = doc->get_table("build")) {
        for (auto& [key, _] : *bt) {
            bool known = false;
            for (auto k : kKnownBuildKeys) if (key == k) { known = true; break; }
            if (!known) {
                // ⚠️ THE LIST IN THE MESSAGE IS THE SAME LIST. It used to be a
                // THIRD hand-written copy and had already drifted from both
                // others: it named neither `jobs` nor `bmi_schedule`, while
                // `kKnownBuildKeys` carried a `schedule` that nothing reads and
                // omitted the `bmi_schedule` the parser actually looks for.
                //
                // The user-visible result was the worst possible one: writing
                // the documented `bmi_schedule = "on"` produced
                //   [build] has unsupported key 'bmi_schedule' (ignored)
                // which is FALSE — it is read a few lines above — so the only
                // way to turn the feature on told you it had been ignored,
                // while the typo `schedule` was accepted in silence.
                std::string supported;
                for (auto k : kKnownBuildKeys) {
                    if (!supported.empty()) supported += ", ";
                    supported += k;
                }
                m.schemaWarnings.push_back(std::format(
                    "[build] has unsupported key '{}' (ignored). Supported keys: {}.",
                    key, supported));
            }
        }
    }

    for (auto const& flag : m.buildConfig.cxxflags) {
        if (starts_with_std_flag(flag)) {
            return std::unexpected(error(origin,
                std::format("build.cxxflags contains '{}'; use [package].standard to configure the C++ language standard",
                            flag)));
        }
    }

    // [runtime] — launch-time requirements.
    if (auto v = doc->get_string_array("runtime.library_dirs")) {
        for (auto& s : *v) m.runtimeConfig.libraryDirs.emplace_back(s);
    }
    if (auto v = doc->get_string_array("runtime.dlopen_libs"))
        m.runtimeConfig.dlopenLibs = *v;
    if (auto v = doc->get_string_array("runtime.capabilities"))
        m.runtimeConfig.capabilities = *v;
    if (auto v = doc->get_string_array("runtime.provides"))
        m.runtimeConfig.provides = *v;
    auto read_runtime_paths = [&](std::string_view key,
                                  std::vector<std::filesystem::path>& out) {
        if (auto v = doc->get_string_array(key))
            for (auto& s : *v) out.emplace_back(s);
    };
    if (auto v = doc->get_string_array("runtime.libraries"))
        m.runtimeConfig.linkIntent.libraries = *v;
    read_runtime_paths("runtime.link_library_dirs",
                       m.runtimeConfig.linkIntent.linkLibraryDirs);
    read_runtime_paths("runtime.transitive_needed_dirs",
                       m.runtimeConfig.linkIntent.transitiveNeededDirs);
    read_runtime_paths("runtime.runtime_search_dirs",
                       m.runtimeConfig.linkIntent.runtimeSearchDirs);
    if (auto v = doc->get_string_array("runtime.frameworks"))
        m.runtimeConfig.linkIntent.frameworks = *v;
    read_runtime_paths("runtime.deploy_files",
                       m.runtimeConfig.linkIntent.deployFiles);

    auto table_string = [&](const t::Table& table, std::string_view key,
                            std::string& out) -> bool {
        auto it = table.find(std::string(key));
        if (it == table.end()) return true;
        if (!it->second.is_string()) return false;
        out = it->second.as_string();
        return true;
    };
    if (auto* requirements = doc->get("runtime.requirements")) {
        if (!requirements->is_array()) {
            return std::unexpected(error(origin,
                "runtime.requirements must be an array of tables"));
        }
        std::size_t index = 0;
        for (auto const& value : requirements->as_array()) {
            ++index;
            if (!value.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "runtime.requirements[{}] must be a table", index)));
            }
            RuntimeRequirement requirement;
            auto const& table = value.as_table();
            for (auto const& [key, _] : table) {
                if (key != "kind" && key != "value" && key != "phase"
                    && key != "required" && key != "discovery") {
                    return std::unexpected(error(origin, std::format(
                        "runtime.requirements[{}] has unsupported key '{}'",
                        index, key)));
                }
            }
            if (!table_string(table, "kind", requirement.kind)
                || !table_string(table, "value", requirement.value)
                || !table_string(table, "phase", requirement.phase)) {
                return std::unexpected(error(origin, std::format(
                    "runtime.requirements[{}] kind/value/phase must be strings",
                    index)));
            }
            table_string(table, "discovery", requirement.discovery);
            if (auto it = table.find("required"); it != table.end()) {
                if (!it->second.is_bool()) {
                    return std::unexpected(error(origin, std::format(
                        "runtime.requirements[{}].required must be a boolean",
                        index)));
                }
                requirement.required = it->second.as_bool();
            }
            if (requirement.kind.empty() || requirement.value.empty()) {
                return std::unexpected(error(origin, std::format(
                    "runtime.requirements[{}] requires non-empty kind and value",
                    index)));
            }
            if (requirement.phase != "link" && requirement.phase != "run") {
                return std::unexpected(error(origin, std::format(
                    "runtime.requirements[{}].phase must be 'link' or 'run'",
                    index)));
            }
            m.runtimeConfig.requirements.push_back(std::move(requirement));
        }
    }
    if (auto* artifacts = doc->get("runtime.artifacts")) {
        if (!artifacts->is_array()) {
            return std::unexpected(error(origin,
                "runtime.artifacts must be an array of tables"));
        }
        std::size_t index = 0;
        for (auto const& value : artifacts->as_array()) {
            ++index;
            if (!value.is_table()) {
                return std::unexpected(error(origin, std::format(
                    "runtime.artifacts[{}] must be a table", index)));
            }
            RuntimeArtifact artifact;
            std::string path;
            auto const& table = value.as_table();
            for (auto const& [key, _] : table) {
                if (key != "role" && key != "path" && key != "provenance"
                    && key != "abi" && key != "digest"
                    && key != "host_fingerprint") {
                    return std::unexpected(error(origin, std::format(
                        "runtime.artifacts[{}] has unsupported key '{}'",
                        index, key)));
                }
            }
            if (!table_string(table, "role", artifact.role)
                || !table_string(table, "path", path)
                || !table_string(table, "provenance", artifact.provenance)
                || !table_string(table, "abi", artifact.abi)
                || !table_string(table, "digest", artifact.digest)
                || !table_string(table, "host_fingerprint",
                                 artifact.hostFingerprint)) {
                return std::unexpected(error(origin, std::format(
                    "runtime.artifacts[{}] fields must be strings", index)));
            }
            if (artifact.role.empty() || path.empty()
                || artifact.provenance.empty()) {
                return std::unexpected(error(origin, std::format(
                    "runtime.artifacts[{}] requires role, path, and provenance",
                    index)));
            }
            artifact.path = std::move(path);
            m.runtimeConfig.artifacts.push_back(std::move(artifact));
        }
    }
    // [runtime.<capability>] provider = "<pkg>" — explicit provider override.
    if (auto* rt = doc->get_table("runtime"); rt && !rt->empty()) {
        for (auto& [rk, rv] : *rt) {
            if (!rv.is_table()) continue;  // flat keys handled above
            auto& tt = rv.as_table();
            if (auto it = tt.find("provider"); it != tt.end() && it->second.is_string())
                m.runtimeConfig.providerOverrides[rk] = it->second.as_string();
        }
    }

    // [resources] — metadata and assets compiled into the artifact (mcpp#365).
    // See types.cppm for why the section is not named after Windows and why it
    // is not conditionable.
    if (auto v = doc->get_string("resources.icon"))          m.resources.icon = *v;
    if (auto v = doc->get_string_array("resources.files"))
        for (auto& s : *v) m.resources.files.emplace_back(s);
    if (auto v = doc->get_string_array("resources.extra-inputs"))
        for (auto& s : *v) m.resources.extraInputs.emplace_back(s);
    if (auto* res = doc->get_table("resources"); res && !res->empty()) {
        // `version-info` is two things by design: `= false` opts out, and a
        // `[resources.version-info]` table both opts IN and supplies overrides.
        if (auto it = res->find("version-info"); it != res->end()) {
            if (it->second.is_bool()) {
                m.resources.versionInfo = it->second.as_bool();
            } else if (it->second.is_table()) {
                m.resources.versionInfo = true;
                auto& vi = it->second.as_table();
                auto str = [&](const char* k, std::string& dst) {
                    if (auto f = vi.find(k); f != vi.end() && f->second.is_string())
                        dst = f->second.as_string();
                };
                str("company",           m.resources.info.company);
                str("product",           m.resources.info.product);
                str("description",       m.resources.info.description);
                str("copyright",         m.resources.info.copyright);
                str("original-filename", m.resources.info.originalFilename);
                str("internal-name",     m.resources.info.internalName);
                static constexpr std::string_view kKnownVersionInfoKeys[] = {
                    "company", "product", "description", "copyright",
                    "original-filename", "internal-name",
                };
                for (auto& [k, _] : vi) {
                    bool known = false;
                    for (auto kk : kKnownVersionInfoKeys) if (k == kk) { known = true; break; }
                    if (!known)
                        m.schemaWarnings.push_back(std::format(
                            "[resources.version-info] has unsupported key '{}' (ignored). "
                            "Fields: company, product, description, copyright, "
                            "original-filename, internal-name.", k));
                }
            } else {
                return std::unexpected(error(origin,
                    "[resources].version-info must be a boolean (`false` to opt out) "
                    "or a [resources.version-info] table of overrides"));
            }
        }
        static constexpr std::string_view kKnownResourceKeys[] = {
            "icon", "files", "extra-inputs", "version-info",
        };
        for (auto& [k, _] : *res) {
            bool known = false;
            for (auto kk : kKnownResourceKeys) if (k == kk) { known = true; break; }
            if (!known)
                m.schemaWarnings.push_back(std::format(
                    "[resources] has unsupported key '{}' (ignored). Keys: icon, "
                    "files, extra-inputs, version-info.", k));
        }
    }

    // [lib] — library root convention (cargo-style).
    if (auto v = doc->get_string("lib.path")) {
        m.lib.path = *v;
    }

    // [pack] — `mcpp pack` configuration. See docs/35-pack-design.md.
    if (auto v = doc->get_string("pack.default_mode")) {
        const auto& s = *v;
        if (s != "static" && s != "bundle-project" && s != "bundle-all") {
            return std::unexpected(error(origin, std::format(
                "[pack].default_mode = '{}' invalid; expected "
                "'static' | 'bundle-project' | 'bundle-all'", s)));
        }
        m.packConfig.defaultMode = s;
    }
    if (auto v = doc->get_string_array("pack.include"))
        m.packConfig.include = *v;
    if (auto v = doc->get_string_array("pack.exclude"))
        m.packConfig.exclude = *v;
    // [pack.bundle-project] sub-table for fine-grained PEP 600 overrides.
    if (auto v = doc->get_string_array("pack.bundle-project.also_skip"))
        m.packConfig.alsoSkip = *v;
    if (auto v = doc->get_string_array("pack.bundle-project.force_bundle"))
        m.packConfig.forceBundle = *v;

    // [target.<triple>] — per-target overrides. We accept both GCC
    // (x86_64-linux-musl) and Rust-style (x86_64-unknown-linux-musl)
    // triple forms; the latter is canonicalised by stripping the
    // `-unknown-` segment so both keys map to the same entry.
    auto canon_triple = [](std::string s) {
        constexpr std::string_view kUnknown = "-unknown-";
        if (auto p = s.find(kUnknown); p != std::string::npos)
            s.replace(p, kUnknown.size(), "-");
        return s;
    };
    if (auto* tt = doc->get_table("target")) {
        for (auto& [triple, val] : *tt) {
            if (!val.is_table()) continue;
            auto& body = val.as_table();
            TargetEntry e;
            if (auto it = body.find("toolchain"); it != body.end() && it->second.is_string())
                e.toolchain = it->second.as_string();
            if (auto it = body.find("linkage"); it != body.end() && it->second.is_string()) {
                e.linkage = it->second.as_string();
                if (e.linkage != "static" && e.linkage != "dynamic") {
                    return std::unexpected(error(origin, std::format(
                        "[target.{}].linkage = '{}' is invalid; expected 'static' or 'dynamic'",
                        triple, e.linkage)));
                }
            }
            // #336 — the C++ runtime contract, same axis as `linkage` (both
            // describe the produced artifact's run-time dependencies) and so
            // the same scoping. Only the scalar form here: a per-target,
            // per-role matrix is a surface nobody has asked for, and
            // [build].cxx_runtime already covers the role split.
            if (auto it = body.find("cxx_runtime"); it != body.end() && it->second.is_string()) {
                e.cxxRuntime = it->second.as_string();
                if (e.cxxRuntime != "self-contained"
                    && e.cxxRuntime != "toolchain-coupled"
                    && e.cxxRuntime != "host-coupled") {
                    return std::unexpected(error(origin, std::format(
                        "[target.{}].cxx_runtime = '{}' is invalid; expected one of "
                        "\"self-contained\", \"toolchain-coupled\", \"host-coupled\"",
                        triple, e.cxxRuntime)));
                }
            }
            // Unsupported keys are REPORTED, not dropped. `[targets.<name>]`
            // has done this since #249; this table did not, so a key that looks
            // plausible — `cxx_runtime_tests` was the real one — was accepted
            // in silence and had no effect (#418). A configuration key that
            // does nothing is worse than one that does not exist.
            static constexpr std::string_view kKnownTargetKeys[] = {
                "build", "cxx_runtime", "linkage", "toolchain",
            };
            for (auto& [key, _] : body) {
                bool known = false;
                for (auto k : kKnownTargetKeys) if (key == k) { known = true; break; }
                if (known) continue;
                std::string supported;
                for (auto k : kKnownTargetKeys) {
                    if (!supported.empty()) supported += ", ";
                    supported += k;
                }
                m.schemaWarnings.push_back(std::format(
                    "[target.{}] has unsupported key '{}' (ignored). Supported keys: {}. "
                    "Per-role contracts go in [build].cxx_runtime's table form.",
                    triple, key, supported));
            }
            m.targetOverrides[canon_triple(triple)] = std::move(e);

            // [target.<predicate>.{build,dependencies,...}] — platform-conditional
            // config (L1). `triple` is the predicate key (cfg(...) or a bare
            // triple); stored deferred, evaluated against the resolved target in
            // prepare_build.
            ConditionalConfig cc;
            cc.predicate = triple;
            if (auto bit = body.find("build"); bit != body.end() && bit->second.is_table()) {
                auto& bt = bit->second.as_table();
                auto read_list = [&](const char* key, std::vector<std::string>& out) {
                    if (auto f = bt.find(key); f != bt.end() && f->second.is_array())
                        for (auto& v : f->second.as_array())
                            if (v.is_string()) out.push_back(v.as_string());
                };
                auto read_paths = [&](const char* key,
                                      std::vector<std::filesystem::path>& out) {
                    if (auto f = bt.find(key); f != bt.end() && f->second.is_array())
                        for (auto& v : f->second.as_array())
                            if (v.is_string()) out.emplace_back(v.as_string());
                };
                read_list("cflags",   cc.inputs.cflags);
                read_list("cxxflags", cc.inputs.cxxflags);
                read_list("ldflags",  cc.inputs.ldflags);
                read_list("sources",  cc.inputs.sources);
                // #296: package-level macros are a build input like any other,
                // so the cfg axis carries them too — a platform-only macro
                // (`[target.'cfg(windows)'.build] defines = ["USE_WIN32"]`)
                // must reach the scan and the compile exactly like an
                // unconditional one.
                read_list("defines",  cc.inputs.defines);
                // #258: per-glob flags and include dirs, through the SAME entry
                // grammar `[build].flags` uses — a conditional section is just
                // a set of build inputs, so it reads the same way.
                read_paths("include_dirs",       cc.inputs.includeDirs);
                read_paths("include_dirs_after", cc.inputs.includeDirsAfter);
                if (auto f = bt.find("flags"); f != bt.end()) {
                    if (auto err = parse_glob_flags_value(
                            f->second,
                            std::format("[target.{}.build].flags", triple),
                            cc.inputs.globFlags))
                        return std::unexpected(error(origin, *err));
                }
                // The conditional axis carries BuildInputs and nothing else, so
                // its vocabulary is exactly that struct's members — a key
                // outside it (`static_stdlib`, `target`, a profile knob) is not
                // conditionable and would otherwise vanish without a word, the
                // #296 failure mode. MUST stay in sync with the reads above and
                // with types.cppm's BuildInputs.
                static constexpr std::string_view kKnownConditionalBuildKeys[] = {
                    "cflags", "cxxflags", "defines", "flags",
                    "include_dirs", "include_dirs_after", "ldflags", "sources",
                };
                for (auto& [key, _] : bt) {
                    bool known = false;
                    for (auto k : kKnownConditionalBuildKeys)
                        if (key == k) { known = true; break; }
                    if (!known) {
                        m.schemaWarnings.push_back(std::format(
                            "[target.{}.build] has unsupported key '{}' (ignored). "
                            "A conditional section may only contribute build INPUTS: "
                            "sources, cflags, cxxflags, ldflags, defines, flags, "
                            "include_dirs, include_dirs_after. Selection knobs "
                            "(target, linkage) and profile settings are resolved "
                            "before the predicate is evaluated and cannot be "
                            "conditioned; the C++ runtime contract IS "
                            "per-target, but it is spelled "
                            "[target.<triple>].cxx_runtime, beside `linkage`.",
                            triple, key));
                    }
                }
            }
            // [target.<predicate>.{dependencies,dev-dependencies,build-dependencies}]
            // parsed via the shared table-based loader (same selectors/namespaces
            // as the global [dependencies]) into the deferred config.
            auto read_deps = [&](const char* key, std::map<std::string, DependencySpec>& out)
                -> std::expected<void, ManifestError>
            {
                if (auto f = body.find(key); f != body.end() && f->second.is_table())
                    return load_deps_table(key, f->second.as_table(), out);
                return {};
            };
            if (auto r = read_deps("dependencies",       cc.dependencies);     !r) return std::unexpected(r.error());
            if (auto r = read_deps("dev-dependencies",   cc.devDependencies);  !r) return std::unexpected(r.error());
            if (auto r = read_deps("build-dependencies", cc.buildDependencies); !r) return std::unexpected(r.error());
            // [target.<predicate>.feature-deps.<feature>] (#359). The feature
            // itself is registered UNCONDITIONALLY: whether the platform
            // matches decides what the feature pulls in, not whether the
            // feature exists. Otherwise requesting it on a non-matching
            // platform would trip the unknown-feature diagnostic.
            if (auto f = body.find("feature-deps");
                f != body.end() && f->second.is_table()) {
                for (auto& [fname, fval] : f->second.as_table()) {
                    if (!fval.is_table()) continue;
                    if (auto r = load_deps_table(
                            std::format("[target.{}.feature-deps.{}]", triple, fname),
                            fval.as_table(), cc.featureDeps[std::string(fname)]); !r)
                        return std::unexpected(r.error());
                    m.featuresMap.try_emplace(std::string(fname),
                                              std::vector<std::string>{});
                }
            }
            if (!cc.inputs.cflags.empty() || !cc.inputs.cxxflags.empty()
                || !cc.inputs.ldflags.empty() || !cc.inputs.sources.empty()
                || !cc.inputs.defines.empty()
                || !cc.inputs.globFlags.empty() || !cc.inputs.includeDirs.empty()
                || !cc.inputs.includeDirsAfter.empty()
                || !cc.dependencies.empty() || !cc.devDependencies.empty()
                || !cc.buildDependencies.empty() || !cc.featureDeps.empty())
                m.conditionalConfigs.push_back(std::move(cc));
        }
    }

    // [workspace] — multi-package workspace support (0.0.11+).
    if (doc->get_table("workspace")) {
        m.workspace.present = true;
        if (auto v = doc->get_string_array("workspace.members"))
            m.workspace.members = *v;
        if (auto v = doc->get_string_array("workspace.exclude"))
            m.workspace.exclude = *v;

        // [workspace.dependencies] — versions that members inherit via .workspace = true.
        if (auto* wdeps = doc->get_table("workspace.dependencies")) {
            for (auto& [k, v] : *wdeps) {
                if (v.is_string()) {
                    auto selector = parse_dep_selector(
                        "workspace.dependencies", k, k);
                    if (!selector) return std::unexpected(selector.error());
                    if (auto r = assign_dep("workspace.dependencies",
                                            m.workspace.dependencies,
                                            *selector, v,
                                            k.find('.') != std::string::npos,
                                            false); !r) {
                        return std::unexpected(r.error());
                    }
                    continue;
                }
                if (!v.is_table()) continue;
                auto& sub = v.as_table();
                // #224: a flat (non-namespaced, non-dotted) key whose value is
                // an inline dep-spec table — `ylib = { path = "..." }` — must
                // be recognized as ONE dependency spec here, same as
                // load_deps_table's (1') branch does for [dependencies].
                // Without this check it falls into load_selector_dep_table
                // below, which treats "path"/"version"/etc. as nested
                // selector path components and silently mis-files the entry
                // under a key like "ylib.path" instead of "ylib".
                if (looks_like_inline_dep_spec(sub)) {
                    auto selector = parse_dep_selector(
                        "workspace.dependencies", k, k);
                    if (!selector) return std::unexpected(selector.error());
                    if (auto r = assign_dep("workspace.dependencies",
                                            m.workspace.dependencies,
                                            *selector, v,
                                            k.find('.') != std::string::npos,
                                            false); !r) {
                        return std::unexpected(r.error());
                    }
                    continue;
                }
                if (is_namespace_table("workspace.dependencies", k)) {
                    if (auto r = load_nested_dep_table("workspace.dependencies",
                                                       m.workspace.dependencies,
                                                       k, k, sub); !r) {
                        return std::unexpected(r.error());
                    }
                } else {
                    if (auto r = load_selector_dep_table("workspace.dependencies",
                                                         m.workspace.dependencies,
                                                         k, sub); !r) {
                        return std::unexpected(r.error());
                    }
                }
            }
        }
    }

    // [indices] — custom package index repositories.
    //
    // Accepted forms:
    //   acme = "git@gitlab.example.com:platform/mcpp-index.git"       # short: value = url
    //   acme-stable = { url = "git@...", tag = "v2.0" }               # long: inline table
    //   local-dev = { path = "<path>/my-packages" }                  # local path
    //   mcpplibs = { url = "https://...", rev = "abc123" }            # pin built-in
    if (auto* indices_t = doc->get_table("indices")) {
        for (auto& [k, v] : *indices_t) {
            // R6: `default` (canonical spelling) or the empty-quoted key `""`
            // redirects the DEFAULT namespace (bare `gtest = "1.15.2"` deps,
            // not routed through any explicit namespace prefix) rather than
            // declaring a literal index named "default". Normalize both
            // spellings to kDefaultNamespace so prepare.cppm's lookups
            // (usesBuiltinIndex / findIndexForNs) key on the same string
            // dependency resolution already uses for the default namespace.
            bool isDefaultAlias = (k.empty() || k == "default");
            std::string key = isDefaultAlias ? std::string(kDefaultNamespace) : k;
            mcpp::pm::IndexSpec spec;
            // `spec.name` mirrors the (normalized) map key: several call
            // sites (config.cppm's ensure_project_index_dir, prepare.cppm's
            // install-target formatting) key xlings' project-index identity
            // off whichever of {map key, spec.name} they happen to iterate,
            // so the two must always agree — same invariant as every other
            // [indices] entry, "default"/"" included.
            spec.name = key;

            if (v.is_string()) {
                // Short form: key = "url"
                spec.url = v.as_string();
            } else if (v.is_table()) {
                auto& sub = v.as_table();
                if (auto it = sub.find("url");    it != sub.end() && it->second.is_string()) spec.url    = it->second.as_string();
                if (auto it = sub.find("rev");    it != sub.end() && it->second.is_string()) spec.rev    = it->second.as_string();
                if (auto it = sub.find("tag");    it != sub.end() && it->second.is_string()) spec.tag    = it->second.as_string();
                if (auto it = sub.find("branch"); it != sub.end() && it->second.is_string()) spec.branch = it->second.as_string();
                if (auto it = sub.find("path");   it != sub.end() && it->second.is_string()) spec.path   = it->second.as_string();
                if (auto it = sub.find("artifact"); it != sub.end() && it->second.is_string()) spec.artifact = it->second.as_string();
                if (auto it = sub.find("source");   it != sub.end() && it->second.is_string()) spec.source   = it->second.as_string();
                if (spec.url.empty() && spec.path.empty()) {
                    return std::unexpected(error(origin, std::format(
                        "[indices].{} must specify 'url' or 'path'", k)));
                }
            } else {
                return std::unexpected(error(origin, std::format(
                    "[indices].{} must be a string (url) or inline table", k)));
            }

            // R6's design goal for the default-namespace redirect is
            // pointing the default namespace at a LOCAL index checkout
            // (`path = ...`), so the index repo itself can test module
            // packages against an unpublished tree. A `url` form silently
            // no-ops instead of redirecting: is_builtin() (index_spec.cppm)
            // still returns true whenever `path` is empty, so every
            // consumer (prepare.cppm's readLuaContent/useProjectEnv/
            // findRawInstalled, config.cppm's ensure_project_index_dir)
            // falls through to the builtin/global registry and just
            // ignores the configured url — no error, nothing redirected.
            // Reject it loudly at parse time instead of accepting a no-op.
            // This only fires for the `default`/`""` ALIAS; a literal
            // `[indices] mcpplibs = { url = ..., rev = ... }` (pin the
            // builtin registry to a commit) is a different key spelling
            // and must keep working unchanged.
            if (isDefaultAlias && spec.path.empty()) {
                return std::unexpected(error(origin, std::format(
                    "[indices] default-namespace redirect currently supports only "
                    "'path = ...' (a local index checkout), not 'url'; got url = '{}'. "
                    "Use a named namespace for a remote custom index.", spec.url)));
            }

            // `default`, `""`, and a literal `mcpplibs` key all normalize to
            // the same map slot (kDefaultNamespace). Without a duplicate
            // check, declaring more than one of these silently clobbers
            // whichever was assigned last, order-dependent on TOML table
            // iteration. Fail loudly instead.
            if (m.indices.contains(key)) {
                return std::unexpected(error(origin, std::format(
                    "[indices] '{}' collides with an existing default-namespace entry "
                    "(default/\"\"/mcpplibs all map to the same slot)", k)));
            }

            m.indices[key] = std::move(spec);
        }
    }

    return m;
}

// M5.0: inject defaults and auto-infer targets when fields are absent.
// Mutates manifest in-place; called from load() with the project root.
namespace {

void apply_defaults_and_infer(Manifest& m, const std::filesystem::path& root) {
    // Default sources glob (covers .cppm/.cpp/.cc/.c plus assembly under
    // src/). Assembly in the tree almost certainly wants building; a project
    // that vendors foreign-syntax .asm can `!`-exclude it.
    const auto extTable =
        mcpp::extension_table_for(m.buildConfig.moduleExtensions);
    if (m.buildConfig.sources.empty()) {
        // Derived from the extension table rather than written beside it —
        // otherwise declaring `module_extensions = [".ixx"]` would change how
        // `.ixx` is TREATED without changing whether it is FOUND, and the key
        // would appear to do nothing.
        m.buildConfig.sources = mcpp::default_source_globs(extTable);
        m.modules.sources = m.buildConfig.sources;   // legacy mirror
        m.inferredNotes.push_back(mcpp::default_source_globs_note(extTable));
    }

    // Default include_dirs: ["include"] iff <root>/include/ exists.
    if (m.buildConfig.includeDirs.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(root / "include", ec)) {
            m.buildConfig.includeDirs.push_back("include");
            m.inferredNotes.push_back("include_dirs [include]");
        }
    }

    // Auto-target inference (only when no [targets] declared).
    if (m.targets.empty()) {
        std::error_code ec;
        auto mainCpp = root / "src" / "main.cpp";
        bool hasMain   = std::filesystem::exists(mainCpp, ec);

        // "Is there a module interface under src/" — asked through the table,
        // so a library whose interfaces are all `.ixx` still infers a lib
        // target instead of silently having none.
        //
        // The extension that answered is kept for the inferred-note: saying
        // ".cppm" when the project's interfaces are `.ixx` would be a note that
        // names the wrong thing, and a `.cppm` project still prints exactly
        // what it always did.
        std::string moduleInterfaceExt;
        if (std::filesystem::is_directory(root / "src", ec)) {
            for (auto& e : std::filesystem::recursive_directory_iterator(root / "src", ec)) {
                if (ec) break;
                if (e.is_regular_file(ec) && !ec
                    && mcpp::produces_bmi(mcpp::classify(e.path(), extTable))) {
                    moduleInterfaceExt = e.path().extension().string();
                    break;
                }
            }
        }
        const bool hasModuleInterface = !moduleInterfaceExt.empty();

        if (hasMain) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Binary;
            t.main = "src/main.cpp";
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (bin from src/main.cpp)", m.package.name));
        } else if (hasModuleInterface) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Library;
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (lib from {} in src/)", m.package.name, moduleInterfaceExt));
        }
        // If neither, no auto-target — caller will error if it needs one.
    }
}

} // namespace

std::expected<Manifest, ManifestError> load(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) {
        return std::unexpected(ManifestError{
            std::format("cannot open '{}'", path.string()),
            path, 0, 0});
    }
    std::stringstream ss;
    ss << is.rdbuf();
    auto m = parse_string(ss.str(), path);
    if (!m) return m;

    // M5.0: defaults + target inference (uses filesystem context relative to mcpp.toml).
    apply_defaults_and_infer(*m, path.parent_path());
    return m;
}

std::string default_template(std::string_view packageName) {
    // M5.0: minimal mcpp.toml — convention over configuration.
    // sources / target / standard are all auto-inferred. Users add fields as
    // they grow out of the defaults.
    return std::format(R"([package]
name        = "{}"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
)", packageName);
}

std::expected<std::string, std::string>
upsert_dependency_text(std::string_view source,
                       const DependencyTextEdit& edit) {
    auto original = parse_string(source);
    if (!original) {
        return std::unexpected(std::format(
            "invalid manifest before dependency edit: {}",
            original.error().format()));
    }

    if (edit.namespace_.empty()) {
        return std::unexpected(
            "dependency edit requires a canonical non-empty namespace");
    }
    const auto spelling = edit.namespace_ == kDefaultNamespace
        ? edit.shortName
        : std::format("{}.{}", edit.namespace_, edit.shortName);
    auto selector = mcpp::pm::parse_package_selector(spelling);
    if (!selector) return std::unexpected(selector.error().message);
    auto coordinate = mcpp::pm::normalize_package_selector(*selector);
    if (coordinate.namespace_ != edit.namespace_
        || coordinate.shortName != edit.shortName) {
        return std::unexpected(std::format(
            "dependency edit identity ({}, {}) is not canonical",
            edit.namespace_, edit.shortName));
    }
    if (edit.version.empty()) {
        return std::unexpected("dependency edit version is empty");
    }
    for (unsigned char ch : edit.version) {
        if (ch < 0x20 || ch == 0x7f || ch == '"' || ch == '\\') {
            return std::unexpected(
                "dependency edit version contains an unsafe TOML character");
        }
    }
    for (auto const& featureName : edit.features) {
        auto feature = mcpp::pm::parse_package_selector(featureName);
        if (!feature || feature->namespace_) {
            return std::unexpected(std::format(
                "dependency feature '{}' must be one safe atom", featureName));
        }
    }

    std::string value;
    if (edit.features.empty()) {
        value = std::format("\"{}\"", edit.version);
    } else {
        std::string featureList;
        for (auto const& feature : edit.features) {
            if (!featureList.empty()) featureList += ", ";
            featureList += std::format("\"{}\"", feature);
        }
        value = std::format(
            "{{ version = \"{}\", features = [{}] }}",
            edit.version, featureList);
    }

    struct SectionSpan {
        std::size_t headerStart;
        std::size_t headerEnd;
        std::size_t bodyStart;
        std::size_t sectionEnd;
    };

    auto trim = [](std::string_view line) {
        while (!line.empty()
               && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty()
               && (line.back() == ' ' || line.back() == '\t'
                   || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        return line;
    };

    auto section_header_matches = [&](std::string_view line,
                                      std::string_view header) {
        line = trim(line);
        if (!line.starts_with(header)) return false;
        line.remove_prefix(header.size());
        line = trim(line);
        return line.empty() || line.front() == '#';
    };

    auto find_section = [&](std::string_view text,
                            std::string_view header)
        -> std::optional<SectionSpan> {
        std::optional<SectionSpan> result;
        std::size_t lineStart = 0;
        while (lineStart <= text.size()) {
            auto newline = text.find('\n', lineStart);
            auto lineEnd = newline == std::string_view::npos
                ? text.size() : newline;
            auto line = text.substr(lineStart, lineEnd - lineStart);
            if (!result && section_header_matches(line, header)) {
                result = SectionSpan{
                    .headerStart = lineStart,
                    .headerEnd = lineEnd,
                    .bodyStart = newline == std::string_view::npos
                        ? lineEnd : lineEnd + 1,
                    .sectionEnd = text.size(),
                };
            } else if (result) {
                auto stripped = trim(line);
                if (!stripped.empty() && stripped.front() == '[') {
                    result->sectionEnd = lineStart;
                    break;
                }
            }
            if (newline == std::string_view::npos) break;
            lineStart = newline + 1;
        }
        return result;
    };

    auto key_line_span = [&](std::string_view text,
                             const SectionSpan& section,
                             std::string_view key)
        -> std::optional<std::pair<std::size_t, std::size_t>> {
        const auto quotedKey = std::format("\"{}\"", key);
        std::size_t lineStart = section.bodyStart;
        while (lineStart < section.sectionEnd) {
            auto newline = text.find('\n', lineStart);
            auto lineEnd = newline == std::string_view::npos
                ? text.size() : newline;
            if (lineEnd > section.sectionEnd) lineEnd = section.sectionEnd;
            auto line = trim(text.substr(lineStart, lineEnd - lineStart));
            auto assignment_for = [&](std::string_view candidate) {
                if (!line.starts_with(candidate)) return false;
                auto rest = line.substr(candidate.size());
                while (!rest.empty()
                       && (rest.front() == ' ' || rest.front() == '\t')) {
                    rest.remove_prefix(1);
                }
                return !rest.empty() && rest.front() == '=';
            };
            if (!line.starts_with('#')
                && (assignment_for(key) || assignment_for(quotedKey))) {
                return std::pair{lineStart, lineEnd};
            }
            if (newline == std::string_view::npos
                || newline >= section.sectionEnd) break;
            lineStart = newline + 1;
        }
        return std::nullopt;
    };

    std::string text(source);
    const std::string table = edit.dev
        ? "dev-dependencies" : "dependencies";
    const std::string baseHeader = std::format("[{}]", table);
    const bool defaultNamespace = edit.namespace_ == kDefaultNamespace;

    // Retained flat dotted spellings are removed before canonical namespace
    // subtable emission. The parsed identity gate above ensures this edits the
    // exact PackageId, not a short-name substring match.
    if (!defaultNamespace) {
        if (auto base = find_section(text, baseHeader)) {
            if (auto legacy = key_line_span(text, *base, spelling)) {
                auto eraseEnd = legacy->second;
                if (eraseEnd < text.size() && text[eraseEnd] == '\n')
                    ++eraseEnd;
                text.erase(legacy->first, eraseEnd - legacy->first);
            }
        }
    }

    const std::string sectionHeader = defaultNamespace
        ? baseHeader
        : std::format("[{}.{}]", table, edit.namespace_);
    const std::string line = std::format("{} = {}", edit.shortName, value);
    if (auto section = find_section(text, sectionHeader)) {
        if (auto existing = key_line_span(text, *section, edit.shortName)) {
            text.replace(existing->first, existing->second - existing->first,
                         line);
        } else if (section->bodyStart == section->headerEnd) {
            text.insert(section->bodyStart, "\n" + line);
        } else {
            text.insert(section->bodyStart, line + "\n");
        }
    } else {
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += std::format("\n{}\n{}\n", sectionHeader, line);
    }

    auto reparsed = parse_string(text);
    if (!reparsed) {
        return std::unexpected(std::format(
            "dependency edit produced invalid manifest: {}",
            reparsed.error().format()));
    }
    auto const& dependencies = edit.dev
        ? reparsed->devDependencies : reparsed->dependencies;
    auto exact = std::ranges::find_if(
        dependencies, [&](auto const& item) {
            auto const& dep = item.second;
            return dep.namespace_ == edit.namespace_
                && dep.shortName == edit.shortName;
        });
    if (exact == dependencies.end()
        || exact->second.version != edit.version
        || exact->second.features != edit.features) {
        return std::unexpected(std::format(
            "dependency edit did not materialize exact PackageId ({}, {})",
            edit.namespace_, edit.shortName));
    }
    return text;
}

} // namespace mcpp::manifest
