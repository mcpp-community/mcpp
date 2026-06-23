// mcpp.pm.index_management — registry/index operations: search, list, add,
// remove, update, pin, unpin.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.pm.index_management;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.lockfile;
import mcpp.manifest;
import mcpp.project;
import mcpp.ui;
import mcpp.xlings;

namespace mcpp::pm {

// `mcpp search <keyword>`.
export int search_packages(const std::string& keyword) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    auto xlEnv = mcpp::config::make_xlings_env(*cfg);
    if (!mcpp::xlings::is_index_fresh(xlEnv, cfg->searchTtlSeconds)) {
        mcpp::ui::status("Updating", "package index (auto-refresh)");
        mcpp::xlings::ensure_index_fresh(xlEnv, cfg->searchTtlSeconds, /*quiet=*/true);
    }

    mcpp::fetcher::Fetcher f(*cfg);
    auto hits = f.search(keyword);
    if (!hits) { mcpp::ui::error(hits.error().message); return 1; }

    if (hits->empty()) {
        std::println("");
        std::println("No packages match `{}`.", keyword);
        return 0;
    }
    std::println("");
    for (auto& h : *hits) {
        std::println("  {:<20}  {}", h.name, h.description);
    }
    return 0;
}

// `mcpp index list` — global registries + project [indices].
export int index_list() {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::fetcher::Fetcher f(*cfg);

    auto repos = f.list_repos();
    if (!repos) { mcpp::ui::error(repos.error().message); return 1; }
    if (repos->empty()) {
        for (auto& r : cfg->indexRepos) {
            bool isDefault = (r.name == cfg->defaultIndex);
            std::println("  {:<15}  {}{}",
                         r.name, r.url, isDefault ? "  (default)" : "");
        }
    } else {
        for (auto& r : *repos) {
            std::println("  {:<15}  {}", r.name, r.url);
        }
    }

    // Show project-level custom indices from mcpp.toml [indices].
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (root) {
        auto m = mcpp::manifest::load(*root / "mcpp.toml");
        if (m && !m->indices.empty()) {
            std::println("");
            std::println("Project indices (mcpp.toml):");
            for (auto& [name, spec] : m->indices) {
                if (spec.is_local()) {
                    std::println("  {:<15}  {}  (local path)", name, spec.path.string());
                } else {
                    std::string suffix;
                    if (spec.is_builtin()) suffix = "  (pin)";
                    else if (!spec.tag.empty()) suffix = std::format("  (tag: {})", spec.tag);
                    else if (!spec.rev.empty()) suffix = std::format("  (rev: {})", spec.rev.substr(0, 8));
                    std::println("  {:<15}  {}{}", name, spec.url, suffix);
                }
            }
        }
    }
    return 0;
}

// `mcpp index add <name> <url>`.
export int index_add(const std::string& name, const std::string& url) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::fetcher::Fetcher f(*cfg);
    auto r = f.add_repo(name, url);
    if (!r) { mcpp::ui::error(r.error().message); return 1; }
    mcpp::ui::status("Added", std::format("registry `{}` -> {}", name, url));
    return 0;
}

// `mcpp index remove <name>`.
export int index_remove(const std::string& name) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::fetcher::Fetcher f(*cfg);
    auto r = f.remove_repo(name);
    if (!r) { mcpp::ui::error(r.error().message); return 1; }
    mcpp::ui::status("Removed", std::format("registry `{}`", name));
    return 0;
}

