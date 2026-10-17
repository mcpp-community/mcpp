// mcpp.pm.package_fetcher — packages through the provider port: search,
// plan, install, the index list, a package's descriptor and its payload.
//
// Each operation goes to the provider (mcpp.provider.select): which program
// runs, its protocol and its layout are the adapter's. A payload's
// completeness is the provider's answer (its `payload_status`, design §7.1):
// mcpp writes nothing into a payload -- no marker, no copy from another home.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.pm.package_fetcher;

import std;
import mcpp.config;
import mcpp.log;
import mcpp.platform;         // env::offline_mode
import mcpp.manifest;        // xpkg_lua_identity_matches — descriptor identity gate
import mcpp.pm.compat;
import mcpp.pm.dep_spec;
import mcpp.pm.index_contract;   // index.toml floor check (E0006)
import mcpp.pm.refresh_policy;   // decide_for_miss / apply (#648 A5)
import mcpp.build.refusal;       // offline-download-required (#648 A1)
import mcpp.ui;
import mcpp.pm.index_spec;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.libs.json;

export namespace mcpp::pm {

// --- Events parsed from NDJSON (aliases to mcpp::xlings types) ---

enum class EventKind { Progress, Log, Data, Error, Heartbeat, Result };

using ProgressEvent = mcpp::provider::ProgressEvent;
using LogEvent      = mcpp::provider::LogEvent;
using DataEvent     = mcpp::provider::DataEvent;
using ErrorEvent    = mcpp::provider::ErrorEvent;
using ResultEvent   = mcpp::provider::ResultEvent;
using Event         = mcpp::provider::Event;

// --- Listener interface (alias to mcpp::provider::EventHandler) ---

using EventHandler = mcpp::provider::EventHandler;

// --- Capability invocation ---

struct CallError {
    std::string message;
};

using CallResult = mcpp::provider::CallResult;

// What a caller wants of one payload (design 2026-10-17 D3).
//
// A packaging revision the index raised is a newer packaging, not a broken
// payload: a command that only USES a package keeps an installed payload while
// the provider calls it usable, and only an explicit request for the package
// (`mcpp toolchain install`) brings the index's current packaging. 2026.10.16.x
// replaced every payload whose revision was not the index's inside the build
// that read it -- a 101.7 MB gcc and a 36.7 MB glibc on a user's next
// `mcpp run`, and, on a home with one dangling record, a failure on every run.
enum class PayloadIntent {
    Locate,     // the installed payload when it is usable; never installs
    Use,        // a usable payload; installs or replaces only one that is not
    Provision,  // the index's current packaging; also replaces a usable payload that is not current
};

// What to do about one payload, from what the provider says of it. Pure: the
// unit tests pin the whole matrix.
enum class PayloadAction {
    Take,       // use the payload that is there
    Install,    // nothing is there: install it
    Replace,    // something is there and will not do: replace it
    Refuse,     // Locate, and nothing usable is there
};
PayloadAction payload_action(PayloadIntent intent, bool present, bool usable, bool current);

// What a failed install calls for, by the provider's error code (D5). A
// deterministic refusal (`E_INTERNAL`: a hook or a metadata transaction that
// refused; `E_PERMISSION`, `E_DISK_FULL`, `E_CANCELLED`) is said once and not
// retried, and no index refresh can help it; only a package the provider could
// not find refreshes the index first. No code at all (a provider that said
// nothing, #238) keeps the old conservative path.
struct InstallFailurePolicy {
    bool refreshIndex = false;   // the index may predate this version
    bool retry = false;          // a second attempt can succeed
};
InstallFailurePolicy install_failure_policy(std::string_view errorCode);

class Fetcher {
public:
    explicit Fetcher(const config::GlobalConfig& cfg) : cfg_(cfg) {}

    // Generic capability call. The handler (if non-null) receives streaming
    // events. On success returns aggregate CallResult.
    std::expected<CallResult, CallError>
        call(std::string_view capability,
             std::string_view argsJson,
             EventHandler* handler = nullptr);

    // High-level helpers (parsed payload for common ops).

    struct SearchHit {
        std::string source;       // "mcpplibs"
        std::string name;
        std::string description;
    };
    std::expected<std::vector<SearchHit>, CallError>
        search(std::string_view keyword);

    // #487 — what the package behind a search hit publishes: its descriptor's
    // per-OS version tables merged semver-descending. Empty when the hit
    // carries no `ns:name` prefix or has no readable descriptor — display
    // degrades to the bare line, the search itself never fails on this.
    std::vector<std::string> versions_for_hit(const SearchHit& hit) const;

    struct PackageInstall {
        std::string name;
        std::string version;
        std::string action;       // "download" | "skip" | ...
        std::string url;
        std::string sha256;
    };
    // Plan: report what install_packages WOULD do, no side effects.
    std::expected<std::vector<PackageInstall>, CallError>
        plan_install(const std::vector<std::string>& targets);

    // Actually install. Streams progress events to the handler if set.
    std::expected<CallResult, CallError>
        install(const std::vector<std::string>& targets,
                EventHandler* handler = nullptr);

    // List + manage index repos.
    struct RepoInfo { std::string name, url; bool isDefault; };
    std::expected<std::vector<RepoInfo>, CallError> list_repos();
    std::expected<void, CallError> add_repo(std::string_view name, std::string_view url);
    std::expected<void, CallError> remove_repo(std::string_view name);

    // ─── Namespace-aware overloads (canonical, 0.0.10+) ────────────
    //
    // These are the primary APIs. They use structured (namespace, shortName)
    // to look up packages, with compat fallback candidates generated by
    // mcpp::pm::compat::xpkg_lua_candidates / install_dir_candidates.

    // Install path for an installed package. Returns the directory under
    // $MCPP_HOME/registry/data/xpkgs/<dir>/<version>/ if it exists.
    std::optional<std::filesystem::path>
        install_path(std::string_view ns, std::string_view shortName,
                     std::string_view version) const;

    // Read the raw xpkg .lua file content for a package from the cloned
    // index. Uses namespace-aware candidate filenames.
    std::optional<std::string>
        read_xpkg_lua(std::string_view ns, std::string_view shortName) const;

    // Read the raw xpkg .lua file content from a local path index.
    // Used for [indices] entries with `path = "/some/dir"`.
    static std::optional<std::string>
        read_xpkg_lua_from_path(const std::filesystem::path& indexPath,
                                std::string_view ns,
                                std::string_view shortName);

    // Read xpkg .lua from a project-level xlings data directory.
    // Used for custom git indices whose clone lives under the project's
    // .mcpp/ directory rather than the global xlings home.
    static std::optional<std::string>
        read_xpkg_lua_from_project_data(const std::filesystem::path& projectDir,
                                        std::string_view ns,
                                        std::string_view shortName);

