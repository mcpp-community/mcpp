// mcpp.pm.commands — package-management CLI commands.
//
// Hosts `cmd_add` / `cmd_remove` / `cmd_update` — the user-facing
// commands that mutate `mcpp.toml` and `mcpp.lock`. Previously sat
// in `cli.cppm`'s detail namespace; PR-R5 of the pm subsystem refactor
// (`.agents/docs/2026-05-08-pm-subsystem-architecture.md`) moves them
// into the pm subsystem so `cli.cppm` is responsible only for the
// global CLI framework + non-pm commands.
//
// The bodies started life as a strict zero-behavior-change move out of
// `cli.cppm`; `cmd_add` has since grown the index existence gate (#305).

export module mcpp.pm.commands;

import std;
import mcpp.config;
import mcpp.fetcher.progress;      // bootstrap progress for load_or_init
import mcpp.manifest;             // kDefaultNamespace alias
import mcpp.lockfile;             // load / write (still via shim)
import mcpp.platform.axis;        // HostPlatform for the published-version check
import mcpp.pm.dep_spec;          // DependencyCoordinate
import mcpp.pm.dependency_selector; // same candidates the manifest parser derives
import mcpp.pm.index_route;       // shared index routing (with mcpp.build.prepare)
import mcpp.pm.index_refresh;     // shared refresh policy (with mcpp.build.prepare)
import mcpp.pm.resolver;          // is_version_constraint
import mcpp.project;              // shared find_manifest_root
import mcpp.ui;
import mcpp.xlings;               // index freshness
import mcpplibs.cmdline;

namespace mcpp::pm::commands::detail {

// Render candidate coordinates the way prepare's resolution error does, so a
// rejected `mcpp add` and a failed `mcpp build` name the same identities.
inline std::string format_tried(
    const std::vector<mcpp::pm::DependencyCoordinate>& candidates) {
    std::string tried;
    for (auto& c : candidates) {
        if (!tried.empty()) tried += ", ";
        tried += c.namespace_.empty()
            ? std::format("(no namespace, {})", c.shortName)
            : std::format("({}, {})", c.namespace_, c.shortName);
    }
    return tried;
}

// A version the descriptor does not publish is worth flagging but not worth
// refusing over: version tables are per-OS, so "absent for this host" is not
// "absent". Say what IS published and let the user decide — the alternative is
// a hard failure on a dependency that resolves fine on the platform it was
// added for.
inline void warn_unpublished_version(std::string_view lua,
                                     std::string_view display,
                                     const std::string& version) {
    if (mcpp::pm::is_version_constraint(version)) return;
    auto versions = mcpp::manifest::list_xpkg_versions(
        lua, mcpp::platform::HostPlatform::current());
    if (versions.empty()) return;
    if (std::ranges::find(versions, version) != versions.end()) return;

    std::string avail;
    for (auto& v : versions) {
        if (!avail.empty()) avail += ", ";
        avail += v;
    }
    mcpp::ui::warning(std::format(
        "'{}' has no version {} published for this platform — available: {}",
        display, version, avail));
}

} // namespace mcpp::pm::commands::detail