// `mcpp index update [name]` — empty filterName updates everything.
export int index_update(const std::string& filterName) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    // Update global index repos.
    mcpp::ui::status("Updating", "all index repos");
    auto xlEnv = mcpp::config::make_xlings_env(*cfg);
    int rc = mcpp::xlings::update_index(xlEnv);
    if (rc != 0) { mcpp::ui::error("index update failed"); return 1; }

    // Also update project-level custom indices if present.
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (root) {
        auto m = mcpp::manifest::load(*root / "mcpp.toml");
        if (m && !m->indices.empty()) {
            // filterName: optional single-index filter (parameter).
            for (auto& [idxName, spec] : m->indices) {
                if (!filterName.empty() && idxName != filterName) continue;
                if (spec.is_local()) {
                    mcpp::ui::status("Skipped", std::format("index `{}` is a local path", idxName));
                    continue;
                }
                if (spec.is_builtin()) continue;
                // Re-sync the project-level clone via xlings.
                mcpp::config::ensure_project_index_dir(*cfg, *root, m->indices);
                auto projEnv = mcpp::config::make_project_xlings_env(*cfg, *root);
                int prc = mcpp::xlings::update_index(projEnv);
                if (prc != 0) {
                    mcpp::ui::error(std::format("project index `{}` update failed", idxName));
                } else {
                    mcpp::ui::status("Updated", std::format("project index `{}`", idxName));
                }
                break;  // ensure_project_index_dir handles all custom indices at once
            }
        }
    }

    mcpp::ui::status("Updated", "index refresh complete");
    return 0;
}

// `mcpp index status` — read-only, offline snapshot of the local indexes.
// Never touches the network: reports presence/freshness/age and, when an
// index is missing or stale, points at the explicit `mcpp index update`.
export int index_status() {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    auto xlEnv = mcpp::config::make_xlings_env(*cfg);

    auto fmt_age = [](std::int64_t s) -> std::string {
        if (s < 0)      return "unknown";
        if (s < 90)     return std::format("{}s ago", s);
        if (s < 5400)   return std::format("{}m ago", s / 60);
        if (s < 172800) return std::format("{}h ago", s / 3600);
        return std::format("{}d ago", s / 86400);
    };
    auto show = [&](const char* label, const mcpp::xlings::IndexStatus& st) {
        std::string state = !st.present ? "missing"
                          :  st.fresh   ? "fresh"
                          :              "stale";
        std::println("  {:<10} {:<8} {:<12} {}",
                     label, state, fmt_age(st.ageSeconds), st.dir.string());
    };

    auto official = mcpp::xlings::official_index_status(xlEnv, cfg->searchTtlSeconds);
    auto deflt    = mcpp::xlings::default_index_status(xlEnv, cfg->searchTtlSeconds);

    std::println("");
    std::println("  {:<10} {:<8} {:<12} {}", "index", "state", "refreshed", "path");
    show("xim", official);
    show("mcpplibs", deflt);
    std::println("");

    bool anyMissing = !official.present || !deflt.present;
    if (anyMissing) {
        mcpp::ui::status("Hint", "an index is missing — run `mcpp index update` to fetch it");
    } else {
        std::println("  Up to date locally. Refresh on demand with `mcpp index update`.");
    }
    return 0;
}