    // Install path under a project-level xlings data directory.
    static std::optional<std::filesystem::path>
        install_path_from_project_data(const std::filesystem::path& projectDir,
                                       std::string_view ns,
                                       std::string_view shortName,
                                       std::string_view version);

    // ─── did-you-mean scan (#278) — DIAGNOSTIC ONLY ────────────────
    //
    // Enumerate every descriptor under the given index roots and return those
    // whose canonical SHORT name equals `shortName`, each with its FQN and the
    // merged per-OS version list it publishes (#487), so a failed lookup can
    // tell the user the exact line to write AND what versions exist behind it,
    // instead of leaving them to guess a namespace or re-run xpkg parse.
    //
    // THREE CONSTRAINTS — this must never grow into an `IdentityIndex`:
    //   1. Call it ONLY on an already-failed path. Successful resolution must
    //      not pay for a directory walk.
    //   2. Its result feeds ERROR TEXT / SEARCH DISPLAY ONLY. It must never be
    //      written back into a DependencySpec, the lockfile, or the install
    //      layer — bare names resolving across arbitrary namespaces is
    //      precisely the reproducibility hazard this design rejects (design
    //      §3.2/§4.2). The version list is under the same rule: it describes
    //      what exists, it is not a resolution input.
    //   3. An empty result is not an error. It downgrades the message, never
    //      the exit code.
    struct ShortNameMatch {
        std::string fqn;                    // "ns.name"
        std::vector<std::string> versions;  // semver-desc, may be empty

        // " (1.0.0, 0.9.0, ...)" — empty when nothing is published/readable.
        // The list and its truncation marker come from the one place that
        // decides them, so this line and `mcpp search` cannot drift apart.
        std::string versions_label() const {
            if (versions.empty()) return {};
            return " (" + mcpp::manifest::join_versions_desc(versions) + ")";
        }
    };
    static std::vector<ShortNameMatch>
        scan_short_name_matches(const std::vector<std::filesystem::path>& indexRoots,
                                std::string_view shortName);

    // The index roots a plain `mcpp build` would consult (global registry data
    // dirs). Used to feed `scan_short_name_matches` on the failure path.
    std::vector<std::filesystem::path> builtin_index_roots() const;

    // ─── Legacy overloads (COMPAT(internal:legacy-fetcher-api)) ─────────────
    //
    // Accept a raw package name string and infer namespace from it.
    // New code should use the (ns, shortName) overloads above.

    std::optional<std::filesystem::path>
        install_path(std::string_view name, std::string_view version) const;

    std::optional<std::string>
        read_xpkg_lua(std::string_view package_name) const;

    // Unified xpkg payload (one xlings package's installed location and its
    // standard subdirs). Returned by resolve_xpkg_path().
    struct XpkgPayload {
        std::filesystem::path        root;           // xpkg install root
        std::filesystem::path        binDir;         // root/bin/  (or empty)
        std::filesystem::path        libDir;         // root/lib/  (or empty)
        std::filesystem::path        includeDir;     // root/include/ (or empty)
        std::filesystem::path        sourceDir;      // primary source subdir (or root)
        bool                         hasBin     = false;
        bool                         hasLib     = false;
        bool                         hasInclude = false;
    };

    // Resolve a target like "xim:gcc@15.1.0" or "name@1.0" to its xpkg
    // payload location, installing through the provider as `intent` allows
    // (PayloadIntent, design 2026-10-17 D3).
    //
    // Target syntax:
    //   "<index>:<name>@<version>"   explicit index namespace
    //   "<name>@<version>"           uses xim namespace by default
    std::expected<XpkgPayload, CallError>
        resolve_xpkg_path(std::string_view target,
                          PayloadIntent intent,
                          EventHandler* handler = nullptr);

private:
    const config::GlobalConfig& cfg_;

    // Build the env-prefixed command for a capability call.
    std::string build_command(std::string_view capability,
                              std::string_view argsJson) const;
};

// ─── install_packages failure diagnostics (#238) ────────────────────
//
// When `xlings install_packages` exits non-zero, mcpp historically reported
// only `fetch '<pkg>@<ver>' failed (exit 1)`. The known #238 failure mode —
// ≥2 project-level index_repos (root [indices] inheritance + a default-ns
// redirect, per #224) making the xlings resolver silently fail — left the
// user with an opaque exit code and no next step. The child emits
// {"exitCode":1,"kind":"result"} with NO error event (see
// mcpp.fetcher.progress), so mcpp must reconstruct actionable context from
// what IT knows: the target, the configured index repos, and whatever the
// child DID log. The two helpers below are pure/self-contained so the message
// shape is unit-testable without a live xlings.

// Read the `index_repos` entry names out of a seeded `.xlings.json`. Returns
// the entries as "name" (or "name -> url" when a url is present). Missing or
// unreadable file yields an empty vector — callers treat "unknown" gracefully.
std::vector<std::string>
read_seeded_index_repos(const std::filesystem::path& xlingsJson);

// Compose the enriched, actionable error for an install_packages exit!=0.
//   target      : "<ns.name>@<version>" (or index-scoped "idx:ns.name@ver")
//   exitCode    : the child's non-zero exit
//   indexRepos  : names/identities of the configured index repos (see reader)
//   childError  : any error/warn text the child DID emit (may be empty)
// When ≥2 repos are configured the message flags the known xlings-side gap
// and links the tracking issue; otherwise it still names the target, the repo
// set, and points at MCPP_VERBOSE=1 for the raw xlings output.
std::string
format_install_failure_diagnostic(std::string_view target,
                                  int exitCode,
                                  std::span<const std::string> indexRepos,
                                  std::string_view childError = {});

} // namespace mcpp::pm

namespace mcpp::pm {

namespace {

// A field of a data event's JSON: a string, or an object/array as compact
// JSON text (what the readers below walk). Empty when absent.
std::string extract_string(std::string_view text, std::string_view key) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return {};
    auto it = j.find(std::string(key));
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
std::string extract_object(std::string_view text, std::string_view key) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return {};
    auto it = j.find(std::string(key));
    return it != j.end() && (it->is_object() || it->is_array()) ? it->dump() : std::string{};
}

mcpp::provider::Location location_of(const config::GlobalConfig& cfg) {
    return mcpp::config::make_xlings_env(cfg);
}

} // namespace

std::string Fetcher::build_command(std::string_view capability,
                                   std::string_view argsJson) const
{
    return mcpp::provider::open(location_of(cfg_))->operation_command(capability, argsJson);
}

// ─── install_packages failure diagnostics (#238) ────────────────────