export namespace mcpp::pm::commands {

inline int cmd_add(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string spec = parsed.positional(0);
    if (spec.empty()) {
        mcpp::ui::error("usage: mcpp add [<ns>:]<pkg>[@<ver>]");
        return 2;
    }

    // Split @<version> tail.
    std::string nameSpec, version;
    if (auto at = spec.find('@'); at == std::string::npos) {
        nameSpec = spec;
    } else {
        nameSpec = spec.substr(0, at);
        version  = spec.substr(at + 1);
    }

    // Split <ns>:<name>. The colon form is explicit namespace syntax and is
    // written as [dependencies.<ns>]. Without a colon, keep the user's
    // selector spelling in the single [dependencies] table; dotted selectors
    // are resolved later by the manifest parser's candidate rules.
    std::string ns, shortName;
    bool explicitNamespace = false;
    if (auto col = nameSpec.find(':'); col != std::string::npos) {
        explicitNamespace = true;
        ns        = nameSpec.substr(0, col);
        shortName = nameSpec.substr(col + 1);
    } else {
        ns        = std::string{mcpp::manifest::kDefaultNamespace};
        shortName = nameSpec;
    }
    if (shortName.empty()) {
        mcpp::ui::error(std::format("invalid spec '{}': empty package name", spec));
        return 2;
    }

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
    auto manifestPath = *root / "mcpp.toml";

    if (version.empty()) {
        mcpp::ui::error(std::format(
            "package version required: `mcpp add {}@<version>` (M2 supports exact-version only)",
            spec));
        return 2;
    }

    // ── Existence gate (#305) ──────────────────────────────────────────
    // Refuse to write a dependency no index can serve, so a typo fails here
    // instead of halfway into the next `mcpp build`. Two rules keep the gate
    // from refusing packages that are perfectly real:
    //
    //   • It probes the SAME candidates the manifest parser will derive from
    //     the key about to be written. A dotted selector is a namespace path,
    //     not a name: `capi.lua` means `(mcpplibs.capi, lua)` then
    //     `(capi, lua)`, and a literal `(mcpplibs, "capi.lua")` probe can
    //     never match one — `package.name` is a single atomic segment
    //     (SPEC-001 §3.2), so nothing in any index is named "capi.lua".
    //   • It reads through mcpp.pm.index_route, the same routing
    //     `mcpp.build.prepare` resolves dependencies with. A package served by
    //     a project `[indices]` entry therefore counts as present, and a
    //     namespace no readable index can speak for is reported as unverified
    //     rather than rejected: refusing an add is only correct when absence
    //     was actually proven.
    {
        auto selector = explicitNamespace
            ? mcpp::pm::make_direct_dependency_selector(ns, shortName, nameSpec)
            : mcpp::pm::resolve_dependency_selector(
                  nameSpec,
                  mcpp::pm::DependencySelectorMode::OmittedMcpplibsPriority);

        auto cfg = mcpp::config::load_or_init(
            /*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
        if (!cfg) {
            mcpp::ui::error(cfg.error().message);
            return 4;
        }

        auto indices = mcpp::pm::effective_indices(*root);
        mcpp::pm::IndexRoute route{ &indices, *root, &*cfg };
        auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);

        // Does the shared registry answer for any identity we tried? It is the
        // only index a refresh can do anything about — a project
        // `[indices] path = …` is whatever the user has on disk.
        const bool registryInvolved = std::ranges::any_of(selector.candidates,
            [&](const mcpp::pm::DependencyCoordinate& c) {
                auto* idx = route.find_for_ns(c.namespace_);
                return !idx || idx->is_builtin();
            });

        // Only pay for a refresh when the answer was "no" and the registry is
        // the thing that would have answered — a package already on disk costs
        // zero network round-trips. This site had the right shape before #315;
        // it now shares the decision with `mcpp build` instead of re-deriving
        // it, so the two cannot drift on debounce, offline or opt-out.
        if (!found.hit && found.conclusive && registryInvolved) {
            auto xlEnv = mcpp::config::make_xlings_env(*cfg);
            auto d = mcpp::pm::decide_for_miss(
                mcpp::pm::policy_for(*cfg), xlEnv, nameSpec);
            if (auto r = mcpp::pm::apply(d, xlEnv); !r)
                mcpp::ui::warning(r.error());
            if (d.shouldRefresh)
                found = mcpp::pm::lookup_descriptor(route, selector.candidates);
        }

        if (!found.hit && found.conclusive) {
            std::string hint;
            if (!selector.candidates.empty()) {
                for (auto& fqn : mcpp::pm::cross_namespace_matches(
                         route, selector.candidates.front().shortName)) {
                    hint += "\n    " + fqn;
                }
            }
            if (!hint.empty()) {
                hint = "\n  a package with this name exists under another "
                       "namespace:" + hint;
            }
            if (registryInvolved) {
                hint += "\n  hint: `mcpp index update` if it was published "
                        "recently";
            }
            mcpp::ui::error(std::format(
                "package '{}' not found in any configured index\n  tried: {}{}",
                nameSpec, detail::format_tried(selector.candidates), hint));
            return 2;
        }
        if (!found.hit) {
            mcpp::ui::warning(std::format(
                "'{}' could not be verified — no readable index covers that "
                "namespace yet; adding it unchecked", nameSpec));
        } else {
            detail::warn_unpublished_version(found.hit->lua, nameSpec, version);
        }
    }

    std::ifstream in(manifestPath);
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str();

    // Insertion strategy:
    //   - Default namespace → `[dependencies] ... name = "version"` (no quotes).
    //   - Other namespace   → `[dependencies.<ns>] ... name = "version"`,
    //                         creating the subtable if absent.
    // --dev → [dev-dependencies] (test-only deps like gtest; consumed by
    // `mcpp test`, never linked into `mcpp build` app binaries).
    const bool dev = parsed.is_flag_set("dev");
    const std::string table = dev ? "dev-dependencies" : "dependencies";
    const bool isDefaultNs = !explicitNamespace
        || ns == mcpp::manifest::kDefaultNamespace;
    const std::string section = isDefaultNs
        ? std::format("[{}]", table)
        : std::format("[{}.{}]", table, ns);
    const std::string key = explicitNamespace ? shortName : nameSpec;
    auto pos = text.find(section);
    if (pos == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += "\n";
        text += std::format("\n{}\n{} = \"{}\"\n", section, key, version);
    } else {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        text.insert(nl, std::format("\n{} = \"{}\"", key, version));
    }
    {
        std::ofstream os(manifestPath);
        os << text;
    }

    std::string display = explicitNamespace
        ? (isDefaultNs ? shortName : std::format("{}:{}", ns, shortName))
        : nameSpec;
    mcpp::ui::status("Adding", std::format("{} v{} to {}", display, version, table));
    std::println("");
    std::println("Run `mcpp build` to fetch and build with the new dependency.");
    return 0;
}

inline int cmd_remove(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    if (name.empty()) {
        mcpp::ui::error("usage: mcpp remove <pkg>");
        return 2;
    }

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
    auto manifestPath = *root / "mcpp.toml";

    std::ifstream in(manifestPath);
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str();

    // Accept the same forms as `mcpp add`: bare/dotted selector in the single
    // [dependencies] table, or explicit `<ns>:<name>` subtable syntax.
    std::string ns, shortName;
    bool explicitNamespace = false;
    if (auto col = name.find(':'); col != std::string::npos) {
        explicitNamespace = true;
        ns = name.substr(0, col);  shortName = name.substr(col + 1);
    } else {
        ns = std::string{mcpp::manifest::kDefaultNamespace};
        shortName = name;
    }
    const bool isDefaultNs = !explicitNamespace
        || ns == mcpp::manifest::kDefaultNamespace;
    const std::string singleTableKey = explicitNamespace ? shortName : name;

    bool changed = false;
    auto erase_line_at = [&](std::size_t p) {
        auto bol = text.rfind('\n', p);
        auto eol = text.find('\n', p);
        if (bol == std::string::npos) bol = 0; else ++bol;
        if (eol == std::string::npos) eol = text.size();
        text.erase(bol, (eol - bol) + (eol < text.size() ? 1 : 0));
        changed = true;
    };

    // Try bare `<short> = ` and quoted `"<short>" = ` (default-ns flat form).
    if (isDefaultNs) {
        for (const auto& needle : {
            std::format("\n{} = ", singleTableKey),
            std::format("\n\"{}\" = ", singleTableKey),
        }) {
            if (auto p = text.find(needle); p != std::string::npos) {
                erase_line_at(p + 1);
                break;
            }
        }
    }

    auto erase_from_subtable = [&](const std::string& tableNs,
                                   const std::string& tableShort) {
        auto sectHeader = std::format("[dependencies.{}]", tableNs);
        if (auto sp = text.find(sectHeader); sp != std::string::npos) {
            auto bodyStart = text.find('\n', sp);
            if (bodyStart == std::string::npos) bodyStart = text.size();
            auto sectEnd = text.find("\n[", bodyStart);
            if (sectEnd == std::string::npos) sectEnd = text.size();
            std::string section = text.substr(bodyStart, sectEnd - bodyStart);
            for (const auto& needle : {
                std::format("\n{} = ", tableShort),
                std::format("\n\"{}\" = ", tableShort),
            }) {
                if (auto p = section.find(needle); p != std::string::npos) {
                    auto absStart = bodyStart + p + 1;
                    erase_line_at(absStart);
                    break;
                }
            }
            // If the subtable now contains no `name = ...` lines, drop it.
            auto headerPos = text.find(sectHeader);
            if (changed && headerPos != std::string::npos) {
                auto bodyAfter = text.find('\n', headerPos);
                auto endAfter  = text.find("\n[", bodyAfter == std::string::npos ? headerPos : bodyAfter);
                if (endAfter == std::string::npos) endAfter = text.size();
                std::string body = text.substr(bodyAfter == std::string::npos ? headerPos : bodyAfter,
                                                endAfter - (bodyAfter == std::string::npos ? headerPos : bodyAfter));
                bool hasEntry = false;
                std::size_t i = 0;
                while (i < body.size()) {
                    auto j = body.find('\n', i);
                    auto line = body.substr(i, (j == std::string::npos ? body.size() : j) - i);
                    auto first = line.find_first_not_of(" \t");
                    if (first != std::string::npos
                        && line[first] != '#' && line[first] != '\n'
                        && line[first] != '[') {
                        hasEntry = true; break;
                    }
                    if (j == std::string::npos) break;
                    i = j + 1;
                }
                if (!hasEntry) {
                    auto headerLineStart = text.rfind('\n', headerPos);
                    if (headerLineStart == std::string::npos) headerLineStart = 0;
                    text.erase(headerLineStart, endAfter - headerLineStart);
                }
            }
        }
    };

    // Try the namespaced subtable form `[dependencies.<ns>] <short> = `.
    // After deleting the dep line, prune the `[dependencies.<ns>]` header
    // if no entries remain under it.
    if (!isDefaultNs) {
        erase_from_subtable(ns, shortName);
    }

    // Backward-compatible removal: before dotted selectors were preserved in
    // the single table, `mcpp add acme.util` wrote `[dependencies.acme] util`.
    // Keep `mcpp remove acme.util` able to clean that old shape.
    if (!changed && !explicitNamespace) {
        if (auto dot = name.find('.'); dot != std::string::npos) {
            erase_from_subtable(name.substr(0, dot), name.substr(dot + 1));
        }
    }

    // Legacy: `[dependencies.<name>] ...` — pre-namespace inline-spec subtable
    // shape (e.g. when path/git deps were authored as their own subtable). We
    // only honour this for the default-ns input form to avoid colliding with
    // the new `[dependencies.<ns>]` namespacing semantics.
    if (!changed && isDefaultNs) {
        auto block = std::format("[dependencies.{}]", shortName);
        if (auto p = text.find(block); p != std::string::npos) {
            auto bol = text.rfind('\n', p);
            if (bol == std::string::npos) bol = 0; else ++bol;
            auto end = text.find("\n[", p + block.size());
            if (end == std::string::npos) end = text.size();
            else                          end += 1;
            text.erase(bol, end - bol);
            changed = true;
        }
    }

    if (!changed) {
        mcpp::ui::error(std::format("no dependency '{}' in mcpp.toml", name));
        return 1;
    }
    std::ofstream os(manifestPath);
    os << text;
    mcpp::ui::status("Removing", std::format("{} from dependencies", name));
    // Also clean lockfile entry if present
    auto lockPath = *root / "mcpp.lock";
    if (std::filesystem::exists(lockPath)) {
        if (auto lock = mcpp::lockfile::load(lockPath); lock) {
            std::erase_if(lock->packages,
                [&](const auto& p) { return p.name == name; });
            (void)mcpp::lockfile::write(*lock, lockPath);
        }
    }
    return 0;
}

inline int cmd_update(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::optional<std::string> only;
    if (parsed.positional_count() > 0) only = parsed.positional(0);

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }

    // Refresh the index FIRST (#315/D6).
    //
    // This command used to only drop lock entries and tell the user to run
    // `mcpp build` — but the build path never reads mcpp.lock (prepare writes
    // it and nothing on that path loads it), so the whole command was a no-op:
    // it changed no behaviour whatsoever. It is also the only command whose
    // stated purpose is "get me newer dependencies", which since #315 is
    // exactly what an index refresh is for. Explicit intent, so no debounce and
    // no TTL — but still refused when offline, loudly, rather than silently
    // doing nothing again.
    // Skipped when nothing in the project is served by the shared registry:
    // syncing it does nothing for path deps, git deps or a project `[indices]`
    // entry, and paying a multi-repo network round-trip to achieve nothing is
    // the exact behaviour #315 is about.
    if (auto cfg = mcpp::config::load_or_init(
            /*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback())) {
        auto m = mcpp::manifest::load(*root / "mcpp.toml");
        bool registryInvolved = false;
        if (m) {
            auto indices = mcpp::pm::effective_indices(*root);
            mcpp::pm::IndexRoute route{ &indices, *root, &*cfg };
            for (auto& [_, spec] : m->dependencies)
                if (mcpp::pm::routes_to_builtin(route, spec)) { registryInvolved = true; break; }
        }
        if (registryInvolved) {
            auto xlEnv = mcpp::config::make_xlings_env(*cfg);
            if (auto r = mcpp::pm::force_refresh(xlEnv); !r) {
                mcpp::ui::error(r.error());
                return 1;
            }
        }
    } else {
        mcpp::ui::warning(cfg.error().message);
    }

    auto lockPath = *root / "mcpp.lock";
    if (only) {
        // Targeted update — drop just that lock entry; next build will refetch.
        if (std::filesystem::exists(lockPath)) {
            auto lock = mcpp::lockfile::load(lockPath);
            if (lock) {
                std::erase_if(lock->packages,
                    [&](const auto& p) { return p.name == *only; });
                (void)mcpp::lockfile::write(*lock, lockPath);
            }
        }
        mcpp::ui::status("Updating", std::format("{} in mcpp.lock", *only));
    } else {
        // Wholesale update — wipe the lockfile.
        std::error_code ec;
        std::filesystem::remove(lockPath, ec);
        mcpp::ui::status("Updating", "all dependencies (mcpp.lock cleared)");
    }
    std::println("");
    std::println("Run `mcpp build` to re-resolve and rewrite mcpp.lock.");
    return 0;
}

} // namespace mcpp::pm::commands
