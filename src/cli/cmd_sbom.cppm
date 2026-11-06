// mcpp.cli.cmd_sbom — the dependency graph, in the format a procurement
// process asks for.
//
// THIS IS AN OUTPUT FORMAT, NOT A NEW MECHANISM, AND THAT IS THE WHOLE
// REASON IT IS CHEAP.
//
// Everything a software bill of materials names — which components went into
// this artefact, at which versions, from which source, with which integrity
// value — is already recorded in `mcpp.lock`, because that file exists to
// record exactly what a build resolved. `mcpp sbom` reads it and writes it out
// in a shape a legal or security review can consume. It resolves nothing,
// builds nothing and asks the network for nothing.
//
// AND IT READS THE LOCK RATHER THAN RE-RESOLVING, WHICH IS THE POINT. An
// SBOM produced by resolving again would describe a graph that may differ from
// the one that was built — which is the single thing an SBOM must never do.
// If the lock is stale, `--locked` is the tool that says so; this command
// reports what was recorded and says when nothing was.
//
// CycloneDX 1.5 rather than SPDX: it is JSON, its `components` array maps onto
// the lock's package list without inventing structure, and it is what the
// scanners in this space ingest. SPDX can be generated from the same data if a
// consumer needs it; nothing here is format-specific except `render`.

export module mcpp.cli.cmd_sbom;

import std;
import mcpp.lockfile;
import mcpp.version;
import mcpp.manifest;
import mcpp.project;
import mcpp.ui;
import mcpplibs.cmdline;

namespace {

// JSON string escaping, kept local: the values here are package names,
// versions and URLs, and pulling a JSON library in for six characters would be
// a dependency this module does not otherwise need.
std::string esc(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    o += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else o += c;
        }
    }
    return o;
}

// One segment of a package URL: the purl specification percent-encodes all but
// the unreserved characters and `:`. `+` is the one that occurs here, in a
// version's build metadata and in a variant's name.
std::string purl_segment(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '.' || c == '-' || c == '_' || c == '~' || c == ':')
            o += c;
        else
            o += std::format("%{:02X}", static_cast<unsigned>(u));
    }
    return o;
}

std::string purl(std::string_view ns, std::string_view name, std::string_view version) {
    return std::format("pkg:mcpp/{}/{}@{}", purl_segment(ns.empty() ? "mcpp" : ns),
                       purl_segment(name), purl_segment(version));
}

// A package URL for an mcpp package. `pkg:` URLs are how every SBOM consumer
// correlates a component with an advisory feed, so emitting one is what makes
// the document useful rather than merely well-formed. It therefore states the
// package's identity, not the spelling of the manifest that declared it: the
// lock may record `mcpplibs.cmdline` or `cmdline` for one package. A variant
// keeps its `+p<digest>` in the name: a patched library is another library.
std::string purl(const mcpp::lockfile::LockedPackage& p) {
    return purl(p.namespace_, mcpp::pm::short_name(p), p.version);
}

// AN UNKNOWN LICENCE IS REPORTED AS UNKNOWN, NEVER OMITTED AND NEVER
// GUESSED. A component with no `licenses` key reads as "not examined"; one
// carrying a wrong identifier reads as examined and is worse than silence. The
// lock records no licence — it records resolution — so unless the package's own
// manifest is present locally, this is genuinely not known here, and the
// document says so in a field a reviewer can filter on.
std::string licence_block(std::string_view spdx) {
    if (spdx.empty())
        return R"(      "licenses": [ { "license": { "name": "NOASSERTION" } } ],)";
    return std::format(
        "      \"licenses\": [ {{ \"license\": {{ \"id\": \"{}\" }} }} ],",
        esc(spdx));
}

}  // namespace

export namespace mcpp::cli {

int cmd_sbom(const mcpplibs::cmdline::ParsedArgs& parsed) {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error(
            "`mcpp sbom` must run inside a package (no mcpp.toml found)");
        return 2;
    }

    // The effective manifest: a workspace member's `[package]` fields may come
    // from `[workspace.package]` (#690, W4).
    auto effective = mcpp::project::load_effective_manifest(*root);
    if (!effective) {
        mcpp::ui::error(effective.error());
        return 2;
    }
    std::optional<mcpp::manifest::Manifest> man = std::move(effective->manifest);

    const auto lockPath = *root / "mcpp.lock";
    std::vector<mcpp::lockfile::LockedPackage> pkgs;
    if (auto lock = mcpp::lockfile::load(lockPath)) {
        pkgs = lock->packages;
    } else if (std::filesystem::exists(lockPath)) {
        mcpp::ui::error(std::format(
            "mcpp.lock exists but could not be read: {}", lock.error().message));
        return 2;
    } else {
        // NOT AN ERROR, AND NOT SILENCE EITHER. A project with no
        // dependencies has no lock and its bill of materials is one component,
        // which is a true answer. A project that has never been built also has
        // no lock, and that answer would be false. The note distinguishes them
        // for the reader, who is the only one who knows which case they are in.
        mcpp::ui::info("note",
            "no mcpp.lock: reporting the root package only. If this project has "
            "dependencies, build it once so the resolution is recorded.");
    }