std::vector<std::string>
read_seeded_index_repos(const std::filesystem::path& xlingsJson)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(xlingsJson, ec)) return out;
    std::ifstream in(xlingsJson, std::ios::binary);
    if (!in) return out;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    // The array is emitted by seed_xlings_json as:
    //   "index_repos": [ { "name": "..", "url": ".." }, ... ]
    // Parse just the slice between "index_repos": [ ... matching ] and pull
    // each object's name/url. Cheap hand-roll (matches the module's existing
    // manual-JSON style); tolerant of missing url.
    auto key = text.find("\"index_repos\"");
    if (key == std::string::npos) return out;
    auto lb = text.find('[', key);
    if (lb == std::string::npos) return out;
    // find matching ']' at the same bracket depth.
    std::size_t p = lb + 1;
    int depth = 1;
    std::size_t rb = std::string::npos;
    bool in_str = false;
    for (; p < text.size(); ++p) {
        char c = text[p];
        if (in_str) {
            if (c == '\\' && p + 1 < text.size()) { ++p; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '[') ++depth;
        else if (c == ']') { if (--depth == 0) { rb = p; break; } }
    }
    if (rb == std::string::npos) return out;
    std::string_view arr(text.data() + lb + 1, rb - lb - 1);

    // Whitespace-tolerant field reader: seed_xlings_json PRETTY-prints the
    // objects (`"name": "value"` — space after the colon), whereas the shared
    // extract_string assumes compact NDJSON (`"name":"value"`). Reusing it here
    // silently matched nothing. Read `"key"` then skip ws/`:` then the quoted
    // value ourselves. (Repo names/urls carry no escaped quotes.)
    auto field = [](std::string_view obj, std::string_view key) -> std::string {
        std::string needle = std::format("\"{}\"", key);
        auto p = obj.find(needle);
        if (p == std::string_view::npos) return "";
        p += needle.size();
        while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == ':'))
            ++p;
        if (p >= obj.size() || obj[p] != '"') return "";
        ++p;
        std::string out;
        while (p < obj.size() && obj[p] != '"') out.push_back(obj[p++]);
        return out;
    };

    // Walk each { ... } object inside the array.
    std::size_t q = 0;
    while (q < arr.size()) {
        auto ob = arr.find('{', q);
        if (ob == std::string_view::npos) break;
        auto oe = arr.find('}', ob);
        if (oe == std::string_view::npos) break;
        auto obj = arr.substr(ob, oe - ob + 1);
        auto name = field(obj, "name");
        auto url  = field(obj, "url");
        if (!name.empty()) {
            out.push_back(url.empty() ? name
                                      : std::format("{} -> {}", name, url));
        }
        q = oe + 1;
    }
    return out;
}

std::string
format_install_failure_diagnostic(std::string_view target,
                                  int exitCode,
                                  std::span<const std::string> indexRepos,
                                  std::string_view childError)
{
    std::string repoList;
    for (std::size_t i = 0; i < indexRepos.size(); ++i) {
        if (i) repoList += ", ";
        repoList += indexRepos[i];
    }

    std::string msg = std::format(
        "xlings install_packages failed (exit {}) for '{}'", exitCode, target);

    if (indexRepos.empty()) {
        msg += " with an unknown index-repo configuration";
    } else {
        msg += std::format(" with {} index repo{} configured [{}]",
                           indexRepos.size(),
                           indexRepos.size() == 1 ? "" : "s",
                           repoList);
    }

    if (indexRepos.size() >= 2) {
        msg += "; ≥2 project-level index repos is a known xlings resolution "
               "gap that can make install_packages silently exit non-zero "
               "(mcpp #238; root cause openxlings/xlings#374 — the resolver "
               "does not report which repo/package failed)";
    }

    if (!childError.empty()) {
        msg += std::format("\n  xlings reported: {}", childError);
    } else {
        msg += "\n  xlings emitted no structured error; re-run with "
               "MCPP_VERBOSE=1 to see the raw xlings invocation + output.";
    }

    // If an index was too new for this binary, THAT is the cause, and the
    // not-found above is only its symptom. Without this the last line the user
    // reads names a wire address and a missing package — pointing at naming or
    // publication — while the real answer (E0006, printed much earlier and
    // possibly scrolled away) says "upgrade mcpp". The message that stops the
    // build has to carry the reason the build stopped.
    if (auto hint = mcpp::pm::unusable_index_hint(); !hint.empty()) {
        msg += "\n" + hint;
    }
    return msg;
}

std::expected<CallResult, CallError>
Fetcher::call(std::string_view capability,
              std::string_view argsJson,
              EventHandler* handler)
{
    auto provider = mcpp::provider::open(location_of(cfg_));
    mcpp::log::debug("fetcher", std::format("call: cap='{}'", capability));
    if (mcpp::log::is_enabled(mcpp::log::Level::debug)) {
        auto cmd = provider->operation_command(capability, argsJson);
        mcpp::log::debug("fetcher", std::format("cmd = {}", cmd));
    }
    auto r = provider->call(capability, argsJson, handler);
    if (!r) {
        mcpp::log::error("fetcher", std::format("call '{}' failed: {}", capability, r.error()));
        return std::unexpected(CallError{r.error()});
    }
    mcpp::log::debug("fetcher", std::format("call '{}' exitCode={}", capability, r->exitCode));
    return *r;
}

// --- Helpers ---

namespace {

// Build args JSON for vector<string> targets.
std::string make_targets_args(const std::vector<std::string>& targets,
                              bool yes = false)
{
    std::string out = "{\"targets\":[";
    for (std::size_t i = 0; i < targets.size(); ++i) {
        out += "\"" + targets[i] + "\"";
        if (i + 1 < targets.size()) out += ",";
    }
    out += "]";
    if (yes) out += ",\"yes\":true";
    out += "}";
    return out;
}

} // namespace

std::expected<std::vector<Fetcher::SearchHit>, CallError>
Fetcher::search(std::string_view keyword) {
    auto args = std::format("{{\"keyword\":\"{}\"}}", keyword);
    auto r = call("search_packages", args);
    if (!r) return std::unexpected(r.error());

    std::vector<SearchHit> hits;
    for (auto& d : r->dataEvents) {
        if (d.dataKind != "styled_list") continue;
        // payload: {"items":[["name","desc"], ...], "title":"..."}
        // Crude parse: find "items": [ ... ] and extract pairs.
        auto items = extract_object(d.payloadJson, "items");
        // items looks like [["a","b"],["c","d"]]
        // Walk character by character.
        std::size_t p = 0;
        while (p < items.size()) {
            if (items[p] == '[') {
                // start of a pair
                ++p;
                // first string
                while (p < items.size() && items[p] != '"') ++p;
                std::string name;
                if (p < items.size() && items[p] == '"') {
                    ++p;
                    while (p < items.size() && items[p] != '"') {
                        if (items[p] == '\\' && p + 1 < items.size()) { p += 2; continue; }
                        name.push_back(items[p++]);
                    }
                    if (p < items.size()) ++p;     // skip closing "
                }
                // skip until next "
                while (p < items.size() && items[p] != '"' && items[p] != ']') ++p;
                std::string desc;
                if (p < items.size() && items[p] == '"') {
                    ++p;
                    while (p < items.size() && items[p] != '"') {
                        if (items[p] == '\\' && p + 1 < items.size()) { p += 2; continue; }
                        desc.push_back(items[p++]);
                    }
                    if (p < items.size()) ++p;
                }
                // record
                if (!name.empty()) {
                    hits.push_back({cfg_.defaultIndex, std::move(name), std::move(desc)});
                }
                // advance past ]
                while (p < items.size() && items[p] != ']') ++p;
                if (p < items.size()) ++p;
            } else {
                ++p;
            }
        }
    }
    return hits;
}

