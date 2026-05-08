// mcpp.pm.commands — package-management CLI commands.
//
// Hosts `cmd_add` / `cmd_remove` / `cmd_update` — the user-facing
// commands that mutate `mcpp.toml` and `mcpp.lock`. Previously sat
// in `cli.cppm`'s detail namespace; PR-R5 of the pm subsystem refactor
// (`.agents/docs/2026-05-08-pm-subsystem-architecture.md`) moves them
// into the pm subsystem so `cli.cppm` is responsible only for the
// global CLI framework + non-pm commands.
//
// Strict zero-behavior-change move: every line below is identical to
// what previously lived in `cli.cppm`, only the surrounding namespace
// has changed.

export module mcpp.pm.commands;

import std;
import mcpp.manifest;            // kDefaultNamespace alias
import mcpp.lockfile;             // load / write (still via shim)
import mcpp.ui;
import mcpplibs.cmdline;

namespace mcpp::pm::commands::detail {

// Locate mcpp.toml by walking upward from cwd. Local copy of the
// `cli.cppm` helper of the same name so this module doesn't have to
// import `mcpp.cli` (which would create a circular dep). A future
// refactor can fold the two into a shared `mcpp.project` module.
inline std::optional<std::filesystem::path>
find_manifest_root(std::filesystem::path start) {
    auto p = std::filesystem::absolute(start);
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) return p;
        auto parent = p.parent_path();
        if (parent == p) return std::nullopt;
        p = parent;
    }
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

    // Split <ns>:<name>. xpkg-style namespace separator. Bare `name` keeps
    // the default namespace (mcpp); legacy `ns.name` is also accepted on
    // input for ergonomics, but written out in the new subtable form.
    std::string ns, shortName;
    if (auto col = nameSpec.find(':'); col != std::string::npos) {
        ns        = nameSpec.substr(0, col);
        shortName = nameSpec.substr(col + 1);
    } else if (auto dot = nameSpec.find('.'); dot != std::string::npos) {
        ns        = nameSpec.substr(0, dot);
        shortName = nameSpec.substr(dot + 1);
    } else {
        ns        = std::string{mcpp::manifest::kDefaultNamespace};
        shortName = nameSpec;
    }
    if (shortName.empty()) {
        mcpp::ui::error(std::format("invalid spec '{}': empty package name", spec));
        return 2;
    }

    auto root = detail::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
    auto manifestPath = *root / "mcpp.toml";

    if (version.empty()) {
        mcpp::ui::error(std::format(
            "package version required: `mcpp add {}@<version>` (M2 supports exact-version only)",
            spec));
        return 2;
    }

    std::ifstream in(manifestPath);
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str();

    // Insertion strategy:
    //   - Default namespace → `[dependencies] ... name = "version"` (no quotes).
    //   - Other namespace   → `[dependencies.<ns>] ... name = "version"`,
    //                         creating the subtable if absent.
    const bool isDefaultNs = (ns == mcpp::manifest::kDefaultNamespace);
    const std::string section = isDefaultNs
        ? "[dependencies]"
        : std::format("[dependencies.{}]", ns);
    auto pos = text.find(section);
    if (pos == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += "\n";
        text += std::format("\n{}\n{} = \"{}\"\n", section, shortName, version);
    } else {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        text.insert(nl, std::format("\n{} = \"{}\"", shortName, version));
    }
    {
        std::ofstream os(manifestPath);
        os << text;
    }

    std::string display = isDefaultNs ? shortName : std::format("{}:{}", ns, shortName);
    mcpp::ui::status("Adding", std::format("{} v{} to dependencies", display, version));
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

    auto root = detail::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
    auto manifestPath = *root / "mcpp.toml";

    std::ifstream in(manifestPath);
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str();

    // Accept the same forms as `mcpp add`: bare `name` (default ns),
    // `<ns>:<name>`, or legacy `<ns>.<name>`. The line we want to delete
    // depends on which form the user wrote in mcpp.toml — try every one.
    std::string ns, shortName;
    if (auto col = name.find(':'); col != std::string::npos) {
        ns = name.substr(0, col);  shortName = name.substr(col + 1);
    } else if (auto dot = name.find('.'); dot != std::string::npos) {
        ns = name.substr(0, dot);  shortName = name.substr(dot + 1);
    } else {
        ns = std::string{mcpp::manifest::kDefaultNamespace};
        shortName = name;
    }
    const bool isDefaultNs = (ns == mcpp::manifest::kDefaultNamespace);

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
            std::format("\n{} = ", shortName),
            std::format("\n\"{}\" = ", shortName),
        }) {
            if (auto p = text.find(needle); p != std::string::npos) {
                erase_line_at(p + 1);
                break;
            }
        }
    }

    // Try the namespaced subtable form `[dependencies.<ns>] <short> = `.
    // After deleting the dep line, prune the `[dependencies.<ns>]` header
    // if no entries remain under it.
    if (!isDefaultNs) {
        auto sectHeader = std::format("[dependencies.{}]", ns);
        if (auto sp = text.find(sectHeader); sp != std::string::npos) {
            auto bodyStart = text.find('\n', sp);
            if (bodyStart == std::string::npos) bodyStart = text.size();
            auto sectEnd = text.find("\n[", bodyStart);
            if (sectEnd == std::string::npos) sectEnd = text.size();
            std::string section = text.substr(bodyStart, sectEnd - bodyStart);
            for (const auto& needle : {
                std::format("\n{} = ", shortName),
                std::format("\n\"{}\" = ", shortName),
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

    auto root = detail::find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }
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