    // One component per package: a lock written by 2026.9.29.1 to .5 can hold
    // one identity under two spellings, and a bom-ref is unique in a document.
    std::set<std::string> listed;
    std::erase_if(pkgs, [&](const auto& d) { return !listed.insert(purl(d)).second; });

    const auto& p = man->package;
    const std::string rootPurl = purl(p.namespace_, p.name, p.version);

    std::string out;
    out += "{\n";
    out += "  \"bomFormat\": \"CycloneDX\",\n";
    out += "  \"specVersion\": \"1.5\",\n";
    out += "  \"version\": 1,\n";
    out += "  \"metadata\": {\n";
    out += "    \"tools\": [ { \"name\": \"mcpp\", \"version\": \""
         + esc(std::string(mcpp::MCPP_VERSION)) + "\" } ],\n";
    out += "    \"component\": {\n";
    out += "      \"type\": \"application\",\n";
    out += "      \"bom-ref\": \"" + esc(rootPurl) + "\",\n";
    out += "      \"name\": \"" + esc(p.name) + "\",\n";
    out += "      \"version\": \"" + esc(p.version) + "\",\n";
    out += licence_block(p.license) + "\n";
    out += "      \"purl\": \"" + esc(rootPurl) + "\"\n";
    out += "    }\n";
    out += "  },\n";
    out += "  \"components\": [";

    bool first = true;
    for (auto const& d : pkgs) {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    {\n";
        out += "      \"type\": \"library\",\n";
        out += "      \"bom-ref\": \"" + esc(purl(d)) + "\",\n";
        out += "      \"name\": \"" + esc(mcpp::pm::short_name(d)) + "\",\n";
        out += "      \"version\": \"" + esc(d.version) + "\",\n";
        if (!d.namespace_.empty())
            out += "      \"group\": \"" + esc(d.namespace_) + "\",\n";
        out += licence_block({}) + "\n";
        // The integrity value the lock recorded. `fnv1a:` entries are mcpp's
        // own resolution digest rather than a content hash, so they are emitted
        // as a property rather than as a `hashes` entry — claiming a weak
        // digest is a cryptographic hash is the kind of statement an SBOM is
        // read to trust.
        std::vector<std::pair<std::string, std::string>> properties;
        if (d.hash.starts_with("sha256:")) {
            out += "      \"hashes\": [ { \"alg\": \"SHA-256\", \"content\": \""
                 + esc(d.hash.substr(7)) + "\" } ],\n";
        } else if (!d.hash.empty()) {
            properties.emplace_back("mcpp:resolution-digest", d.hash);
        }
        // A VARIANT STATES ITS PEDIGREE (design 2026-11-02 §14.1): the
        // component is the package changed by these patches, in this order,
        // and each patch is named with its SHA-256, so that what was changed
        // in a third party's source is part of what the bill of materials
        // says rather than something a reader has to find.
        if (!d.variant.empty()) {
            out += "      \"pedigree\": { \"patches\": [";
            bool firstPatch = true;
            for (auto const& patch : d.patches) {
                const auto at = patch.rfind("#sha256:");
                const auto path = at == std::string::npos ? patch : patch.substr(0, at);
                out += std::format("{} {{ \"type\": \"unofficial\", \"diff\": {{ \"url\": \"{}\" }} }}",
                                   firstPatch ? "" : ",", esc(path));
                firstPatch = false;
                properties.emplace_back("mcpp:patch", patch);
            }
            out += " ] },\n";
            properties.emplace_back("mcpp:variant", d.variant);
        }
        if (!properties.empty()) {
            out += "      \"properties\": [";
            for (std::size_t i = 0; i < properties.size(); ++i)
                out += std::format("{} {{ \"name\": \"{}\", \"value\": \"{}\" }}", i ? "," : "",
                                   esc(properties[i].first), esc(properties[i].second));
            out += " ],\n";
        }
        if (!d.source.empty())
            out += "      \"externalReferences\": [ { \"type\": \"distribution\","
                   " \"url\": \"" + esc(d.source) + "\" } ],\n";
        out += "      \"purl\": \"" + esc(purl(d)) + "\"\n";
        out += "    }";
    }
    out += first ? "]\n" : "\n  ]\n";
    out += "}\n";

    if (auto o = parsed.value("output")) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(*o).parent_path(), ec);
        std::ofstream f(*o, std::ios::binary);
        if (!f) {
            mcpp::ui::error(std::format("cannot write {}", *o));
            return 2;
        }
        f << out;
        mcpp::ui::info("sbom", std::format("wrote {} ({} component{})", *o,
            pkgs.size(), pkgs.size() == 1 ? "" : "s"));
    } else {
        std::print("{}", out);
    }
    return 0;
}

}  // namespace mcpp::cli
