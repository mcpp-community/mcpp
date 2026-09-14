// mcpp.pm.publisher — generate xpkg Lua entry from mcpp.toml + scanner.
//
// See docs/11-publishing-a-library.md for the produced layout.

module;

export module mcpp.pm.publisher;

import std;
import mcpp.pack.host_requirements;  // J: one derivation, two projections
import mcpp.manifest;
import mcpp.diag;
import mcpp.modgraph.graph;
import mcpp.platform;
// `cfgpred::os_only_platforms` (#630 item 7) — the standalone half of
// `mcpp.build.prepare`, carrying no dependency on the rest of it (see that
// module's header comment), which is what makes it importable from the
// publish side without pulling in the build plan.
import mcpp.build.prepare_inputs;

export namespace mcpp::pm {

struct ReleaseInfo {
    std::string version;     // tag/version, e.g. "0.1.0"

    struct PerPlatform {
        std::string url;
        std::string sha256;
    };
    PerPlatform linux;
    PerPlatform macosx;
    PerPlatform windows;
};

// Generate the xpkg Lua content for a package.
std::string emit_xpkg(const mcpp::manifest::Manifest&  manifest,
                      const mcpp::modgraph::Graph&      graph,
                      const ReleaseInfo&                release);

// Convenience: synthesize a placeholder ReleaseInfo for `mcpp emit xpkg --version V`
// before publish infrastructure exists. Uses {url, sha256} sentinels.
ReleaseInfo placeholder_release(std::string_view version);

// Compute the convention-based GitHub Release tarball URL for a package:
//   "<repo>/releases/download/v<version>/<name>-<version>.tar.gz"
// Returns empty string if `repo` is empty or doesn't look like a https URL.
std::string release_tarball_url(std::string_view repo,
                                std::string_view name,
                                std::string_view version);

// Compute SHA-256 of `file` by shelling out to `sha256sum` (universally
// available on Linux). Returns empty string on failure.
std::string sha256_of_file(const std::filesystem::path& file);

// Pack the package source tree at `root` into a tarball at `output` using
// `git archive` (so .gitignore'd files are excluded automatically). The
// tarball uses prefix "<name>-<version>/" so unpacking yields a clean
// versioned directory.
//
// Requires the project to be in a git repo.
//
// Returns a non-empty error message on failure (empty on success).
std::string make_release_tarball(const std::filesystem::path& root,
                                 std::string_view name,
                                 std::string_view version,
                                 const std::filesystem::path& output);

// Convenience: build a real ReleaseInfo for v0.0.3-style local publish
// where all three platforms point at the same source tarball. Caller has
// already produced the tarball + sha256 by other means.
ReleaseInfo make_release_info(std::string_view version,
                              std::string_view url,
                              std::string_view sha256);

} // namespace mcpp::pm

