// mcpp.cli.cmd_xpkg — `mcpp xpkg parse <file.lua>`: parse an xpkg
// descriptor's mcpp segment with EXACTLY the code the resolver uses at
// build time (synthesize_from_xpkg_lua + identity), so index lint and the
// user's build can never disagree on the grammar (single source of truth —
// design: .agents/docs/2026-07-08-index-version-semantics-and-descriptor-
// grammar-design.md D2).
//
// Strict by default: unknown mcpp-segment keys are an ERROR — a key this
// binary doesn't know would be silently ignored at build time, which under
// the index-floor discipline means either a typo or a descriptor that needs
// a newer floor. `--allow-unknown` downgrades them to warnings.

module;
#include <cstdio>

export module mcpp.cli.cmd_xpkg;

import std;
import mcpplibs.cmdline;
import mcpp.manifest;
import mcpp.platform.axis;
import mcpp.ui;
import mcpp.wire;
import mcpp.libs.json;

namespace mcpp::cli {

namespace {

// One exit for both spellings.
//
// `--json` prints the payload exactly as it has shipped -- a bare object, no
// envelope -- because consumers already read that shape and this repository's
// own e2e asserts its keys. `--format json` wraps the SAME payload. Two
// spellings of one answer; the payload is produced once so they cannot drift.
//
// The payload is built as a string at three sites in this file, so it is
// parsed back here rather than duplicating three builders. A parse failure
// means mcpp emitted invalid JSON, which is a bug worth surfacing loudly
// rather than papering over.
void emit_xpkg(std::string_view payload, bool enveloped) {
    if (!enveloped) { std::println("{}", payload); return; }
    auto j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        mcpp::ui::error("internal: xpkg payload is not valid JSON");
        j = nlohmann::json::object();
    }
    mcpp::wire::emit({.kind = "mcpp.xpkg", .data = std::move(j)});
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else
                    out += c;
        }
    }
    return out;
}

std::string json_array(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ",";
        out += "\"" + json_escape(v[i]) + "\"";
    }
    return out + "]";
}

} // namespace