// `mcpp index pin <name> [<rev>]` — empty rev falls back to mcpp.lock.
export int index_pin(const std::string& name, std::string rev) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }

    // If no rev supplied, try to get it from the lockfile.
    if (rev.empty()) {
        auto lockPath = *root / "mcpp.lock";
        auto lockRes = mcpp::lockfile::load(lockPath);
        if (lockRes) {
            for (auto& idx : lockRes->indices) {
                if (idx.name == name) { rev = idx.rev; break; }
            }
        }
    }
    if (rev.empty()) {
        mcpp::ui::error(std::format(
            "no revision found for index `{}`. Run `mcpp index update` first, or supply a rev.",
            name));
        return 1;
    }

    // Read mcpp.toml as text and insert/update rev field in [indices.<name>].
    auto tomlPath = *root / "mcpp.toml";
    std::ifstream is(tomlPath);
    if (!is) { mcpp::ui::error(std::format("cannot read '{}'", tomlPath.string())); return 1; }
    std::stringstream ss; ss << is.rdbuf();
    std::string text = ss.str();
    is.close();

    // Strategy: find the [indices] section, then find/create the key.
    // For short form `name = "url"`, replace with long form.
    // For long form `[indices.<name>]` or inline `name = { ... }`, insert/update rev.
    // Use a simple approach: find `[indices]` section, then search for the key name.
    auto indicesPos = text.find("[indices]");
    if (indicesPos == std::string::npos) {
        mcpp::ui::error(std::format("no [indices] section in mcpp.toml for `{}`", name));
        return 1;
    }

    auto bodyStart = text.find('\n', indicesPos);
    if (bodyStart == std::string::npos) bodyStart = text.size();
    else bodyStart += 1;
    auto nextSec = text.find("\n[", bodyStart);
    // Avoid matching [indices.*] sub-tables — look for a line starting with [
    // that is NOT [indices.
    auto bodyEnd = std::string::npos;
    {
        auto pos = bodyStart;
        while (pos < text.size()) {
            auto nl = text.find("\n[", pos);
            if (nl == std::string::npos) break;
            auto secName = text.substr(nl + 2, 20);
            if (!secName.starts_with("indices.") && !secName.starts_with("indices]")) {
                bodyEnd = nl;
                break;
            }
            pos = nl + 2;
        }
    }
    if (bodyEnd == std::string::npos) bodyEnd = text.size();
    auto body = std::string_view(text).substr(bodyStart, bodyEnd - bodyStart);

    // Find the key line: `name = ...` within the indices body.
    auto keyPos = body.find(name);
    if (keyPos == std::string::npos) {
        mcpp::ui::error(std::format("index `{}` not found in [indices] section", name));
        return 1;
    }
    auto absKeyPos = bodyStart + keyPos;

    // Find the line containing this key.
    auto lineEnd = text.find('\n', absKeyPos);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    auto lineContent = text.substr(absKeyPos, lineEnd - absKeyPos);

    // Check if it's inline table form: `name = { ... }`
    auto braceOpen = lineContent.find('{');
    if (braceOpen != std::string::npos) {
        // Inline table. Find the closing brace and insert/update rev.
        auto braceClose = lineContent.find('}', braceOpen);
        if (braceClose == std::string::npos) {
            mcpp::ui::error("malformed inline table in mcpp.toml");
            return 1;
        }
        auto tableContent = lineContent.substr(braceOpen + 1, braceClose - braceOpen - 1);
        auto revPos = tableContent.find("rev");
        if (revPos != std::string::npos) {
            // Replace existing rev value. Find the value start and end.
            auto eqPos = tableContent.find('=', revPos);
            auto valStart = tableContent.find('"', eqPos);
            auto valEnd = tableContent.find('"', valStart + 1);
            if (valStart != std::string::npos && valEnd != std::string::npos) {
                // Replace in the original text.
                auto absValStart = absKeyPos + braceOpen + 1 + valStart + 1;
                auto absValEnd = absKeyPos + braceOpen + 1 + valEnd;
                text.replace(absValStart, absValEnd - absValStart, rev);
            }
        } else {
            // Insert rev field before closing brace.
            auto absClose = absKeyPos + braceClose;
            std::string insert = std::format(", rev = \"{}\"", rev);
            text.insert(absClose, insert);
        }
    } else if (lineContent.find('"') != std::string::npos) {
        // Short form: `name = "url"` — convert to long form with rev.
        auto qStart = lineContent.find('"');
        auto qEnd = lineContent.find('"', qStart + 1);
        if (qStart != std::string::npos && qEnd != std::string::npos) {
            auto url = lineContent.substr(qStart + 1, qEnd - qStart - 1);
            std::string replacement = std::format("{} = {{ url = \"{}\", rev = \"{}\" }}",
                                                  name, url, rev);
            text.replace(absKeyPos, lineEnd - absKeyPos, replacement);
        }
    } else {
        mcpp::ui::error(std::format("cannot parse index `{}` entry in mcpp.toml", name));
        return 1;
    }

    std::ofstream os(tomlPath);
    if (!os) { mcpp::ui::error(std::format("cannot write '{}'", tomlPath.string())); return 1; }
    os << text;

    mcpp::ui::status("Pinned", std::format("index `{}` to rev {}", name, rev.substr(0, 12)));
    return 0;
}