std::vector<std::string>
Fetcher::versions_for_hit(const SearchHit& hit) const {
    // Hit names arrive as `ns:name` from the xlings listing; a bare name has
    // no routable identity, so there is nothing to read.
    auto colon = hit.name.find(':');
    if (colon == std::string::npos || colon == 0
        || colon + 1 == hit.name.size())
        return {};
    auto ns = std::string_view(hit.name).substr(0, colon);
    auto shortName = std::string_view(hit.name).substr(colon + 1);
    auto lua = read_xpkg_lua(ns, shortName);
    if (!lua) return {};
    return mcpp::manifest::all_xpkg_versions_desc(*lua);
}

std::expected<std::vector<Fetcher::PackageInstall>, CallError>
Fetcher::plan_install(const std::vector<std::string>& targets) {
    auto args = make_targets_args(targets);
    auto r = call("plan_install", args);
    if (!r) return std::unexpected(r.error());

    std::vector<PackageInstall> plan;
    // Look in data events for install_plan; payload has "steps":[{...}, ...]
    for (auto& d : r->dataEvents) {
        if (d.dataKind != "install_plan") continue;
        auto stepsArr = extract_object(d.payloadJson, "steps");
        // Walk objects within stepsArr (each {...}).
        int depth = 0;
        std::size_t start = 0;
        for (std::size_t p = 0; p < stepsArr.size(); ++p) {
            char c = stepsArr[p];
            if (c == '{') {
                if (depth == 0) start = p;
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    auto obj = stepsArr.substr(start, p - start + 1);
                    PackageInstall pi;
                    pi.name    = extract_string(obj, "name");
                    pi.version = extract_string(obj, "version");
                    pi.action  = extract_string(obj, "action");
                    pi.url     = extract_string(obj, "url");
                    pi.sha256  = extract_string(obj, "sha256");
                    if (!pi.name.empty()) plan.push_back(std::move(pi));
                }
            }
        }
    }
    return plan;
}

std::expected<CallResult, CallError>
Fetcher::install(const std::vector<std::string>& targets, EventHandler* handler) {
    auto args = make_targets_args(targets, /*yes=*/true);
    return call("install_packages", args, handler);
}

std::expected<std::vector<Fetcher::RepoInfo>, CallError>
Fetcher::list_repos() {
    auto r = call("list_repos", "{}");
    if (!r) return std::unexpected(r.error());

    std::vector<RepoInfo> repos;
    for (auto& d : r->dataEvents) {
        // payload format varies; styled_list with [name, url] pairs is common.
        auto items = extract_object(d.payloadJson, "items");
        // Walk pairs as in search().
        std::size_t p = 0;
        while (p < items.size()) {
            if (items[p] == '[') {
                ++p;
                while (p < items.size() && items[p] != '"') ++p;
                std::string name;
                if (p < items.size() && items[p] == '"') {
                    ++p;
                    while (p < items.size() && items[p] != '"') {
                        if (items[p] == '\\' && p + 1 < items.size()) { p += 2; continue; }
                        name.push_back(items[p++]);
                    }
                    if (p < items.size()) ++p;
                }
                while (p < items.size() && items[p] != '"' && items[p] != ']') ++p;
                std::string url;
                if (p < items.size() && items[p] == '"') {
                    ++p;
                    while (p < items.size() && items[p] != '"') {
                        if (items[p] == '\\' && p + 1 < items.size()) { p += 2; continue; }
                        url.push_back(items[p++]);
                    }
                    if (p < items.size()) ++p;
                }
                if (!name.empty()) {
                    repos.push_back({ std::move(name), std::move(url), false });
                }
                while (p < items.size() && items[p] != ']') ++p;
                if (p < items.size()) ++p;
            } else { ++p; }
        }
    }
    return repos;
}

std::expected<void, CallError>
Fetcher::add_repo(std::string_view name, std::string_view url) {
    auto args = std::format("{{\"name\":\"{}\",\"url\":\"{}\"}}", name, url);
    auto r = call("add_repo", args);
    if (!r) return std::unexpected(r.error());
    if (r->exitCode != 0) {
        std::string msg = "add_repo failed";
        if (r->error) msg += ": " + r->error->message;
        return std::unexpected(CallError{msg});
    }
    return {};
}

std::expected<void, CallError>
Fetcher::remove_repo(std::string_view name) {
    auto args = std::format("{{\"name\":\"{}\"}}", name);
    auto r = call("remove_repo", args);
    if (!r) return std::unexpected(r.error());
    if (r->exitCode != 0) {
        std::string msg = "remove_repo failed";
        if (r->error) msg += ": " + r->error->message;
        return std::unexpected(CallError{msg});
    }
    return {};
}

// ─── Identity-verified descriptor lookup (shared) ───────────────────
//
// The single rule behind all three read_xpkg_lua entry points: a candidate
// filename is only a *hint* for where to look; the file's declared
// `package.{name,namespace}` is the proof of identity. A hit is accepted only
// when it actually declares the requested package, so a foreign bare
// `zlib.lua` never satisfies a request for `compat.zlib`.

namespace {

bool malformed_descriptor_targets(
    std::string_view content,
    std::string_view requestedNs,
    std::string_view requestedShortName,
    std::string_view indexDefaultNs)
{
    if (!mcpp::manifest::xpkg_name_form_violation_from_lua(content))
        return false;

    auto name = mcpp::manifest::extract_xpkg_name(content);
    auto effectiveNs = mcpp::manifest::extract_xpkg_namespace(content);
    if (effectiveNs.empty()) effectiveNs = std::string(indexDefaultNs);

    // Diagnostic attribution only: reconstruct the coordinate the malformed
    // dotted short name appears to spell so its author-facing error can be
    // surfaced. The descriptor is never accepted as that package.
    if (!effectiveNs.empty()) {
        auto legacyPrefix = effectiveNs + ".";
        if (name.starts_with(legacyPrefix))
            name.erase(0, legacyPrefix.size());
    }
    auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size())
        return false;

    std::string apparentNs = effectiveNs;
    if (!apparentNs.empty()) apparentNs.push_back('.');
    apparentNs += name.substr(0, dot);
    return apparentNs == requestedNs
        && name.substr(dot + 1) == requestedShortName;
}

