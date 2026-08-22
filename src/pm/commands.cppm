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
import mcpp.platform.xlings;               // index freshness
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
        mcpp::ui::error("usage: mcpp add [<ns>.]<pkg>@<version>");
        return 2;
    }

    // Split exactly one optional @<version> tail before doing config or index
    // I/O. Multiple delimiters are never reinterpreted as part of a package
    // identity or version.
    std::string rawNameSpec, version;
    auto at = spec.find('@');
    if (at == std::string::npos) {
        rawNameSpec = spec;
    } else {
        if (spec.find('@', at + 1) != std::string::npos) {
            mcpp::ui::error(std::format(
                "invalid package spec '{}': multiple '@' delimiters", spec));
            return 2;
        }
        rawNameSpec = spec.substr(0, at);
        version = spec.substr(at + 1);
    }

    if (version.empty()) {
        mcpp::ui::error(std::format(
            "package version required: `mcpp add {}@<version>` (M2 supports exact-version only)",
            rawNameSpec));
        return 2;
    }
    for (unsigned char ch : version) {
        if (ch < 0x20 || ch == 0x7f || ch == '"' || ch == '\\') {
            mcpp::ui::error(std::format(
                "invalid package version in '{}': unsafe TOML string character",
                spec));
            return 2;
        }
    }

    // Canonical CLI syntax is `[ns.]name@version`. Keep the historical
    // `ns:name@version` form for one migration release, but normalize it
    // before validation, lookup, diagnostics, or manifest mutation.
    bool legacyColon = false;
    std::string selectorSpelling = rawNameSpec;
    if (auto col = rawNameSpec.find(':'); col != std::string::npos) {
        if (rawNameSpec.find(':', col + 1) != std::string::npos
            || col == 0 || col + 1 == rawNameSpec.size()) {
            mcpp::ui::error(std::format(
                "invalid package spec '{}': expected <namespace>:<name>", spec));
            return 2;
        }
        legacyColon = true;
        selectorSpelling = std::format("{}.{}",
            rawNameSpec.substr(0, col), rawNameSpec.substr(col + 1));
    }

    auto parsedSelector = mcpp::pm::parse_package_selector(selectorSpelling);
    if (!parsedSelector) {
        mcpp::ui::error(parsedSelector.error().message);
        return 2;
    }
    auto coordinate = mcpp::pm::normalize_package_selector(*parsedSelector);
    const bool namespaceOmitted = !parsedSelector->namespace_.has_value();
    // Not const: a namespace-omitted selector that only the deprecated
    // bare-name rungs can serve is MIGRATED here, so what lands in mcpp.toml
    // is the canonical identity rather than the spelling that is going away.
    std::string canonicalSelector =
        mcpp::pm::format_package_selector(coordinate);
    const std::string& ns = coordinate.namespace_;
    const std::string& shortName = coordinate.shortName;

    if (legacyColon) {
        mcpp::ui::warning(std::format(
            "package selector '{}' is deprecated; use `mcpp add {}@{}`",
            rawNameSpec, canonicalSelector, version));
    }

    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
    auto manifestPath = *root / "mcpp.toml";

    // ── Existence gate (#305) ──────────────────────────────────────────
    // Refuse to write a dependency no index can serve, so a typo fails here
    // instead of halfway into the next `mcpp build`. Two rules keep the gate
    // from refusing packages that are perfectly real:
    //
    //   • It probes the SAME exact coordinate the manifest parser will derive
    //     from the key about to be written. A dotted selector is a namespace
    //     path, not an ordered search: `capi.lua` means only `(capi, lua)`.
    //   • It reads through mcpp.pm.index_route, the same routing
    //     `mcpp.build.prepare` resolves dependencies with. A package served by
    //     a project `[indices]` entry therefore counts as present, and a
    //     namespace no readable index can speak for is reported as unverified
    //     rather than rejected: refusing an add is only correct when absence
    //     was actually proven.
    {
        auto selector = mcpp::pm::make_direct_dependency_selector(
            ns, shortName, canonicalSelector);

        auto cfg = mcpp::config::load_or_init(
            /*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
        if (!cfg) {
            mcpp::ui::error(cfg.error().message);
            return 4;
        }

        auto indices = mcpp::pm::effective_indices(*root);
        mcpp::pm::IndexRoute route{ &indices, *root, &*cfg };

        // One release train of diagnostics for the former dotted-candidate
        // rule. The old primary is never appended to the real lookup: its
        // presence only earns a warning with two copyable exact selectors.
        if (auto old = mcpp::pm::legacy_prefixed_coordinate(coordinate)) {
            auto oldSelector = mcpp::pm::make_direct_dependency_selector(
                old->namespace_, old->shortName,
                mcpp::pm::format_package_selector(*old));
            auto oldFound = mcpp::pm::lookup_descriptor(
                route, oldSelector.candidates);
            if (oldFound.hit) {
                mcpp::ui::warning(std::format(
                    "package selector '{}' now resolves exactly to '{}'; an "
                    "older mcpp would select the existing package '{}'. Use "
                    "'{}' to keep the old identity or keep '{}' for the new "
                    "exact identity",
                    selectorSpelling, canonicalSelector,
                    mcpp::pm::format_package_selector(*old),
                    mcpp::pm::format_package_selector(*old),
                    canonicalSelector));
            }
        }

        auto found = mcpp::pm::lookup_descriptor(route, selector.candidates);
        if (!found.error.empty()) {
            mcpp::ui::error(found.error);
            return 2;
        }

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
                mcpp::pm::policy_for(*cfg), xlEnv, canonicalSelector);
            if (auto r = mcpp::pm::apply(d, xlEnv); !r)
                mcpp::ui::warning(r.error());
            if (d.shouldRefresh)
                found = mcpp::pm::lookup_descriptor(route, selector.candidates);
            if (!found.error.empty()) {
                mcpp::ui::error(found.error);
                return 2;
            }
        }

        // One-release bare-name migration, mirroring build-time resolution.
        // `mcpp add` is the cheapest place a user can be moved off the old
        // spelling, because it can perform the edit for them: the warning here
        // is followed by the canonical identity actually being written.
        if (!found.hit && namespaceOmitted) {
            for (auto& legacy : mcpp::pm::legacy_bare_candidates(coordinate)) {
                auto legacySelector = mcpp::pm::make_direct_dependency_selector(
                    legacy.namespace_, legacy.shortName,
                    mcpp::pm::format_package_selector(legacy));
                auto legacyFound = mcpp::pm::lookup_descriptor(
                    route, legacySelector.candidates);
                if (!legacyFound.hit) continue;

                auto resolved = legacyFound.hit->coord;
                if (resolved.namespace_.empty())
                    resolved.namespace_ = legacyFound.hit->declaredNs;
                const auto previous = canonicalSelector;
                coordinate = resolved;
                canonicalSelector =
                    mcpp::pm::format_package_selector(coordinate);
                found = std::move(legacyFound);
                // The namespace-less rung can resolve to the same spelling the
                // user typed (an upstream package that declares no namespace at
                // all). Nothing is being migrated there, so say nothing.
                if (canonicalSelector != previous) {
                    mcpp::ui::warning(std::format(
                        "'{}' matched no package; namespace omission means `{}` "
                        "only. '{}' does exist, so it is being written instead "
                        "— the deprecated bare-name search is removed in {}.",
                        previous, mcpp::pm::kDefaultNamespace,
                        canonicalSelector,
                        mcpp::pm::kBareNameFallbackRemovedIn));
                }
                break;
            }
        }

        if (!found.hit && found.conclusive) {
            std::string hint;
            if (!selector.candidates.empty()) {
                for (auto& suggestion : mcpp::pm::cross_namespace_suggestions(
                         route, selector.candidates.front().shortName)) {
                    hint += "\n    " + suggestion.fqn + suggestion.versions_label();
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
            if (!selector.candidates.empty()) {
                hint += "\n  route: " +
                    route.describe(selector.candidates.front().namespace_);
            }
            mcpp::ui::error(std::format(
                "package '{}' not found in any configured index\n  tried: {}{}",
                canonicalSelector, detail::format_tried(selector.candidates),
                hint));
            return 2;
        }
        if (!found.hit) {
            mcpp::ui::warning(std::format(
                "'{}' could not be verified — no readable index covers that "
                "namespace yet; adding it unchecked", canonicalSelector));
        } else {
            detail::warn_unpublished_version(
                found.hit->lua, canonicalSelector, version);
        }
    }

    std::ifstream in(manifestPath);
    if (!in) {
        mcpp::ui::error(std::format(
            "cannot read '{}'", manifestPath.string()));
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        mcpp::ui::error(std::format(
            "read '{}' failed", manifestPath.string()));
        return 2;
    }

    // `mcpp add` and scaffold injection share one structured, source-preserving
    // editor. Default identities go under [dependencies]; foreign/nested
    // identities use [dependencies.<namespace>] with the short key.
    const bool dev = parsed.is_flag_set("dev");
    const std::string table = dev ? "dev-dependencies" : "dependencies";
    auto edited = mcpp::manifest::upsert_dependency_text(ss.str(), {
        .namespace_ = ns,
        .shortName = shortName,
        .version = version,
        .dev = dev,
    });
    if (!edited) {
        mcpp::ui::error(edited.error());
        return 2;
    }
    {
        std::ofstream os(manifestPath, std::ios::binary | std::ios::trunc);
        if (!os) {
            mcpp::ui::error(std::format(
                "cannot open '{}' for writing", manifestPath.string()));
            return 2;
        }
        os.write(edited->data(), static_cast<std::streamsize>(edited->size()));
        os.flush();
        if (!os) {
            mcpp::ui::error(std::format(
                "write '{}' failed", manifestPath.string()));
            return 2;
        }
        os.close();
        if (!os) {
            mcpp::ui::error(std::format(
                "close '{}' failed", manifestPath.string()));
            return 2;
        }
    }

    mcpp::ui::status("Adding", std::format(
        "{} v{} to {}", canonicalSelector, version, table));
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

    // Accept the same canonical selector as `mcpp add`; retain `ns:name` as a
    // one-release migration alias.
    bool legacyColon = false;
    std::string selectorSpelling = name;
    if (auto col = name.find(':'); col != std::string::npos) {
        if (name.find(':', col + 1) != std::string::npos
            || col == 0 || col + 1 == name.size()) {
            mcpp::ui::error(std::format(
                "invalid package selector '{}': expected <namespace>:<name>",
                name));
            return 2;
        }
        legacyColon = true;
        selectorSpelling = std::format(
            "{}.{}", name.substr(0, col), name.substr(col + 1));
    }
    auto parsedSelector = mcpp::pm::parse_package_selector(selectorSpelling);
    if (!parsedSelector) {
        mcpp::ui::error(parsedSelector.error().message);
        return 2;
    }
    auto coordinate = mcpp::pm::normalize_package_selector(*parsedSelector);
    const auto canonicalSelector =
        mcpp::pm::format_package_selector(coordinate);
    const auto& ns = coordinate.namespace_;
    const auto& shortName = coordinate.shortName;
    const bool isDefaultNs = ns == mcpp::manifest::kDefaultNamespace;
    const std::string singleTableKey = isDefaultNs
        ? shortName : canonicalSelector;
    if (legacyColon) {
        mcpp::ui::warning(std::format(
            "package selector '{}' is deprecated; use `mcpp remove {}`",
            name, canonicalSelector));
    }

    bool changed = false;
    auto erase_line_at = [&](std::size_t p) {
        auto bol = text.rfind('\n', p);
        auto eol = text.find('\n', p);
        if (bol == std::string::npos) bol = 0; else ++bol;
        if (eol == std::string::npos) eol = text.size();
        text.erase(bol, (eol - bol) + (eol < text.size() ? 1 : 0));
        changed = true;
    };

    // Try the flat form first: a bare default key or a retained quoted dotted
    // key from an older manifest. Search only the exact [dependencies] body;
    // a global line match can otherwise delete a same-name dev/build dep that
    // happens to appear earlier in the file.
    if (auto headerPos = text.find("[dependencies]");
        headerPos != std::string::npos) {
        auto bodyStart = text.find('\n', headerPos);
        if (bodyStart == std::string::npos) bodyStart = text.size();
        auto sectionEnd = text.find("\n[", bodyStart);
        if (sectionEnd == std::string::npos) sectionEnd = text.size();
        for (const auto& needle : {
                 std::format("\n{} = ", singleTableKey),
                 std::format("\n\"{}\" = ", singleTableKey),
             }) {
            auto p = text.find(needle, bodyStart);
            if (p == std::string::npos || p >= sectionEnd) continue;
            erase_line_at(p + 1);
            break;
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
        mcpp::ui::error(std::format(
            "no dependency '{}' in mcpp.toml", canonicalSelector));
        return 1;
    }
    std::ofstream os(manifestPath);
    os << text;
    mcpp::ui::status("Removing", std::format(
        "{} from dependencies", canonicalSelector));
    // Also clean lockfile entry if present
    auto lockPath = *root / "mcpp.lock";
    if (std::filesystem::exists(lockPath)) {
        if (auto lock = mcpp::lockfile::load(lockPath); lock) {
            std::erase_if(lock->packages,
                [&](const auto& p) { return p.name == canonicalSelector; });
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
    // Until #329 this command changed nothing at all: it dropped lock
    // entries and told the user to run `mcpp build`, but the build path
    // never read mcpp.lock. #329 made the lock authoritative for git
    // branch deps — `mcpp build` now deliberately rebuilds the recorded
    // commit — so dropping an entry here is the one thing that lets a
    // branch advance. That is half of what this command does; the index
    // refresh below is the other half, covering registry-served version
    // deps. Explicit intent, so no debounce and no TTL — but still
    // refused when offline, loudly, rather than silently doing nothing
    // again.
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