// `mcpp index unpin <name>`.
export int index_unpin(const std::string& name) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }

    auto tomlPath = *root / "mcpp.toml";
    std::ifstream is(tomlPath);
    if (!is) { mcpp::ui::error(std::format("cannot read '{}'", tomlPath.string())); return 1; }
    std::stringstream ss; ss << is.rdbuf();
    std::string text = ss.str();
    is.close();

    auto indicesPos = text.find("[indices]");
    if (indicesPos == std::string::npos) {
        mcpp::ui::error(std::format("no [indices] section in mcpp.toml for `{}`", name));
        return 1;
    }

    auto bodyStart = text.find('\n', indicesPos);
    if (bodyStart == std::string::npos) bodyStart = text.size();
    else bodyStart += 1;
    auto bodyEnd = std::string::npos;
    {
        auto pos = bodyStart;
        while (pos < text.size()) {
            auto nl = text.find("\n[", pos);
            if (nl == std::string::npos) break;
            auto secName = text.substr(nl + 2, 20);
            if (!secName.starts_with("indices.") && !secName.starts_with("indices]")) {
                bodyEnd = nl;
                break;
            }
            pos = nl + 2;
        }
    }
    if (bodyEnd == std::string::npos) bodyEnd = text.size();
    auto body = std::string_view(text).substr(bodyStart, bodyEnd - bodyStart);

    auto keyPos = body.find(name);
    if (keyPos == std::string::npos) {
        mcpp::ui::error(std::format("index `{}` not found in [indices] section", name));
        return 1;
    }
    auto absKeyPos = bodyStart + keyPos;

    auto lineEnd = text.find('\n', absKeyPos);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    auto lineContent = text.substr(absKeyPos, lineEnd - absKeyPos);

    auto braceOpen = lineContent.find('{');
    if (braceOpen != std::string::npos) {
        auto braceClose = lineContent.find('}', braceOpen);
        if (braceClose == std::string::npos) {
            mcpp::ui::error("malformed inline table in mcpp.toml");
            return 1;
        }
        auto tableContent = lineContent.substr(braceOpen + 1, braceClose - braceOpen - 1);
        auto revPos = tableContent.find("rev");
        if (revPos == std::string::npos) {
            mcpp::ui::info("Info", std::format("index `{}` has no rev to unpin", name));
            return 0;
        }
        // Remove `, rev = "..."` or `rev = "...", ` from the inline table.
        // Find the full `rev = "..."` span (including surrounding comma + spaces).
        auto absTableStart = absKeyPos + braceOpen + 1;
        auto absRevPos = absTableStart + revPos;

        // Find the extent: key = "value"
        auto eqPos = text.find('=', absRevPos);
        auto valStart = text.find('"', eqPos);
        auto valEnd = text.find('"', valStart + 1);
        if (valStart == std::string::npos || valEnd == std::string::npos) {
            mcpp::ui::error("cannot parse rev field in mcpp.toml");
            return 1;
        }
        auto fieldEnd = valEnd + 1;

        // Determine removal range including comma/spaces.
        auto removeStart = absRevPos;
        auto removeEnd = fieldEnd;
        // Check for leading ", " (comma before rev).
        if (removeStart >= 2 && text.substr(removeStart - 2, 2) == ", ") {
            removeStart -= 2;
        } else if (removeEnd < text.size() && text[removeEnd] == ',') {
            removeEnd += 1;
            if (removeEnd < text.size() && text[removeEnd] == ' ') removeEnd += 1;
        }
        // Also eat leading whitespace before "rev".
        while (removeStart > absTableStart && text[removeStart - 1] == ' ') removeStart--;

        text.erase(removeStart, removeEnd - removeStart);
    } else {
        mcpp::ui::info("Info", std::format(
            "index `{}` is in short form (no rev to unpin)", name));
        return 0;
    }

    std::ofstream os(tomlPath);
    if (!os) { mcpp::ui::error(std::format("cannot write '{}'", tomlPath.string())); return 1; }
    os << text;

    mcpp::ui::status("Unpinned", std::format("index `{}` (rev removed)", name));
    return 0;
}

} // namespace mcpp::pm