export int cmd_xpkg_parse(const mcpplibs::cmdline::ParsedArgs& parsed) {
    auto file = parsed.positional(0);
    if (file.empty()) {
        mcpp::ui::error("usage: mcpp xpkg parse <file.lua> [--json] [--allow-unknown] [--all-os]");
        return 2;
    }
    std::ifstream is{std::filesystem::path{file}};
    if (!is) {
        mcpp::ui::error(std::format("cannot open '{}'", file));
        return 1;
    }
    std::string lua{std::istreambuf_iterator<char>(is), {}};

    bool enveloped = false;
    if (auto f = parsed.value("format")) {
        if (!mcpp::wire::parse_format(*f)) {
            std::println(stderr, "error: {}", mcpp::wire::unsupported_format(*f));
            return 2;
        }
        enveloped = true;
    }
    const bool asJson         = enveloped || parsed.is_flag_set("json");
    const bool allowUnknown   = parsed.is_flag_set("allow-unknown");
    const bool allOs          = parsed.is_flag_set("all-os");
    const bool allowSplitName = parsed.is_flag_set("allow-split-name");
    if (allOs && asJson) {
        mcpp::ui::error("--all-os and --json are mutually exclusive");
        return 2;
    }

    // Identity — same normalization the filename-lookup gate uses.
    auto id = mcpp::manifest::canonical_xpkg_identity_from_lua(lua);
    if (id.name.empty()) {
        mcpp::ui::error(std::format(
            "{}: no package identity (missing `package.name`)", file));
        return 1;
    }
    std::string fqn = id.ns.empty() ? id.name : id.ns + "." + id.name;

    // INV-NAME (#278) — the descriptor's `package.name` literal must BE the FQN.
    // This is the primary defense: index CI runs `mcpp xpkg parse`, so a
    // split-form descriptor is rejected in seconds instead of burning a
    // three-platform workspace job for an hour on an opaque E_NOT_FOUND.
    // The very same predicate guards the install path (prepare.cppm), so lint
    // and runtime can never drift apart.
    //
    // `--allow-split-name` exists because `package.namespace` carries a SECOND,
    // unrelated meaning in xlings-native indices (xim-pkgindex, -scode): there
    // it is an install-directory category (`config`, `scode`, `awesome`) and the
    // index is keyed by the bare `package.name`, so the split spelling is
    // correct in that world. mcpp's own indices (mcpplibs, anything carrying an
    // `index.toml` contract) use it as a package namespace, where INV-NAME
    // holds. Lints over an xlings-native tree should pass this flag.
    if (auto violation = allowSplitName
            ? std::nullopt
            : mcpp::manifest::xpkg_name_form_violation_from_lua(lua)) {
        if (asJson) {
            emit_xpkg(std::format(
                "{{\"namespace\":\"{}\",\"name\":\"{}\",\"error\":\"{}\"}}",
                json_escape(id.ns), json_escape(id.name),
                json_escape(*violation)), enveloped);
        } else {
            mcpp::ui::error(std::format("{}: {}", file, *violation));
        }
        return 1;
    }

    // Versions per platform (xpm table).
    static constexpr std::string_view kPlatforms[] = {"linux", "macosx", "windows"};
    std::map<std::string, std::vector<std::string>> versions;
    std::string anyVersion;
    for (auto plat : kPlatforms) {
        auto v = mcpp::manifest::list_xpkg_versions(
            lua, mcpp::platform::TargetPlatform::for_lint_of(plat));
        if (!v.empty() && anyVersion.empty()) anyVersion = v.front();
        versions.emplace(std::string(plat), std::move(v));
    }
    if (anyVersion.empty()) {
        mcpp::ui::error(std::format(
            "{}: xpm table declares no versions for any platform", file));
        return 1;
    }

    // Form A descriptors carry no `mcpp = {}` table — build info comes
    // from the fetched source's own mcpp.toml. Nothing further to parse.
    auto field = mcpp::manifest::extract_mcpp_field(lua);
    if (field.kind != mcpp::manifest::McppField::TableBody) {
        if (asJson) {
            emit_xpkg(std::format(
                "{{\"namespace\":\"{}\",\"name\":\"{}\",\"form\":\"A\"}}",
                json_escape(id.ns), json_escape(id.name)), enveloped);
        } else {
            std::println("package    {} (namespace '{}')", fqn, id.ns);
            std::println("form       A — no mcpp segment (build info from the "
                         "source's mcpp.toml)");
            std::println("parse OK");
        }
        return 0;
    }

    // --all-os: re-parse the descriptor once per OS section. The build path
    // splices only the running host's per-OS block and skip-tables the rest,
    // so a typo in e.g. the `windows` block is invisible on linux CI; this
    // is the lint-time closure over all three (design doc 2026-07-19 §4).
    if (allOs) {
        int allRc = 0;
        for (auto plat : kPlatforms) {
            // Only OSes the xpm table ships for: a linux+macosx-only package
            // legitimately has no windows section to validate.
            if (versions[std::string(plat)].empty()) {
                std::println("parse --   [{}]  (no xpm versions declared)", plat);
                continue;
            }
            auto pm = mcpp::manifest::synthesize_from_xpkg_lua(
                lua, fqn, anyVersion,
                mcpp::platform::TargetPlatform::for_lint_of(plat));
            if (!pm) {
                mcpp::ui::error(std::format("{} [{}]: {}", file, plat,
                                            pm.error().format()));
                allRc = 1;
                continue;
            }
            for (auto& k : pm->xpkgUnknownKeys) {
                auto msg = std::format(
                    "{} [{}]: unknown mcpp-segment key '{}' — silently "
                    "ignored at build time by this mcpp version", file, plat, k);
                if (allowUnknown) std::println(stderr, "warning: {}", msg);
                else              { mcpp::ui::error(msg); allRc = 1; }
            }
            std::println("parse OK [{}]  sources {}  includes {}  generated {}",
                         plat, pm->modules.sources.size(),
                         pm->buildConfig.includeDirs.size(),
                         pm->buildConfig.generatedFiles.size());
        }
        return allRc;
    }

    // The parse users get at build time — same function, same grammar.
    // Host axis here: `mcpp xpkg parse` with no --all-os is inspecting what
    // THIS machine would read, which is a diagnostic question about the host.
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, fqn, anyVersion, mcpp::platform::HostPlatform::current());
    if (!m) {
        mcpp::ui::error(std::format("{}: {}", file, m.error().format()));
        return 1;
    }

    int rc = 0;
    if (!m->xpkgUnknownKeys.empty()) {
        for (auto& k : m->xpkgUnknownKeys) {
            auto msg = std::format(
                "{}: unknown mcpp-segment key '{}' — silently ignored at "
                "build time by this mcpp version", file, k);
            if (allowUnknown) std::println(stderr, "warning: {}", msg);
            else              mcpp::ui::error(msg);
        }
        if (!allowUnknown) rc = 1;
    }

    if (asJson) {
        std::string genFiles = "[";
        bool first = true;
        for (auto& [path, content] : m->buildConfig.generatedFiles) {
            if (!first) genFiles += ",";
            first = false;
            genFiles += std::format("{{\"path\":\"{}\",\"bytes\":{}}}",
                                    json_escape(path.generic_string()), content.size());
        }
        genFiles += "]";
        std::string targets = "[";
        for (std::size_t i = 0; i < m->targets.size(); ++i) {
            if (i) targets += ",";
            targets += "\"" + json_escape(m->targets[i].name) + "\"";
        }
        targets += "]";
        std::string vers = "{";
        {
            bool f2 = true;
            for (auto& [plat, v] : versions) {
                if (!f2) vers += ",";
                f2 = false;
                vers += "\"" + plat + "\":" + json_array(v);
            }
        }
        vers += "}";
        std::string genContents = "{";
        {
            bool f3 = true;
            for (auto& [path, content] : m->buildConfig.generatedFiles) {
                if (!f3) genContents += ",";
                f3 = false;
                genContents += "\"" + json_escape(path.generic_string()) + "\":\"" +
                               json_escape(content) + "\"";
            }
        }
        genContents += "}";
        emit_xpkg(std::format(
                     "{{\"namespace\":\"{}\",\"name\":\"{}\",\"versions\":{},"
                     "\"standard\":\"{}\",\"import_std\":{},\"sources\":{},"
                     "\"include_dirs\":{},\"generated_files\":{},"
                     "\"generated_contents\":{},\"targets\":{},"
                     "\"unknown_keys\":{}}}",
                     json_escape(id.ns), json_escape(id.name), vers,
                     json_escape(m->package.standard),
                     m->language.importStd ? "true" : "false",
                     json_array(m->modules.sources),
                     json_array([&] {
                         std::vector<std::string> dirs;
                         for (auto& d : m->buildConfig.includeDirs)
                             dirs.push_back(d.generic_string());
                         return dirs;
                     }()),
                     genFiles, genContents, targets,
                     json_array(m->xpkgUnknownKeys)), enveloped);
        return rc;
    }

    std::println("package    {} (namespace '{}')", fqn, id.ns);
    for (auto& [plat, v] : versions) {
        if (v.empty()) continue;
        std::string joined;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) joined += ", ";
            joined += v[i];
        }
        std::println("versions   {:<8} {}", plat, joined);
    }
    std::println("standard   {}  import_std={}", m->package.standard,
                 m->language.importStd);
    if (!m->modules.sources.empty())
        std::println("sources    {}", m->modules.sources.size());
    if (!m->buildConfig.includeDirs.empty())
        std::println("includes   {}", m->buildConfig.includeDirs.size());
    for (auto& [path, content] : m->buildConfig.generatedFiles)
        std::println("generated  {} ({} bytes)", path.generic_string(), content.size());
    for (auto& t : m->targets)
        std::println("target     {}", t.name);
    if (!m->featuresMap.empty())
        std::println("features   {}", m->featuresMap.size());
    if (rc == 0) std::println("parse OK");
    return rc;
}

} // namespace mcpp::cli