// Try candidate filenames (canonical first) in a single index's pkgs/ dir;
// return the first hit whose DECLARED identity matches (ns, shortName).
std::optional<std::string>
read_identity_verified_xpkg_lua(const std::filesystem::path& pkgsDir,
                                std::string_view ns,
                                std::string_view shortName,
                                const std::vector<std::string>& filenames,
                                std::string_view indexDefaultNs = {})
{
    // Index→client version contract: the tree carries its own floor
    // (<indexRoot>/index.toml min_mcpp). Checked here — the single choke
    // point every transport converges on (artifact snapshot, git clone,
    // [indices] path, CI-restored cache).
    //
    // NOTHING IS PRINTED HERE. The fact is recorded, and this tree answers no
    // lookup. Whether it matters is decided later: a lookup that then fails
    // carries the cause in its own message (unusable_index_hint), and a run
    // that succeeds has no error to report. The read site used to print the
    // E0006 text as `error:` at the start of runs that went on to resolve every
    // package from another tree and exit 0.
    if (mcpp::pm::index_marked_unusable(pkgsDir.parent_path())
        || mcpp::pm::check_index_floor(pkgsDir.parent_path()))
        return std::nullopt;

    std::error_code ec;
    if (!std::filesystem::exists(pkgsDir, ec)) return std::nullopt;
    for (auto& fname : filenames) {
        if (fname.empty()) continue;
        char first = static_cast<char>(std::tolower(
            static_cast<unsigned char>(fname.front())));
        auto candidate = pkgsDir / std::string(1, first) / fname;
        if (!std::filesystem::exists(candidate, ec)) continue;

        std::ifstream is(candidate);
        std::stringstream ss; ss << is.rdbuf();
        auto content = ss.str();
        // A candidate filename is enough to attribute a malformed descriptor
        // to this lookup, even though malformed identity must never count as a
        // successful package match. Return the bytes so the route/build layer
        // can report the precise INV-NAME violation instead of degrading it to
        // a misleading "not found". The index-wide fallback below remains
        // selective: it attributes only a malformed name whose apparent
        // coordinate is exactly the one requested, never an arbitrary bad file.
        if (mcpp::manifest::xpkg_name_form_violation_from_lua(content)) {
            return content;
        }
        if (mcpp::manifest::xpkg_lua_identity_matches(
                content, ns, shortName, /*allowLegacyBareDefault=*/true,
                indexDefaultNs)) {
            return content;
        }
        // Filename matched but the descriptor is a different package — keep
        // scanning the remaining candidates / index dirs.
    }

    // ── Identity-first fallback: the filename is only a hint ────────
    //
    // SPEC-001 §3.4 — a descriptor's identity is its declared
    // `package.{namespace,name}`; the file may be called anything. The
    // candidate-filename probes above are a fast path for the RECOMMENDED
    // names (`<name>.lua` / `<namespace>.<name>.lua`), not a constraint: a
    // package filed under any other name must still resolve.
    //
    // This closes the "discovery half" of identity-first resolution — until now only
    // the VERIFY half was implemented (every hit re-checked against declared
    // identity), while discovery silently could not see a descriptor whose
    // filename matched no candidate.
    //
    // Cost: runs ONLY after every candidate missed, so a conforming index pays
    // nothing. Every hit goes through the same identity gate, so a scan can
    // never accept a package the filename path would have rejected.
    auto scan_dir = [&](const std::filesystem::path& dir)
        -> std::optional<std::string>
    {
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".lua") continue;
            std::ifstream is(entry.path());
            if (!is) continue;
            std::stringstream ss; ss << is.rdbuf();
            auto content = ss.str();
            if (malformed_descriptor_targets(
                    content, ns, shortName, indexDefaultNs)) {
                return content;
            }
            if (mcpp::manifest::xpkg_lua_identity_matches(
                    content, ns, shortName, /*allowLegacyBareDefault=*/true,
                    indexDefaultNs)) {
                return content;
            }
        }
        return std::nullopt;
    };

    if (auto hit = scan_dir(pkgsDir)) return hit;   // descriptors sitting flat
    for (auto& letterDir : std::filesystem::directory_iterator(pkgsDir, ec)) {
        if (!letterDir.is_directory(ec)) continue;
        if (auto hit = scan_dir(letterDir.path())) return hit;
    }
    return std::nullopt;
}

