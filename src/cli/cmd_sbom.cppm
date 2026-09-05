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

// A package URL for an mcpp package. `pkg:` URLs are how every SBOM consumer
// correlates a component with an advisory feed, so emitting one is what makes
// the document useful rather than merely well-formed.
std::string purl(const mcpp::lockfile::LockedPackage& p) {
    const std::string ns = p.namespace_.empty() ? std::string("mcpp")
                                                : p.namespace_;
    return std::format("pkg:mcpp/{}/{}@{}", ns, p.name, p.version);
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

    auto man = mcpp::manifest::load(*root / "mcpp.toml");
    if (!man) {
        mcpp::ui::error(man.error().message);
        return 2;
    }

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

    const auto& p = man->package;
    const std::string rootNs = p.namespace_.empty() ? std::string("mcpp")
                                                    : p.namespace_;

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
    out += "      \"bom-ref\": \"" + esc(std::format("pkg:mcpp/{}/{}@{}",
                rootNs, p.name, p.version)) + "\",\n";
    out += "      \"name\": \"" + esc(p.name) + "\",\n";
    out += "      \"version\": \"" + esc(p.version) + "\",\n";
    out += licence_block(p.license) + "\n";
    out += "      \"purl\": \"" + esc(std::format("pkg:mcpp/{}/{}@{}",
                rootNs, p.name, p.version)) + "\"\n";
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
        out += "      \"name\": \"" + esc(d.name) + "\",\n";
        out += "      \"version\": \"" + esc(d.version) + "\",\n";
        if (!d.namespace_.empty())
            out += "      \"group\": \"" + esc(d.namespace_) + "\",\n";
        out += licence_block({}) + "\n";
        // The integrity value the lock recorded. `fnv1a:` entries are mcpp's
        // own resolution digest rather than a content hash, so they are emitted
        // as a property rather than as a `hashes` entry — claiming a weak
        // digest is a cryptographic hash is the kind of statement an SBOM is
        // read to trust.
        if (d.hash.starts_with("sha256:")) {
            out += "      \"hashes\": [ { \"alg\": \"SHA-256\", \"content\": \""
                 + esc(d.hash.substr(7)) + "\" } ],\n";
        } else if (!d.hash.empty()) {
            out += "      \"properties\": [ { \"name\": \"mcpp:resolution-digest\","
                   " \"value\": \"" + esc(d.hash) + "\" } ],\n";
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