namespace mcpp::pm {

namespace {

// Quote `s` as a Lua double-quoted string literal: `"..."`.
//
// We deliberately use `"..."` (not the long-bracket `[[...]]` form)
// so the only meta-characters that need escaping are the standard set
// for `"` strings:
//   - `"`  and `\\`  must be backslash-escaped
//   - newline / carriage-return / NUL break the string literal
//   - other control bytes are escaped numerically (`\xHH`) for safety
//     so the emitted .lua is purely printable ASCII even when the input
//     contains exotic bytes
//
// Long-bracket sequences like `]=]` are NOT a vector here because we
// never emit `[[`/`]=]` ourselves — the output is always `"..."`.
std::string lua_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case 0:    out += "\\0";  break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    // Other C0 controls + DEL — emit as \xHH to keep the
                    // .lua text purely printable.
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string platform_block(std::string_view version, const ReleaseInfo::PerPlatform& pp) {
    return std::format(
        "        ['{0}'] = {{ url = {1}, sha256 = {2} }},\n",
        version, lua_escape(pp.url), lua_escape(pp.sha256));
}

// The install-time edge of a published package, per platform.
//
// A consumer that runs `mcpp add <pkg>` gets whatever this names installed
// alongside it. Nothing emitted it before, so a package declaring an
// environment had its edge written into the descriptor by hand — which is how
// `riscv-virt-rt` 0.3.0 shipped without the C library its own target row named,
// and why that file now carries thirty lines explaining the removal.
//
// The declaration is read UNRESOLVED (`workspaceByPlatform`), because a
// descriptor has a block per platform and the host resolution has already
// discarded two of the three. Emitting from the host-resolved list instead
// would produce a descriptor whose edges depend on which machine packed it,
// and nothing downstream could tell.
//
// A package still on the superseded `[xlings] deps` emits NO edge, exactly
// as before. That is not an oversight: `deps` is resolved for the host at
// load, so the per-platform declaration a descriptor needs is already gone by
// the time this runs, and writing the host's answer into all three blocks
// would be the machine-dependent descriptor this comment rejects. The
// advisory that key raises tells its author how to obtain an edge.
std::string platform_deps_block(
    const std::map<std::string, std::vector<std::string>>& byPlatform,
    std::string_view platform)
{
    auto it = byPlatform.find(std::string(platform));
    if (it == byPlatform.end() || it->second.empty()) return {};
    std::string out = "            deps = {";
    bool first = true;
    for (auto const& address : it->second) {
        out += std::format("{} {}", first ? "" : ",", lua_escape(address));
        first = false;
    }
    out += " },\n";
    return out;
}

} // namespace

std::string emit_xpkg(const mcpp::manifest::Manifest&  manifest,
                      const mcpp::modgraph::Graph&      graph,
                      const ReleaseInfo&                release)
{
    std::string out;
    out += "-- AUTO-GENERATED by `mcpp emit xpkg`. Do not edit by hand.\n";
    out += std::format("-- Source: mcpp.toml @ v{}\n", release.version);
    out += "package = {\n";
    out += "    spec = \"1\",\n";
    // #278 — emit BOTH `namespace` and the fully-qualified `name` (INV-NAME).
    //
    // This used to write only the bare project name and no `namespace` at all.
    // A namespaced index requires the namespace, so maintainers hand-added a
    // `namespace = "<org>"` line when filing the package — and that edit turned
    // the descriptor into the split form, which parses fine but can never be
    // installed. `aimol.tensorvia-cpu` was born exactly this way (the file still
    // carries the "AUTO-GENERATED … do not edit by hand" banner). Emitting the
    // FQN here closes the generator half of that loop; `mcpp xpkg parse` closes
    // the lint half. Design: 2026-06-26 §4.5 prescribed this and it never landed.
    if (!manifest.package.namespace_.empty()) {
        auto prefix = manifest.package.namespace_ + ".";
        auto fqn = manifest.package.name.starts_with(prefix)
            ? manifest.package.name
            : prefix + manifest.package.name;
        out += std::format("    namespace = {},\n",
                           lua_escape(manifest.package.namespace_));
        out += std::format("    name = {},\n", lua_escape(fqn));
    } else {
        out += std::format("    name = {},\n", lua_escape(manifest.package.name));
    }
    if (!manifest.package.description.empty())
        out += std::format("    description = {},\n", lua_escape(manifest.package.description));
    if (!manifest.package.license.empty())
        out += std::format("    licenses = {{{}}},\n", lua_escape(manifest.package.license));
    if (!manifest.package.repo.empty())
        out += std::format("    repo = {},\n", lua_escape(manifest.package.repo));
    out += "    type = \"package\",\n\n";

    out += "    xpm = {\n";
    // A TARGET-AXIS TOOL DECLARATION PRODUCES NO EDGE IN GENERAL, AND IS SAID
    // SO HERE -- EXCEPT WHEN THE SELECTOR NAMES ONLY AN OPERATING SYSTEM.
    //
    // `workspaceByPlatform` is filled by the TOP-LEVEL `[xlings.workspace]`,
    // because a descriptor has one block per platform and a platform-keyed
    // value is exactly that shape. `[target.<selector>.xlings.workspace]` is a
    // different question in general -- it conditions on the resolved TARGET,
    // and most selectors are not a platform: `cfg(target_arch = "aarch64")`
    // names no block this file has, and neither does `cfg(not(windows))` (see
    // `os_only_platforms` for why the latter is excluded even though it
    // mentions only an OS).
    //
    // But `cfg(linux)`, `cfg(os = "linux")` and their windows/macos/unix
    // counterparts DO name a block: they ask the exact question a descriptor
    // block answers, on the same axis `[xlings.workspace]` already resolves
    // by platform (design record 2026-09-13-630 §8.2). A copy, not the
    // manifest's own map, because the merge below is specific to rendering
    // this descriptor and must not mutate what the rest of `emit_xpkg` (or a
    // second call) reads.
    auto byPlatform = manifest.xlings.workspaceByPlatform;
    auto merge_into = [&](std::string_view platform, const std::string& address) {
        auto& list = byPlatform[std::string(platform)];
        // Do not duplicate an address already present in the block -- an
        // `[xlings.workspace]` entry and an OS-only conditional one can name
        // the same package without the descriptor listing it twice.
        if (std::ranges::find(list, address) == list.end())
            list.push_back(address);
    };
    for (auto const& cc : manifest.conditionalConfigs) {
        if (cc.xlings.deps.empty() && cc.xlings.featureDeps.empty()) continue;
        auto platforms = mcpp::build::cfgpred::os_only_platforms(cc.predicate);
        if (!platforms.empty()) {
            // The install-time edge a consumer's `xlings install` needs is
            // exactly the same for a package declared here as for one
            // declared in the top-level `[xlings.workspace]` restricted to
            // this platform -- so it is merged into the same containers,
            // under the same key shape (platform → addresses) that table
            // fills. Feature-gated tools have no feature axis in this
            // descriptor at all yet (the unconditional block above does not
            // either), so they fold in unconditionally, same as `deps`.
            for (auto const& platform : platforms) {
                for (auto const& a : cc.xlings.deps) merge_into(platform, a);
                for (auto const& [f, addrs] : cc.xlings.featureDeps)
                    for (auto const& a : addrs) merge_into(platform, a);
            }
            continue;
        }
        std::string named;
        auto add = [&](const std::string& a) {
            if (!named.empty()) named += ", ";
            named += a;
        };
        for (auto const& a : cc.xlings.deps) add(a);
        for (auto const& [f, addrs] : cc.xlings.featureDeps)
            for (auto const& a : addrs) add(a);
        mcpp::diag::warning("publish/target-axis-tools", std::format(
            "[target.'{}'] declares tools ({}) and the descriptor carries no "
            "edge for them: its blocks are per platform, and a selector is not "
            "a platform (an OS-only selector, such as cfg(linux) or "
            "cfg(os = \"windows\"), IS emitted as one of the three blocks; "
            "this one is not). A consumer of this package installs what the "
            "three `xpm.<platform>.deps` blocks name, which come from the "
            "top-level [xlings.workspace]. Declare there anything a CONSUMER "
            "must have installed; the target axis stays correct for what "
            "this package's own build compiles against.", cc.predicate, named));
    }
    out += "        linux   = {\n" + platform_deps_block(byPlatform, "linux")
         + platform_block(release.version, release.linux)   + "        },\n";
    out += "        macosx  = {\n" + platform_deps_block(byPlatform, "macosx")
         + platform_block(release.version, release.macosx)  + "        },\n";
    out += "        windows = {\n" + platform_deps_block(byPlatform, "windows")
         + platform_block(release.version, release.windows) + "        },\n";
    out += "    },\n\n";

    out += "    mcpp = {\n";
    out += "        schema = \"0.1\",\n";
    out += std::format("        language = {},\n", lua_escape(manifest.language.standard));
    out += std::format("        import_std = {},\n", manifest.language.importStd ? "true" : "false");

    // Module list (from scanner)
    out += "        modules = {\n";
    for (auto& u : graph.units) {
        if (!u.provides) continue;
        // Skip partition-only units: their logical name contains ':'
        if (u.provides->logicalName.find(':') != std::string::npos) continue;
        out += std::format("            {},\n", lua_escape(u.provides->logicalName));
    }
    out += "        },\n";

    // Dependencies (excluding dev-dependencies). Path-based deps are
    // local-only and intentionally not exposed in the published xpkg
    // descriptor; only version-based deps are emitted.
    out += "        deps = {\n";
    for (auto& [k, v] : manifest.dependencies) {
        if (v.isPath() || v.version.empty()) continue;
        out += std::format("            [{}] = {},\n", lua_escape(k), lua_escape(v.version));
    }
    out += "        },\n";

    // What the TARGET machine must provide.
    //
    // THE SAME DERIVATION `mcpp pack` USES. A tarball can only DESCRIBE these
    // (its HOST-REQUIREMENTS file); a descriptor can have them RESOLVED, by
    // the xlings on the machine that installs the package. Two projections of
    // one fact — so they come from one function. Deriving them separately is
    // how they drift, and a drifted list is undetectable: each side looks
    // reasonable on its own.
    if (auto hostReqs = mcpp::pack::host_requirements_of(manifest.runtimeConfig);
        !hostReqs.empty()) {
        out += "        runtime = {\n";
        out += "            requirements = {\n";
        for (auto const& req : hostReqs) {
            out += std::format(
                "                {{ kind = \"capability\", value = {}, "
                "phase = \"run\", required = {}, discovery = {} }},\n",
                lua_escape(req.capability),
                req.required ? "true" : "false",
                lua_escape(req.discovery));
        }
        out += "            },\n";
        out += "        },\n";
    }

    out += "        manifest = \"mcpp.toml\",\n";
    out += "    },\n";
    out += "}\n";
    return out;
}

ReleaseInfo placeholder_release(std::string_view version) {
    ReleaseInfo r;
    r.version = std::string(version);
    auto fill = [&](ReleaseInfo::PerPlatform& pp, std::string_view ext) {
        pp.url    = std::format("<TBD: release tarball URL>.{}", ext);
        pp.sha256 = "<TBD: sha256>";
    };
    fill(r.linux,   "tar.gz");
    fill(r.macosx,  "tar.gz");
    fill(r.windows, "zip");
    return r;
}

std::string release_tarball_url(std::string_view repo,
                                std::string_view name,
                                std::string_view version)
{
    // Strip trailing ".git" if present.
    std::string r{repo};
    if (r.ends_with(".git")) r.resize(r.size() - 4);
    if (r.empty()) return {};
    if (!r.starts_with("https://") && !r.starts_with("http://")) return {};
    return std::format("{}/releases/download/v{}/{}-{}.tar.gz",
                       r, version, name, version);
}

std::string sha256_of_file(const std::filesystem::path& file) {
    if (!std::filesystem::exists(file)) return {};
    // An argument vector rather than a `2>/dev/null` command string, which
    // cmd.exe cannot open on a Windows host.
    auto r = mcpp::platform::process::capture_host_tool_stdout(
        {"sha256sum", file.string()});
    if (r.exit_code != 0) return {};
    // sha256sum format: "<64-hex>  <filename>\n"
    auto sp = r.output.find(' ');
    if (sp == std::string::npos || sp != 64) return {};
    return r.output.substr(0, 64);
}

std::string make_release_tarball(const std::filesystem::path& root,
                                 std::string_view name,
                                 std::string_view version,
                                 const std::filesystem::path& output)
{
    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);

    auto prefix = std::format("{}-{}/", name, version);
    auto cmd = std::format(
        "git -C {} archive --format=tar.gz "
        "--prefix={} "
        "-o {} HEAD 2>&1",
        mcpp::platform::shell::quote(root.string()),
        mcpp::platform::shell::quote(prefix),
        mcpp::platform::shell::quote(output.string()));
    auto r = mcpp::platform::process::capture(cmd);
    if (r.exit_code != 0) {
        return std::format("git archive failed (rc={}): {}", r.exit_code, r.output);
    }
    if (!std::filesystem::exists(output)) {
        return std::format("git archive exited 0 but no tarball at '{}'",
                           output.string());
    }
    return {};
}

ReleaseInfo make_release_info(std::string_view version,
                              std::string_view url,
                              std::string_view sha256)
{
    ReleaseInfo r;
    r.version = std::string(version);
    auto fill = [&](ReleaseInfo::PerPlatform& pp) {
        pp.url    = std::string(url);
        pp.sha256 = std::string(sha256);
    };
    fill(r.linux);
    fill(r.macosx);
    fill(r.windows);
    return r;
}

} // namespace mcpp::pm