// Index dirs under a data root in deterministic (sorted) order, so cross-index
// resolution no longer depends on filesystem directory-iteration order.
std::vector<std::filesystem::path>
sorted_index_dirs(const std::filesystem::path& dataRoot)
{
    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    for (auto& entry : std::filesystem::directory_iterator(dataRoot, ec)) {
        if (entry.is_directory()) dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

} // namespace

// ─── Namespace-aware read_xpkg_lua (canonical, 0.0.10+) ─────────────

std::optional<std::string>
Fetcher::read_xpkg_lua(std::string_view ns, std::string_view shortName) const
{
    if (shortName.empty()) return std::nullopt;

    // Candidate filenames generated by compat module — canonical first,
    // then fallback candidates (see compat.cppm for deprecation schedule).
    auto filenames = mcpp::pm::compat::xpkg_lua_candidates(ns, shortName);

    for (auto& dir : mcpp::provider::open(location_of(cfg_))->index_roots()) {
        if (auto lua = read_identity_verified_xpkg_lua(
                dir / "pkgs", ns, shortName, filenames)) {
            return lua;
        }
    }
    return std::nullopt;
}

// ─── read_xpkg_lua from local path index ────────────────────────────
//
// For [indices] entries with `path = "/some/dir"`, read the xpkg .lua
// directly from the filesystem. The index layout follows the standard
// mcpp-index convention: <path>/pkgs/<first-letter>/<name>.lua

std::optional<std::string>
Fetcher::read_xpkg_lua_from_path(const std::filesystem::path& indexPath,
                                  std::string_view ns,
                                  std::string_view shortName)
{
    if (shortName.empty()) return std::nullopt;

    auto filenames = mcpp::pm::compat::xpkg_lua_candidates(ns, shortName);
    // A `[indices]` path index is scoped to a single namespace (`ns`), so a
    // descriptor here that declares no namespace belongs to it.
    return read_identity_verified_xpkg_lua(
        indexPath / "pkgs", ns, shortName, filenames, /*indexDefaultNs=*/ns);
}

// ─── read_xpkg_lua from project-level data dir ─────────────────────
//
// For custom git indices cloned into the project xlings data roots,
// scan the data directory the same way the global read_xpkg_lua does.

std::optional<std::string>
Fetcher::read_xpkg_lua_from_project_data(const std::filesystem::path& projectDir,
                                          std::string_view ns,
                                          std::string_view shortName)
{
    if (shortName.empty()) return std::nullopt;

    auto filenames = mcpp::pm::compat::xpkg_lua_candidates(ns, shortName);

    // The project's own indices (design §5.1), then the data roots a project
    // index was cloned into before it had a place of its own.
    std::vector<std::filesystem::path> dirs =
        mcpp::provider::layout().index_roots_of(projectDir / ".mcpp");
    for (auto& data : mcpp::config::project_xlings_data_roots(projectDir))
        for (auto& dir : sorted_index_dirs(data))
            if (std::ranges::find(dirs, dir) == dirs.end()) dirs.push_back(dir);
    for (auto& dir : dirs) {
        if (auto lua = read_identity_verified_xpkg_lua(
                dir / "pkgs", ns, shortName, filenames)) {
            return lua;
        }
    }
    return std::nullopt;
}

// ─── install_path from project-level data dir ──────────────────────
//
// For packages installed under project xlings data roots by custom git indices.

std::optional<std::filesystem::path>
Fetcher::install_path_from_project_data(const std::filesystem::path& projectDir,
                                         std::string_view ns,
                                         std::string_view shortName,
                                         std::string_view version)
{
    std::vector<std::filesystem::path> bases =
        mcpp::provider::layout().stores_of(projectDir / ".mcpp");
    for (auto& data : mcpp::config::project_xlings_data_roots(projectDir))
        bases.push_back(mcpp::provider::layout().store_in(data));
    for (auto& base : bases) {
        if (!std::filesystem::exists(base)) continue;

        // Try canonical directory name: ns.shortName
        auto qname = std::string(ns) + "." + std::string(shortName);
        auto verdir = base / qname / std::string(version);
        if (std::filesystem::exists(verdir)) return verdir;

        // Try shortName alone.
        verdir = base / std::string(shortName) / std::string(version);
        if (std::filesystem::exists(verdir)) return verdir;

        auto indexName = ns.empty()
            ? std::string{mcpp::pm::kDefaultNamespace}
            : std::string{ns};
        for (auto& dirName : mcpp::pm::compat::install_dir_candidates(
                 ns, shortName, indexName)) {
            verdir = base / dirName / std::string(version);
            if (std::filesystem::exists(verdir)) return verdir;
        }
    }

    return std::nullopt;
}

// ─── did-you-mean scan (#278) — DIAGNOSTIC ONLY ─────────────────────
//
// See the declaration for the three constraints that keep this from becoming a
// resolution path. Deliberately tolerant: unreadable files are skipped, not
// reported — a diagnostic helper must never turn into a second failure mode.

std::vector<Fetcher::ShortNameMatch>
Fetcher::scan_short_name_matches(
    const std::vector<std::filesystem::path>& indexRoots,
    std::string_view shortName)
{
    std::vector<ShortNameMatch> hits;
    if (shortName.empty()) return hits;

    std::error_code ec;
    for (auto& root : indexRoots) {
        auto pkgsDir = root / "pkgs";
        if (!std::filesystem::exists(pkgsDir, ec)) continue;
        for (auto& letterDir : std::filesystem::directory_iterator(pkgsDir, ec)) {
            if (!letterDir.is_directory(ec)) continue;
            for (auto& entry :
                     std::filesystem::directory_iterator(letterDir.path(), ec)) {
                if (entry.path().extension() != ".lua") continue;
                std::ifstream is(entry.path());
                if (!is) continue;
                std::stringstream ss; ss << is.rdbuf();
                auto content = ss.str();

                auto id = mcpp::manifest::canonical_xpkg_identity_from_lua(content);
                if (id.name != shortName) continue;
                if (id.ns.empty()) continue;   // bare upstream: already reachable

                // #487 — the descriptor is already in memory, so the version
                // union costs one more pass over text we had to read anyway.
                // Display-only: constraint 2 above applies to it verbatim.
                hits.push_back({id.ns + "." + id.name,
                                mcpp::manifest::all_xpkg_versions_desc(content)});
            }
        }
    }

    std::sort(hits.begin(), hits.end(),
              [](auto const& a, auto const& b) { return a.fqn < b.fqn; });
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](auto const& a, auto const& b) {
                               return a.fqn == b.fqn;
                           }),
               hits.end());
    return hits;
}

std::vector<std::filesystem::path>
Fetcher::builtin_index_roots() const
{
    return mcpp::provider::open(location_of(cfg_))->index_roots();
}

// ─── Legacy read_xpkg_lua (COMPAT(internal:legacy-fetcher-api)) ─────────────────
//
// Infers namespace from the raw package_name string and delegates to the
// namespace-aware overload.

std::optional<std::string>
Fetcher::read_xpkg_lua(std::string_view package_name) const
{
    if (package_name.empty()) return std::nullopt;
    auto resolved = mcpp::pm::compat::resolve_package_name(package_name, "");
    return read_xpkg_lua(resolved.namespace_, resolved.shortName);
}

// --- resolve_xpkg_path ---

namespace {

struct ParsedTarget {
    std::string indexName;       // "xim", "mcpplibs", or "" (default)
    std::string packageName;
    std::string version;
};

ParsedTarget parse_target(std::string_view target, std::string_view defaultIndex) {
    ParsedTarget r;
    auto colon = target.find(':');
    auto at    = target.find('@');
    if (at == std::string_view::npos) {
        // No version → caller may need to default
        if (colon != std::string_view::npos) {
            r.indexName = std::string(target.substr(0, colon));
            r.packageName = std::string(target.substr(colon + 1));
        } else {
            r.indexName = std::string(defaultIndex);
            r.packageName = std::string(target);
        }
        return r;
    }
    r.version = std::string(target.substr(at + 1));
    if (colon != std::string_view::npos && colon < at) {
        r.indexName   = std::string(target.substr(0, colon));
        r.packageName = std::string(target.substr(colon + 1, at - colon - 1));
    } else {
        r.indexName   = std::string(defaultIndex);
        r.packageName = std::string(target.substr(0, at));
    }
    return r;
}

// D22: what to do about a failed install depends on why it failed, and xlings
// says why (`code`, interface 1.5+; extraction since 2026.10.10.2). Every
// failure used to end in "check network and retry": a phone whose app sandbox
// refused hard links was sent to check its network (design 2026-10-10 §13).
std::string install_failure_hint(const std::optional<mcpp::provider::ErrorEvent>& error,
                                 const std::filesystem::path& home) {
    const std::string code = error ? error->code : std::string{};
    if (code == "E_NETWORK")
        return "check network and retry, or `mcpp self init --force`";
    if (code == "E_DISK_FULL")
        return std::format("free space on the disk that holds {}, then retry", home.string());
    if (code == "E_PERMISSION")
        return std::format("a file under {} could not be written: check the owner and "
                           "permissions of its directory, then retry", home.string());
    if (code == "E_INVALID_INPUT")
        return "the downloaded archive was damaged and has been dropped; a retry downloads it again";
    if (error && !error->hint.empty()) return error->hint;
    // A refusal the provider decided (a hook, a metadata transaction): the
    // same command fails the same way, so retrying is not the advice (D5).
    if (code == "E_INTERNAL")
        return "the provider refused the change; `mcpp self doctor` reports the state of mcpp's "
               "package home, and the whole failure is in ~/.mcpp/log/mcpp.log";
    return "the whole failure is in ~/.mcpp/log/mcpp.log (`mcpp self doctor` checks the "
           "installation); retry, or `mcpp self init --force`";
}

} // namespace

