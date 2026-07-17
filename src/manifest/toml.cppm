// mcpp.manifest:toml — load and validate mcpp.toml.

export module mcpp.manifest.toml;

import mcpp.manifest.types;
import std;
import mcpp.libs.toml;
import mcpp.pm.dep_spec;
import mcpp.pm.compat;
import mcpp.pm.dependency_selector;
import mcpp.pm.index_spec;
import mcpp.platform;

export namespace mcpp::manifest {

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin = "mcpp.toml");
std::expected<Manifest, ManifestError> load(const std::filesystem::path& path);

// For `mcpp new` scaffolding.
std::string default_template(std::string_view packageName);

} // namespace mcpp::manifest

namespace mcpp::manifest {

namespace t = mcpp::libs::toml;

namespace {

ManifestError error(const std::filesystem::path& origin,
                    const std::string& msg,
                    t::Position pos = {0, 0}) {
    return ManifestError{msg, origin, pos.line, pos.column};
}

} // namespace

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin) {
    auto doc = t::parse(content);
    if (!doc) {
        return std::unexpected(error(origin, doc.error().message, doc.error().where));
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
            if (fval.is_array()) {
                for (auto& v : fval.as_array())
                    if (v.is_string()) implied.push_back(v.as_string());
            } else if (fval.is_table()) {
                auto& ft = fval.as_table();
                read_str_array(ft, "implies", implied);
                std::vector<std::string> defs;
                read_str_array(ft, "defines", defs);
                if (!defs.empty()) m.buildConfig.featureDefines[fname] = std::move(defs);
                std::vector<std::string> reqs, provs;
                read_str_array(ft, "requires", reqs);
                read_str_array(ft, "provides", provs);
                if (!reqs.empty())  m.featureRequires[fname] = std::move(reqs);
                if (!provs.empty()) m.featureProvides[fname] = std::move(provs);
            }
            m.featuresMap[fname] = std::move(implied);
        }
    }

    // [package] provides — package-level capabilities (Feature System v2 S3).
    if (auto v = doc->get_string_array("package.provides")) m.provides = *v;

    // [capabilities] cap = "provider" — root-only provider pins.
    if (auto* caps = doc->get_table("capabilities"); caps && !caps->empty()) {
        for (auto& [cap, cval] : *caps)
            if (cval.is_string()) m.capabilityPins[cap] = cval.as_string();
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
    auto is_dep_spec_key = [](std::string_view k) {
        return k == "path"   || k == "version" || k == "git"
            || k == "rev"    || k == "tag"     || k == "branch"
            || k == "features" || k == "workspace" || k == "visibility"
            || k == "backend";
    };
    auto looks_like_inline_dep_spec = [&](const t::Table& sub) {
        if (sub.empty()) return false;
        for (auto& [sk, sv] : sub) {
            if (!is_dep_spec_key(sk)) return false;
        }
        return true;
    };

    auto fill_inline_spec = [&](DependencySpec& spec,
                                std::string_view section,
                                std::string_view fqName,
                                const t::Table& sub) -> std::expected<void, ManifestError>
    {
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

    auto assign_dep = [&](std::string_view section,
                          std::map<std::string, DependencySpec>& out,
                          const mcpp::pm::DependencySelector& selector,
                          const t::Value& value,
                          bool legacyDottedKey)
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

        auto key = selector.stableMapKey;
        if (value.is_string()) {
            spec.version = value.as_string();
        } else if (value.is_table()) {
            auto& sub = value.as_table();
            if (!looks_like_inline_dep_spec(sub)) {
                return std::unexpected(error(origin, std::format(
                    "[{}.{}] must be a version string or table of "
                    "(path/version/git/rev/tag/branch/features/visibility)",
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
                auto selector = mcpp::pm::make_direct_dependency_selector(
                    ns, k, mapKey);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
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
                auto selector = mcpp::pm::resolve_dependency_selector(
                    selectorText,
                    mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
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
            // (1) string value → flat default-ns short version, or
            // (3) legacy "ns.name" = "ver" (dotted key).
            if (v.is_string()) {
                if (k.find('.') != std::string::npos) {
                    auto legacyKey = mcpp::pm::compat::split_legacy_dependency_key(k);
                    auto selector = mcpp::pm::make_direct_dependency_selector(
                        legacyKey.namespace_, legacyKey.shortName, k);
                    if (auto r = assign_dep(section, out, selector, v,
                                            legacyKey.legacyDottedKey); !r)
                        return r;
                    continue;
                }
                auto selector = mcpp::pm::resolve_dependency_selector(
                    k, mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
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
            // The latter is the legacy dotted-key form; same treatment as (3).
            if (looks_like_inline_dep_spec(sub)) {
                if (k.find('.') != std::string::npos) {
                    auto legacyKey = mcpp::pm::compat::split_legacy_dependency_key(k);
                    auto selector = mcpp::pm::make_direct_dependency_selector(
                        legacyKey.namespace_, legacyKey.shortName, k);
                    if (auto r = assign_dep(section, out, selector, v,
                                            legacyKey.legacyDottedKey); !r)
                        return r;
                    continue;
                }
                auto selector = mcpp::pm::resolve_dependency_selector(
                    k, mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                if (auto r = assign_dep(section, out, selector, v, false); !r)
                    return r;
                continue;
            }

            // (2) namespaced or nested subtable.
            //
            // Explicit tables such as `[dependencies.acme]` are namespace
            // roots. Dotted keys written inside the single dependency table,
            // such as `[dependencies] capi.lua = "0.0.3"`, are ordered
            // selectors: mcpplibs.capi/lua first, then capi/lua.
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
    if (auto v = doc->get_bool("build.allow_host_libs")) m.buildConfig.allowHostLibs = *v;
    if (auto v = doc->get_string_array("build.cflags"))   m.buildConfig.cflags   = *v;
    if (auto v = doc->get_string_array("build.cxxflags")) m.buildConfig.cxxflags = *v;
    // Module-graph-global dialect flags (issue #210) — see types.cppm
    // dialect_flags(); this key is the explicit escape hatch for flags the
    // known-list doesn't recognize yet.
    if (auto v = doc->get_string_array("build.dialect_cxxflags"))
        m.buildConfig.dialectCxxflags = *v;
    if (auto v = doc->get_string_array("build.ldflags"))  m.buildConfig.ldflags  = *v;
    if (auto v = doc->get_string("build.c_standard"))     m.buildConfig.cStandard = *v;
    if (auto v = doc->get_string("build.target"))         m.buildConfig.target = *v;
    if (auto v = doc->get_string("build.default-profile")) m.buildConfig.defaultProfile = *v;
    else if (auto v = doc->get_string("build.profile"))   m.buildConfig.defaultProfile = *v;  // accepted alias

    // [xlings] — build environment (L-1). Subsections mirror .xlings.json 1:1.
    if (auto v = doc->get_string_array("xlings.deps"))  m.xlings.deps = *v;
    if (auto v = doc->get_string("xlings.subos"))       m.xlings.subos = *v;
    if (auto* wt = doc->get_table("xlings.workspace"))
        for (auto& [k, val] : *wt)
            if (val.is_string()) m.xlings.workspace[k] = val.as_string();
    if (auto* et = doc->get_table("xlings.envs"))
        for (auto& [k, val] : *et)
            if (val.is_string()) m.xlings.envs[k] = val.as_string();
    if (auto v = doc->get_string("build.macos_deployment_target"))
        m.buildConfig.macosDeploymentTarget = *v;
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
    // [runtime.<capability>] provider = "<pkg>" — explicit provider override.
    if (auto* rt = doc->get_table("runtime"); rt && !rt->empty()) {
        for (auto& [rk, rv] : *rt) {
            if (!rv.is_table()) continue;  // flat keys handled above
            auto& tt = rv.as_table();
            if (auto it = tt.find("provider"); it != tt.end() && it->second.is_string())
                m.runtimeConfig.providerOverrides[rk] = it->second.as_string();
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
                read_list("cflags",   cc.cflags);
                read_list("cxxflags", cc.cxxflags);
                read_list("ldflags",  cc.ldflags);
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
            if (!cc.cflags.empty() || !cc.cxxflags.empty() || !cc.ldflags.empty()
                || !cc.dependencies.empty() || !cc.devDependencies.empty()
                || !cc.buildDependencies.empty())
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
                    if (k.find('.') != std::string::npos) {
                        auto depKey = mcpp::pm::compat::split_legacy_dependency_key(k);
                        auto selector = mcpp::pm::make_direct_dependency_selector(
                            depKey.namespace_, depKey.shortName, k);
                        if (auto r = assign_dep("workspace.dependencies",
                                                m.workspace.dependencies,
                                                selector, v,
                                                depKey.legacyDottedKey); !r) {
                            return std::unexpected(r.error());
                        }
                        continue;
                    }
                    auto selector = mcpp::pm::resolve_dependency_selector(
                        k, mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);
                    if (auto r = assign_dep("workspace.dependencies",
                                            m.workspace.dependencies,
                                            selector, v, false); !r) {
                        return std::unexpected(r.error());
                    }
                    continue;
                }
                if (!v.is_table()) continue;
                if (is_namespace_table("workspace.dependencies", k)) {
                    if (auto r = load_nested_dep_table("workspace.dependencies",
                                                       m.workspace.dependencies,
                                                       k, k, v.as_table()); !r) {
                        return std::unexpected(r.error());
                    }
                } else {
                    if (auto r = load_selector_dep_table("workspace.dependencies",
                                                         m.workspace.dependencies,
                                                         k, v.as_table()); !r) {
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
            mcpp::pm::IndexSpec spec;
            spec.name = k;

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
                if (spec.url.empty() && spec.path.empty()) {
                    return std::unexpected(error(origin, std::format(
                        "[indices].{} must specify 'url' or 'path'", k)));
                }
            } else {
                return std::unexpected(error(origin, std::format(
                    "[indices].{} must be a string (url) or inline table", k)));
            }

            m.indices[k] = std::move(spec);
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
    if (m.buildConfig.sources.empty()) {
        m.buildConfig.sources = {
            "src/**/*.cppm",
            "src/**/*.cpp",
            "src/**/*.cc",
            "src/**/*.c",
            "src/**/*.S",
            "src/**/*.s",
            "src/**/*.asm",
        };
        m.modules.sources = m.buildConfig.sources;   // legacy mirror
        m.inferredNotes.push_back("sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]");
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

        bool hasCppm = false;
        if (std::filesystem::is_directory(root / "src", ec)) {
            for (auto& e : std::filesystem::recursive_directory_iterator(root / "src", ec)) {
                if (ec) break;
                if (e.is_regular_file(ec) && !ec
                    && e.path().extension() == ".cppm") {
                    hasCppm = true; break;
                }
            }
        }

        if (hasMain) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Binary;
            t.main = "src/main.cpp";
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (bin from src/main.cpp)", m.package.name));
        } else if (hasCppm) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Library;
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (lib from .cppm in src/)", m.package.name));
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

} // namespace mcpp::manifest