std::expected<Fetcher::XpkgPayload, CallError>
Fetcher::resolve_xpkg_path(std::string_view target,
                           PayloadIntent intent,
                           EventHandler* handler)
{
    // Default to xim namespace for tools/toolchain. Modular libs use
    // mcpplibs but consumers (cli) typically pass "<idx>:<name>@<ver>"
    // explicitly anyway.
    auto parsed = parse_target(target, "xim");
    if (parsed.packageName.empty() || parsed.version.empty()) {
        return std::unexpected(CallError{
            std::format("invalid xpkg target '{}': expected `<name>@<version>`",
                        target)});
    }

    auto provider = mcpp::provider::open(location_of(cfg_));
    const mcpp::provider::PackageRef ref{parsed.indexName, parsed.packageName, parsed.version};

    mcpp::log::verbose("fetcher", std::format("resolve: target='{}'", target));

    auto fill_payload = [&](XpkgPayload& p) {
        p.binDir     = p.root / "bin";
        p.libDir     = p.root / "lib";
        p.includeDir = p.root / "include";
        p.hasBin     = std::filesystem::exists(p.binDir);
        p.hasLib     = std::filesystem::exists(p.libDir);
        p.hasInclude = std::filesystem::exists(p.includeDir);
        // sourceDir: prefer single subdir (extracted tarball), else root.
        std::error_code ec;
        std::vector<std::filesystem::path> subs;
        if (std::filesystem::is_directory(p.root)) {
            for (auto& e : std::filesystem::directory_iterator(p.root, ec)) {
                if (e.is_directory()) subs.push_back(e.path());
            }
        }
        p.sourceDir = (subs.size() == 1) ? subs.front() : p.root;
    };

    // The payload as the provider placed it: a version dir that holds bin/,
    // include/ or lib/ is the root (xim packages); otherwise its one
    // subdirectory (an extracted source tarball, mcpplibs packages).
    auto make_payload = [&](const std::filesystem::path& verdir) -> XpkgPayload {
        XpkgPayload payload;
        std::error_code ec;
        std::vector<std::filesystem::path> subs;
        for (auto& e : std::filesystem::directory_iterator(verdir, ec)) {
            if (e.is_directory()) subs.push_back(e.path());
        }
        bool verdir_is_root = std::filesystem::exists(verdir / "bin")
                           || std::filesystem::exists(verdir / "include")
                           || std::filesystem::exists(verdir / "lib");
        payload.root = verdir_is_root            ? verdir
                     : (subs.size() == 1)        ? subs.front()
                     :                              verdir;
        fill_payload(payload);
        return payload;
    };

    // What the provider says of this payload, nothing installed (D1): the
    // provider decides completeness, usability against the recipe's floor and
    // currency; this function only maps them through the caller's intent.
    struct Here {
        std::filesystem::path dir;      // empty: nothing there
        bool usable = false, current = false;
        int installed = -1, revision = 0, floor = 0;
    };
    const auto here = [&]() -> Here {
        const std::array<mcpp::provider::PackageRef, 1> refs{ref};
        auto st = provider->status(refs);
        if (st.size() == 1 && st[0].origin != "absent" && !st[0].dir.empty()) {
            // Present: a complete one (usable / current as the provider says),
            // or an interrupted install, or a success that installed nothing
            // (#533) -- the provider's verdicts say which.
            return {st[0].dir, st[0].usable, st[0].current,
                    st[0].installedRevision, st[0].revision, st[0].floor};
        }
        // The provider could not say, or has nothing where the index's store
        // is: what is installed elsewhere in the home, by its version grammar,
        // holding something -- a payload installed before its index had a
        // store of its own (COMPAT(internal:legacy-registry-store), design
        // 2026-10-16 §5.3). Nothing to judge it by: it is taken as it is.
        auto dir = provider->payload(ref);
        std::error_code ec;
        if (!dir.empty() && std::filesystem::is_directory(dir, ec) && !std::filesystem::is_empty(dir, ec))
            return {dir, true, true};
        return {};
    };

    const auto label = std::format("{}@{}", parsed.packageName, parsed.version);
    auto now = here();
    const auto action = payload_action(intent, !now.dir.empty(), now.usable, now.current);
    if (action == PayloadAction::Take) {
        if (!now.current)
            mcpp::log::verbose("fetcher", std::format(
                "{}: packaging revision {} in use; the index has {} (floor {})",
                label, now.installed, now.revision, now.floor));
        return make_payload(now.dir);
    }
    if (action == PayloadAction::Refuse) {
        return std::unexpected(CallError{std::format(
            "payload of '{}' is not installed\n"
            "  hint: `mcpp toolchain install` installs a toolchain; a build installs what it needs",
            mcpp::provider::to_string(ref))});
    }

    // Offline is checked HERE, after the "already usable" fast path above: a
    // build whose packages are all usable stays functional offline. Only an
    // actual download is refused, naming the package.
    if (mcpp::platform::env::offline_mode()) {
        mcpp::build::refusal::record(
            mcpp::build::refusal::Code::OfflineDownloadRequired);
        return std::unexpected(CallError{ std::format(
            "offline mode: `{}` is not installed and cannot be downloaded\n"
            "       run without --offline (or unset MCPP_OFFLINE) to fetch it",
            label) });
    }

    // A replacement says why, once, before it starts (D3): the user sees a
    // download in a command that only builds, so the reason is part of it.
    const bool replacing = action == PayloadAction::Replace;
    const bool replacingUsable = replacing && now.usable;
    if (replacing) {
        if (now.usable)
            mcpp::ui::status("Updating", std::format("{}  packaging revision {} → {}",
                                                     label, now.installed, now.revision));
        else if (now.installed >= 0 && now.installed < now.floor)
            mcpp::ui::status("Updating", std::format(
                "{}  packaging revision {} → {} (revision {} is below the index's floor {})",
                label, now.installed, now.revision, now.installed, now.floor));
        else
            mcpp::ui::status("Reinstalling", std::format("{}  (the installed payload is incomplete)", label));
    }

    if (parsed.indexName == "xim" && !provider->official_package_present(parsed.packageName)) {
        // A descriptor missing from the local xim index may be one the
        // remote index has. Whether that miss refreshes is the refresh
        // policy's decision (#648 A5).
        auto decision = mcpp::pm::decide_for_miss(
            mcpp::pm::policy_for(cfg_), location_of(cfg_),
            std::format("xim:{}", parsed.packageName));
        if (auto r = mcpp::pm::apply(decision, location_of(cfg_)); !r)
            mcpp::ui::warning(r.error());
    }

    // Satisfied: what the intent asks of the payload now there.
    const auto satisfied = [&]() -> std::optional<std::filesystem::path> {
        auto after = here();
        if (after.dir.empty()) return std::nullopt;
        if (intent == PayloadIntent::Provision ? after.current : after.usable) return after.dir;
        return std::nullopt;
    };

    std::vector<std::string> targets { mcpp::provider::to_string(ref) };
    mcpp::log::verbose("fetcher", std::format("install: {}", targets[0]));
    auto inst = install(targets, handler);
    if (!inst) return std::unexpected(inst.error());
    if (inst->exitCode == 0) {
        if (auto dir = satisfied()) return make_payload(*dir);
        // Reported a success and left nothing complete (mcpp#533's shape):
        // said, not stamped over.
        return std::unexpected(CallError{std::format(
            "'{}' reported a successful install, but its payload is not complete\n"
            "  hint: {}", targets[0], install_failure_hint(inst->error, cfg_.xlingsHome()))});
    }

    // The install failed. What happens next depends on why (D5).
    const std::string code = inst->error ? inst->error->code : std::string{};
    const auto policy = install_failure_policy(code);
    int directRc = -1;
    if (policy.refreshIndex && parsed.indexName == "xim") {
        auto decision = mcpp::pm::decide_for_miss(
            mcpp::pm::policy_for(cfg_), location_of(cfg_), targets[0],
            mcpp::pm::RefreshReason::InstallMiss);
        if (auto r = mcpp::pm::apply(decision, location_of(cfg_)); !r)
            mcpp::log::verbose("fetcher", r.error());
    }
    if (policy.retry) {
        // The provider's own command line: more robust for a large toolchain,
        // and its output stays visible when it fails again.
        mcpp::log::verbose("fetcher", std::format("retrying {} through the provider's command line", targets[0]));
        directRc = provider->install_direct(targets[0]);
        if (directRc == 0)
            if (auto dir = satisfied()) return make_payload(*dir);
    }

    std::string installError = std::format("install of '{}' failed (interface exit {}{})", targets[0],
        inst->exitCode, policy.retry ? std::format(", direct exit {}", directRc) : std::string{});
    if (inst->error)
        installError += ": " + inst->error->message;

    // A replacement that failed leaves the previous payload where it was (the
    // provider's replacement is a transaction). Only a provisioning command
    // replaces a payload that is still usable, so only it can fail with one
    // left in place: it says which revision stays in use. A build replaces
    // only a payload that is not usable, and has nothing to fall back to.
    if (replacingUsable) {
        auto after = here();
        if (!after.dir.empty() && after.usable)
            return std::unexpected(CallError{std::format(
                "{}\n  packaging revision {} is still installed and in use\n  hint: {}",
                installError, after.installed, install_failure_hint(inst->error, cfg_.xlingsHome()))});
    }
    return std::unexpected(CallError{std::format(
        "{}\n  hint: {}", installError,
        install_failure_hint(inst->error, cfg_.xlingsHome()))});
}

PayloadAction payload_action(PayloadIntent intent, bool present, bool usable, bool current) {
    if (present && usable && (intent != PayloadIntent::Provision || current)) return PayloadAction::Take;
    if (intent == PayloadIntent::Locate) return PayloadAction::Refuse;
    return present ? PayloadAction::Replace : PayloadAction::Install;
}

InstallFailurePolicy install_failure_policy(std::string_view errorCode) {
    if (errorCode == "E_NOT_FOUND") return {.refreshIndex = true, .retry = true};
    if (errorCode.empty()) return {.refreshIndex = true, .retry = true};
    if (errorCode == "E_NETWORK" || errorCode == "E_INVALID_INPUT") return {.refreshIndex = false, .retry = true};
    return {};
}

// ─── Namespace-aware install_path (canonical, 0.0.10+) ──────────────

std::optional<std::filesystem::path>
Fetcher::install_path(std::string_view ns, std::string_view shortName,
                      std::string_view version) const
{
    // M6.x: install_path returns the verdir (untouched extract root).
    // Layout discrimination is done by the caller via the xpkg.lua's
    // `mcpp = "<glob path>"` (Form A pointer) or `mcpp = { ... }` (Form B
    // inline) field.
    // Every store a payload of this home may be in: the store of the index
    // that serves it, then the home's, which holds what was installed before
    // that index had a store of its own (design 2026-10-16 §5.3).
    const auto qname = mcpp::pm::compat::qualified_name(ns, shortName);
    const auto candidates = mcpp::pm::compat::install_dir_candidates(ns, shortName, cfg_.defaultIndex);
    std::vector<std::string> acceptedPrefixes;
    if (!ns.empty()) acceptedPrefixes.emplace_back(ns);
    if (!cfg_.defaultIndex.empty()) acceptedPrefixes.emplace_back(cfg_.defaultIndex);
    for (const auto& base : mcpp::provider::open(location_of(cfg_))->stores()) {
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) continue;
        auto try_dir = [&](std::string_view dirName) -> std::optional<std::filesystem::path> {
            auto verdir = base / std::string(dirName) / std::string(version);
            return std::filesystem::exists(verdir, ec) ? std::optional<std::filesystem::path>{verdir} : std::nullopt;
        };
        // Candidate directory names generated by compat module -- canonical
        // first, then fallback candidates (see compat.cppm for deprecation
        // schedule).
        for (auto& dirName : candidates)
            if (auto p = try_dir(dirName)) return *p;

        // Last-resort fallback scan (COMPAT(internal:legacy-fetcher-api)):
        // walk the store for a directory holding this package under an older
        // layout.
        //
        // The bare `-x-<shortName>` arm is bound to the namespaces the caller
        // actually asked for. Unbound, it returned ANY namespace's directory
        // with a matching short name -- `ocornut:imgui` resolving to
        // `compat-x-imgui` -- and install_path's contract is "the verdir for
        // THIS package", so the install was skipped and another package's
        // tree read in its place. See the header of mcpp.fallback.legacy_dirs
        // for why version alignment is what made it reachable.
        if (auto legacy = mcpp::provider::layout().legacy_install_dir(base, qname, shortName, acceptedPrefixes);
            !legacy.empty())
            if (auto p = try_dir(legacy)) return *p;
    }
    return std::nullopt;
}

// ─── Legacy install_path (COMPAT(internal:legacy-fetcher-api)) ──────────────────
//
// Infers namespace from the raw name string and delegates to the
// namespace-aware overload.

std::optional<std::filesystem::path>
Fetcher::install_path(std::string_view name, std::string_view version) const
{
    auto resolved = mcpp::pm::compat::resolve_package_name(name, "");
    return install_path(resolved.namespace_, resolved.shortName, version);
}

} // namespace mcpp::pm
