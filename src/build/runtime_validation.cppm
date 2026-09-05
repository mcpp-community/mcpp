// mcpp.build.runtime_validation — validate only freshly linked Linux ELFs.
//
// The backend snapshots link outputs before ninja and compares their stat
// fingerprints afterwards.  An unchanged no-op build therefore performs zero
// ELF parses.  Verdicts are persisted beside build.ninja and keyed by artifact
// stat + RuntimeBinding contract so doctor can explain the last result without
// probing the host again.
//
// THE RECORD LIVES IN `.mcpp-runtime-verdicts.json`, NOT IN `resolution.json`.
// `prepare_build` rewrites the latter from an empty object at the start of
// every invocation, so a verdict recorded there is deleted before the next run
// can read it back -- which is what made two of these three passes re-parse
// every image on every command (#529). `resolution.json` publishes a copy after
// the link and stays the documented place to read one.

export module mcpp.build.runtime_validation;

import std;
import mcpp.build.loader_contract;
import mcpp.build.plan;
import mcpp.build.symbol_provision;
import mcpp.manifest;
import mcpp.libs.json;
import mcpp.platform;
import mcpp.runtime.elf;
import mcpp.runtime.binding;
import mcpp.ui;
import mcpp.platform.runtime_search;

export namespace mcpp::build::runtime_validation {

struct ArtifactStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;

    bool operator==(const ArtifactStamp&) const = default;
};

using ArtifactSnapshot = std::map<std::filesystem::path, ArtifactStamp>;

struct ValidatedArtifact {
    std::filesystem::path artifact;
    mcpp::platform::elf::RuntimeVerdict verdict;
    bool cacheHit = false;
};

struct ValidationReport {
    std::vector<ValidatedArtifact> artifacts;

    // NOTE: there is deliberately no `has_blocking_failure()` here.
    //
    // There used to be a `has_proven_mismatch()`, and nothing ever called it —
    // the real gate walks the artifacts in `ninja_backend` so it can name WHICH
    // one failed and print its explanation. A second predicate that answers
    // "did anything fail" from the same data is the same decision in two
    // places, and the one with no callers is the one that silently stops
    // agreeing. Ask `verdict.blocking()` per artifact.
};

struct StoredRuntimeSummary {
    std::filesystem::path artifact;
    mcpp::platform::elf::RuntimeVerdict verdict;
    std::string contractHash;
};

ArtifactSnapshot snapshot_link_artifacts(const mcpp::build::BuildPlan& plan);

ValidationReport validate_changed_artifacts(
    const mcpp::build::BuildPlan& plan,
    const ArtifactSnapshot& before);

std::optional<StoredRuntimeSummary>
latest_stored_verdict(const std::filesystem::path& targetRoot);

// Fast paths may run ninja without reconstructing BuildPlan.  They are allowed
// only when every stored artifact still has the stat/contract fingerprint that
// was validated.  The returned snapshot can be compared after ninja; any
// change drops to the full path, which reconstructs the search closure and
// validates before reporting success/running the program.
std::optional<ArtifactSnapshot> validated_artifact_snapshot(
    const std::filesystem::path& outputDir,
    const mcpp::platform::runtime::RuntimeBinding& binding);

bool artifact_snapshot_unchanged(const ArtifactSnapshot& snapshot);

// Does a declared runtime artifact actually resolve to the payload it claims?
//
// mcpp ALREADY enforces exactly this for the private libc: `glibc@2.44`
// resolves that one payload, a stale or missing one is an error, and it never
// picks "whichever installed version looks usable". Applying the same rule to
// every declared runtime artifact is consistency, not a new mechanism — and it
// is the whole check the graphics stack was missing, where a provider was
// declared at one version while the symlink on disk still resolved into the
// previous one. Nothing here knows what a driver is.
//
// FOUR-VALUED, and the last two are the point:
//
//   Ok          resolved real path lies under the declared version
//   Mismatch    it resolves somewhere else -- the binding is stale
//   Missing     declared, but nothing is there
//   Unverified  declared without a version to check against
//
// A two-valued answer would report Unverified as a pass, which is the failure
// mode this whole area keeps producing: "not checked" and "checked and fine"
// must not look the same.
enum class ArtifactVerdict { Ok, Mismatch, Missing, Unverified };

std::string_view to_string(ArtifactVerdict verdict);

ArtifactVerdict artifact_identity_verdict(
    const mcpp::manifest::RuntimeArtifact& artifact);

// Rule E — the loader-tag contract, evaluated on the artifacts this run
// produced, and RECORDED rather than only warned about.
//
// The record is the point. A warning scrolls past; `resolution.json` is the
// machine-readable answer to "what did the last build decide", so a tag
// deviation can be read by CI, by `mcpp why runtime`, and by a test — without
// anyone needing readelf on the box. It is also how "checked and compliant"
// stays distinguishable from "never checked": both look identical when the
// only output is the absence of a warning.
//
// `before` is the pre-ninja snapshot, same as validate_changed_artifacts takes:
// an artifact whose stat did not move was not produced by this run, so its
// verdict is READ BACK instead of re-derived from the ELF. The returned vector
// still covers every artifact either way.
//
// READ BACK FROM THE SIDECAR, NOT FROM `resolution.json`, and the distinction
// is the whole of #529. `prepare_build` regenerates `resolution.json` from an
// empty object at the start of every invocation, so a verdict recorded there
// was deleted before the next run could find it and the read-back never fired
// across processes — 1.36 s of a 1.94 s warm `mcpp test`, every time.
// `.mcpp-runtime-verdicts.json` survives, and `resolution.json` keeps
// publishing a copy after the link, which is how `sync_resolution_verdict`
// already handled the runtime verdicts. One authoritative writer, one
// published view.
std::vector<mcpp::build::loader::TagFinding>
check_and_record_loader_tags(const mcpp::build::BuildPlan& plan,
                             const ArtifactSnapshot& before);

// One image's answer to "is every symbol provided once" (issue #519).
struct SymbolProvisionFinding {
    std::filesystem::path                       artifact;
    mcpp::build::symbol_provision::Report       report;
};

// Evaluate the symbol-provision invariant on the images this run produced.
//
// A SEPARATE ENTRY POINT rather than more of validate_changed_artifacts,
// and the reason is a gate rather than tidiness: that function returns early
// unless the runtime binding's provider is glibc, because everything it checks
// is glibc closure physics. This check is ELF physics — a dynamically linked
// musl image has exactly the same flat namespace — so inheriting that gate
// would make it silently never run there. Same snapshot, same recording, its
// own applicability.
std::vector<SymbolProvisionFinding>
check_symbol_provision(const mcpp::build::BuildPlan& plan,
                       const ArtifactSnapshot& before);

} // namespace mcpp::build::runtime_validation

namespace mcpp::build::runtime_validation {
namespace {

constexpr std::string_view kCacheFile = ".mcpp-runtime-verdicts.json";

ArtifactStamp stamp(const std::filesystem::path& path) {
    ArtifactStamp out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return out;
    out.exists = true;
    out.size = std::filesystem::file_size(path, ec);
    if (ec) { out.exists = false; return out; }
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec) { out.exists = false; return out; }
    out.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
    return out;
}

std::string fingerprint(const std::filesystem::path& artifact,
                        const ArtifactStamp& value,
                        std::string_view contractHash) {
    auto input = std::format("{}\n{}\n{}\n{}",
        artifact.generic_string(), value.size, value.mtime, contractHash);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : input) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return std::format("{:016x}", hash);
}

std::string status_name(mcpp::platform::elf::RuntimeVerdict::Status status) {
    using Status = mcpp::platform::elf::RuntimeVerdict::Status;
    switch (status) {
        case Status::Pass: return "pass";
        case Status::ProvenMismatch: return "proven_mismatch";
        case Status::Unresolvable: return "unresolvable";
        case Status::Inconclusive: return "inconclusive";
    }
    return "inconclusive";
}

// Did this build declare that it reaches outside the sandbox?
//
// Spelled here exactly as `mcpp.build.hermetic` spells it — manifest key OR
// environment variable — because the two checks must agree. A build whose link
// was allowed to resolve host libraries and whose closure was then judged as if
// it had not is the worst of both: it links, and mcpp calls it broken.
bool host_libs_allowed(const mcpp::build::BuildPlan& plan) {
    if (plan.manifest.buildConfig.allowHostLibs) return true;
    const char* e = std::getenv("MCPP_ALLOW_HOST_LIBS");
    return e && *e && *e != '0';
}

// How bad each state is, for rolling many artifacts into one summary.
// `Unresolvable` sits above `Inconclusive` (it is proven, not unknown) and
// below `ProvenMismatch` (mixing payloads is the more fundamental error, and
// it is usually the CAUSE of anything unresolvable alongside it).
int status_severity(mcpp::platform::elf::RuntimeVerdict::Status status) {
    using Status = mcpp::platform::elf::RuntimeVerdict::Status;
    switch (status) {
        case Status::Pass:           return 0;
        case Status::Inconclusive:   return 1;
        case Status::Unresolvable:   return 2;
        case Status::ProvenMismatch: return 3;
    }
    return 1;
}

mcpp::platform::elf::RuntimeVerdict::Status
parse_status(std::string_view value) {
    using Status = mcpp::platform::elf::RuntimeVerdict::Status;
    if (value == "pass") return Status::Pass;
    if (value == "proven_mismatch") return Status::ProvenMismatch;
    if (value == "unresolvable") return Status::Unresolvable;
    // Anything unknown reads as `inconclusive`, never as `pass`: a record
    // written by a newer mcpp must not be mistaken for a clean bill of health.
    return Status::Inconclusive;
}

nlohmann::json read_cache(const std::filesystem::path& outputDir) {
    std::ifstream input(outputDir / kCacheFile);
    if (!input) return nlohmann::json::object();
    auto doc = nlohmann::json::parse(input, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return nlohmann::json::object();
    return doc;
}

void write_cache(const std::filesystem::path& outputDir,
                 const nlohmann::json& doc) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    auto path = outputDir / kCacheFile;
    auto tmp = outputDir / (std::string(kCacheFile) + ".tmp");
    {
        std::ofstream output(tmp, std::ios::trunc);
        if (!output) return;
        output << doc.dump(2) << '\n';
        if (!output) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) std::filesystem::remove(tmp, ec);
}

std::string cache_key(const mcpp::build::BuildPlan& plan,
                      const std::filesystem::path& artifact) {
    std::error_code ec;
    auto relative = std::filesystem::relative(artifact, plan.outputDir, ec);
    return ec ? artifact.lexically_normal().generic_string()
              : relative.lexically_normal().generic_string();
}

// ── The durable home of the post-link verdicts ──────────────────────────────
//
// WHY THESE RECORDS LIVE IN THE SIDECAR AND NOT IN `resolution.json`.
//
// Both post-link passes were written with a read-back: an artifact whose stat
// did not move keeps the verdict already on file instead of being re-parsed,
// because re-reading every image on every drive is what made the loader-tag
// check cost 158.7 s of a 190 s hot run. The read-back was correct and it never
// fired across invocations, because it read `resolution.json` — and
// `prepare_build` rewrites that file from a FRESH json object at the start of
// every invocation, carrying neither key. Every run therefore began by deleting
// the memo its own backend was about to look for.
//
// Measured on a ten-link-unit tree with nothing to do: 1.36 s of a 1.94 s
// `mcpp test`, every time.
//
// `resolution.json` keeps publishing both records — `mcpp why runtime`, doctor,
// e2e 214 and e2e 307 read them there — but it publishes a COPY, written after
// the link, exactly as `sync_resolution_verdict` already publishes the runtime
// verdicts. One authoritative writer, one published view.
constexpr std::string_view kLoaderTagsRecord      = "loader_tags";
constexpr std::string_view kSymbolProvisionRecord = "symbol_provision";

// WHAT INVALIDATES A STORED VERDICT BESIDES THE ARTIFACT ITSELF.
//
// Making the memo durable creates a correctness obligation that did not exist
// while every answer was recomputed: a verdict about which file satisfies a
// DT_NEEDED is a function of more than the artifact's stat.
//
//   the SubOS farm   `<subos>/lib` is a symlink view rewritten by every
//                    `xlings install`, and it sits on the artifact's runtime
//                    search path. Installing a package can change which file
//                    answers, with the artifact untouched. `.xlings.json` is
//                    that view's version stamp — `try_fast_build` already
//                    treats it as one.
//   the policy       `MCPP_ALLOW_HOST_LIBS` is read from the environment at
//                    check time and enters no fingerprint, so it can flip a
//                    verdict with every input file unchanged.
//
// Folded into one key rather than compared field by field, so a third input
// added later has one place to go.
std::string post_link_key(const mcpp::build::BuildPlan& plan) {
    std::string material = plan.runtimeBinding.contractHash;
    material += '\x1f';
    if (!plan.runtimeBinding.subosDir.empty()) {
        std::error_code ec;
        auto stampPath = plan.runtimeBinding.subosDir / ".xlings.json";
        auto size = std::filesystem::file_size(stampPath, ec);
        if (!ec) material += std::to_string(size);
        ec.clear();
        auto when = std::filesystem::last_write_time(stampPath, ec);
        if (!ec)
            material += std::to_string(
                static_cast<std::int64_t>(when.time_since_epoch().count()));
    }
    material += '\x1f';
    material += host_libs_allowed(plan) ? "host-libs" : "hermetic";
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : material) { hash ^= c; hash *= 0x100000001b3ull; }
    return std::format("{:016x}", hash);
}

// The stored entries for one pass, or an empty array when nothing usable is on
// file. A key mismatch reads as "nothing stored", which re-derives everything
// once — the safe direction, and the only one that keeps a stale farm from
// answering for a fresh one.
nlohmann::json stored_post_link(const nlohmann::json& doc,
                                std::string_view name,
                                std::string_view key) {
    if (!doc.is_object()) return nlohmann::json::array();
    if (doc.value("post_link_key", "") != key) return nlohmann::json::array();
    auto it = doc.find(std::string(name));
    if (it == doc.end() || !it->is_array()) return nlohmann::json::array();
    return *it;
}

// Merge this drive's findings over what is on file, and drop only what has
// LEFT THE DISK.
//
// Pruning on "not in the current plan" is the shape that made the sidecar's own
// pass cost 1.19 s in an edit-test loop: `mcpp build` and `mcpp test` share one
// output directory and have different link-unit sets — measured, the test plan
// contains the nine test binaries and not `bin/app` — so each command deleted
// the other's verdicts and both paid full price on every alternation. The
// record is a property of the output directory, not of whichever command last
// ran against it.
nlohmann::json merge_post_link(const nlohmann::json& stored,
                               nlohmann::json fresh,
                               const std::filesystem::path& outputDir) {
    std::set<std::string> covered;
    for (auto const& e : fresh)
        if (e.is_object()) covered.insert(e.value("path", ""));
    for (auto const& e : stored) {
        if (!e.is_object()) continue;
        auto rel = e.value("path", "");
        if (rel.empty() || covered.contains(rel)) continue;
        std::error_code ec;
        if (!std::filesystem::exists(outputDir / rel, ec)) continue;
        fresh.push_back(e);
    }
    std::sort(fresh.begin(), fresh.end(), [](auto const& a, auto const& b) {
        return a.value("path", "") < b.value("path", "");
    });
    return fresh;
}

// Store the authoritative copy, then publish the readable one.
//
// The publish half is not decoration: `mcpp why runtime`, `mcpp doctor` and two
// e2e tests read `runtime.<name>` out of `resolution.json`, and that file is the
// documented place to look (docs/05). What changed is which copy survives an
// invocation — the sidecar's — so the published one can be regenerated from it
// rather than being the only one there was.
void persist_post_link(const mcpp::build::BuildPlan& plan,
                       std::string_view name, std::string_view key,
                       const nlohmann::json& entries) {
    auto doc = read_cache(plan.outputDir);
    if (!doc.is_object()) doc = nlohmann::json::object();
    // A key change invalidates the OTHER pass's entries too — they were derived
    // under the same farm and the same policy — so they go with it rather than
    // being silently carried across as if they had been re-checked.
    const bool keyMoved = doc.value("post_link_key", "") != key;
    if (keyMoved) {
        doc.erase(std::string(kLoaderTagsRecord));
        doc.erase(std::string(kSymbolProvisionRecord));
    }
    if (keyMoved || doc.value(std::string(name), nlohmann::json::array()) != entries) {
        doc["post_link_key"] = std::string(key);
        doc[std::string(name)] = entries;
        write_cache(plan.outputDir, doc);
    }

    // THE PUBLISHED COPY IS REWRITTEN UNCONDITIONALLY, and the store above is
    // not. They have opposite lifetimes: the sidecar survives `prepare_build`,
    // which is the whole point, while `resolution.json` was regenerated from a
    // fresh object at the start of this very invocation and therefore carries
    // nothing yet. Skipping the publish when the CONTENT had not changed left
    // `runtime.symbol_provision` absent on every warm build — the record was
    // correct and the documented place to read it was empty, which is the
    // failure this whole change exists to remove, moved one file over.
    const auto path = plan.outputDir / "resolution.json";
    nlohmann::json resolution;
    {
        std::ifstream input(path);
        resolution = nlohmann::json::parse(input, nullptr, false);
    }
    if (resolution.is_discarded() || !resolution.is_object()) return;
    auto runtime = resolution.find("runtime");
    if (runtime == resolution.end() || !runtime->is_object()) return;
    (*runtime)[std::string(name)] = entries;

    std::error_code ec;
    auto tmp = path;
    tmp += ".tmp";
    if (std::ofstream output(tmp); output) {
        output << resolution.dump(2) << '\n';
        output.close();
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
        }
    }
}

std::vector<std::filesystem::path>
runtime_search_dirs(const mcpp::build::BuildPlan& plan) {
    std::vector<std::filesystem::path> out;
    auto append = [&](auto const& dirs) {
        for (auto const& dir : dirs) {
            if (dir.empty() || std::ranges::find(out, dir) != out.end()) continue;
            out.push_back(dir);
        }
    };
    append(plan.runtimeLibraryDirs);
    append(plan.depRuntimeLibraryDirs);
    append(plan.toolchain.compilerRuntimeDirs);
    append(plan.runtimeBinding.libraryDirs);
    // The SubOS farm comes from the PLAN's closure, not straight from the
    // binding — because the plan is where the guards live. A cross target gets
    // no farm entry in its DT_RPATH, so a model that consulted the binding
    // directly would resolve an aarch64 DT_NEEDED out of this host's x86_64
    // farm and report a pass the target machine will not honour. The model has
    // to look exactly where the artifact looks.
    for (auto const& dir : plan.runtimeSearch) {
        if (dir.origin != mcpp::platform::search::Origin::SubosFarm) continue;
        if (dir.path.empty() || std::ranges::find(out, dir.path) != out.end()) continue;
        out.push_back(dir.path);
    }
    return out;
}

std::optional<ValidatedArtifact> cached_artifact(
    const nlohmann::json& doc,
    std::string_view key,
    std::string_view expectedFingerprint,
    const std::filesystem::path& path) {
    try {
        auto artifacts = doc.find("artifacts");
        if (artifacts == doc.end() || !artifacts->is_object()) return std::nullopt;
        auto it = artifacts->find(std::string(key));
        if (it == artifacts->end() || !it->is_object()
            || it->value("fingerprint", "") != expectedFingerprint)
            return std::nullopt;
        ValidatedArtifact out;
        out.artifact = path;
        out.cacheHit = true;
        out.verdict.status = parse_status(it->value("status", "inconclusive"));
        out.verdict.diagnostics = it->value(
            "diagnostics", std::vector<std::string>{});
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

void store_artifact(nlohmann::json& doc,
                    std::string_view key,
                    std::string_view artifactFingerprint,
                    const ValidatedArtifact& value) {
    if (!doc.contains("artifacts") || !doc["artifacts"].is_object())
        doc["artifacts"] = nlohmann::json::object();
    doc["artifacts"][std::string(key)] = {
        {"fingerprint", artifactFingerprint},
        {"status", status_name(value.verdict.status)},
        {"diagnostics", value.verdict.diagnostics},
    };
}

void sync_resolution_verdict(const mcpp::build::BuildPlan& plan,
                             const nlohmann::json& cache) {
    const auto path = plan.outputDir / "resolution.json";
    std::ifstream input(path);
    auto resolution = nlohmann::json::parse(input, nullptr, false);
    if (resolution.is_discarded() || !resolution.is_object()) return;
    auto runtime = resolution.find("runtime");
    if (runtime == resolution.end() || !runtime->is_object()) return;

    nlohmann::json checked = nlohmann::json::array();
    using Status = mcpp::platform::elf::RuntimeVerdict::Status;
    Status summary = Status::Pass;
    bool any = false;
    if (auto artifacts = cache.find("artifacts");
        artifacts != cache.end() && artifacts->is_object()) {
        for (auto it = artifacts->begin(); it != artifacts->end(); ++it) {
            if (!it.value().is_object()) continue;
            any = true;
            auto status = parse_status(it.value().value("status", "inconclusive"));
            // Worst wins, by an explicit severity order rather than a chain of
            // pairwise comparisons that has to be re-derived every time a
            // state is added.
            if (status_severity(status) > status_severity(summary))
                summary = status;
            checked.push_back({
                {"path", (plan.outputDir / it.key()).lexically_normal().generic_string()},
                {"status", status_name(status)},
                {"diagnostics", it.value().value(
                    "diagnostics", std::vector<std::string>{})},
                {"fingerprint", it.value().value("fingerprint", "")},
            });
        }
    }
    const bool hasCheckableOutput = std::ranges::any_of(
        plan.linkUnits, [](auto const& unit) {
            return unit.kind != mcpp::build::LinkUnit::StaticLibrary;
        });
    (*runtime)["validation"] = {
        {"status", any ? status_name(summary)
                       : hasCheckableOutput ? "pending" : "not_exercised"},
        {"source", "post_link"},
        {"contract_hash", plan.runtimeBinding.contractHash},
        {"artifacts", std::move(checked)},
    };

    std::error_code ec;
    auto tmp = path;
    tmp += ".tmp";
    if (std::ofstream output(tmp); output) {
        output << resolution.dump(2) << '\n';
        output.close();
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
        }
    }
}

} // namespace

ArtifactSnapshot snapshot_link_artifacts(const mcpp::build::BuildPlan& plan) {
    ArtifactSnapshot out;
    for (auto const& unit : plan.linkUnits) {
        if (unit.kind == mcpp::build::LinkUnit::StaticLibrary) continue;
        auto artifact = plan.outputDir / unit.output;
        out.emplace(artifact, stamp(artifact));
    }
    return out;
}

ValidationReport validate_changed_artifacts(
    const mcpp::build::BuildPlan& plan,
    const ArtifactSnapshot& before) {
    ValidationReport report;
    if constexpr (!mcpp::platform::is_linux) return report;
    // Provider dispatch (see mcpp.runtime.binding): what follows is
    // ELF/glibc physics, and an identity from another provider — `ucrt@…` on
    // Windows — has no rules here rather than a missing glibc.
    if (plan.runtimeBinding.platform != "linux"
        || mcpp::platform::runtime::runtime_provider(
               plan.runtimeBinding.runtimeId) != "glibc")
        return report;

    auto doc = read_cache(plan.outputDir);
    bool changedCache = false;
    if (doc.value("schema", 0) != 1
        || doc.value("contract_hash", "") != plan.runtimeBinding.contractHash) {
        doc = nlohmann::json::object();
        changedCache = true;
    }
    doc["schema"] = 1;
    doc["contract_hash"] = plan.runtimeBinding.contractHash;
    auto searchDirs = runtime_search_dirs(plan);
    // AN ARTIFACT LEAVES THIS RECORD WHEN IT LEAVES THE DISK, NOT WHEN IT
    // LEAVES THE CURRENT COMMAND'S PLAN.
    //
    // Pruning against the current plan is correct only if one plan owns the
    // output directory, and none does: `mcpp build` and `mcpp test` share it
    // and have different link-unit sets — measured, the test plan carries the
    // test binaries and not the package's own `bin/`. Each command therefore
    // deleted the other's verdicts, and an edit-test loop paid the full ELF
    // re-parse on every alternation (1.19 s of a 3.15 s `mcpp test` on a
    // ten-unit tree, with nothing to rebuild).
    //
    // Disk existence keeps the file bounded — which is what the pruning is for
    // — without making the record a property of whichever command ran last.
    if (auto artifacts = doc.find("artifacts");
        artifacts != doc.end() && artifacts->is_object()) {
        for (auto it = artifacts->begin(); it != artifacts->end();) {
            std::error_code existsEc;
            if (!std::filesystem::exists(plan.outputDir / it.key(), existsEc)) {
                it = artifacts->erase(it);
                changedCache = true;
            } else {
                ++it;
            }
        }
    }
    // A BINDING THAT CANNOT BE EVALUATED IS ONE FACT, NOT ONE PER ARTIFACT.
    //
    // On a brand-new MCPP_HOME the first build finds `binding.loader` and
    // `binding.libraryDirs` both empty (the second build has them; #417), and
    // rule B then reported that per artifact — two lines each, thirteen
    // artifacts, twenty-six lines of the same sentence on a user's very first
    // build. The root cause is a separate question and is NOT settled; this is
    // the half of the criterion that does not depend on it.
    //
    // Said once, before the loop, naming what is missing. Rule B still runs:
    // it has other inputs (PT_INTERP identity), and suppressing it entirely
    // would trade noise for a blind spot.
    const bool bindingUnevaluated =
        !plan.runtimeBinding.loader.has_value() && plan.runtimeBinding.libraryDirs.empty();
    if (bindingUnevaluated && !before.empty()) {
        mcpp::ui::warning(std::format(
            "runtime binding {} has no loader path or library directory yet, so "
            "rule B cannot decide for this build's artifacts. This is expected on "
            "the first build in a fresh MCPP_HOME; a second build resolves it.",
            plan.runtimeBinding.runtimeId.empty() ? "<unnamed>"
                                                  : plan.runtimeBinding.runtimeId));
    }

    for (auto const& [artifact, oldStamp] : before) {
        auto now = stamp(artifact);
        if (!now.exists) continue;

        auto key = cache_key(plan, artifact);
        auto fp = fingerprint(artifact, now, plan.runtimeBinding.contractHash);
        if (auto cached = cached_artifact(doc, key, fp, artifact)) {
            // Same stat before/after and a current stored verdict is the hot
            // no-op: do not parse. Only PASS may also stay silent; a stored
            // mismatch must keep failing and an inconclusive result must keep
            // explaining itself on every invocation.
            if (!(now == oldStamp)
                || cached->verdict.status
                    != mcpp::platform::elf::RuntimeVerdict::Status::Pass)
                report.artifacts.push_back(std::move(*cached));
            continue;
        }

        ValidatedArtifact validated;
        validated.artifact = artifact;
        auto resolution = mcpp::platform::elf::resolve_runtime_closure(
            artifact, plan.runtimeBinding, searchDirs);
        // The SAME opt-out the link-time hermeticity check honours, read the
        // same way (manifest key or environment). A build that declared it is
        // reaching outside the sandbox on purpose has taken responsibility for
        // run-time resolution, so mcpp reports rather than blocks.
        validated.verdict = mcpp::platform::elf::validate_runtime_artifact(
            artifact, plan.runtimeBinding, resolution, host_libs_allowed(plan));
        store_artifact(doc, key, fp, validated);
        changedCache = true;
        report.artifacts.push_back(std::move(validated));
    }
    if (changedCache) write_cache(plan.outputDir, doc);
    sync_resolution_verdict(plan, doc);
    return report;
}

std::optional<ArtifactSnapshot> validated_artifact_snapshot(
    const std::filesystem::path& outputDir,
    const mcpp::platform::runtime::RuntimeBinding& binding) {
    auto doc = read_cache(outputDir);
    if (doc.value("schema", 0) != 1
        || doc.value("contract_hash", "") != binding.contractHash)
        return std::nullopt;
    auto artifacts = doc.find("artifacts");
    if (artifacts == doc.end() || !artifacts->is_object() || artifacts->empty())
        return std::nullopt;

    ArtifactSnapshot out;
    for (auto it = artifacts->begin(); it != artifacts->end(); ++it) {
        if (!it.value().is_object()) return std::nullopt;
        if (parse_status(it.value().value("status", "inconclusive"))
            != mcpp::platform::elf::RuntimeVerdict::Status::Pass)
            return std::nullopt;
        auto artifact = outputDir / it.key();
        auto current = stamp(artifact);
        if (!current.exists) return std::nullopt;
        auto expected = fingerprint(artifact, current, binding.contractHash);
        if (it.value().value("fingerprint", "") != expected)
            return std::nullopt;
        out.emplace(std::move(artifact), current);
    }
    return out;
}

bool artifact_snapshot_unchanged(const ArtifactSnapshot& snapshot) {
    return std::ranges::all_of(snapshot, [](auto const& entry) {
        return stamp(entry.first) == entry.second;
    });
}

std::string_view to_string(ArtifactVerdict verdict) {
    switch (verdict) {
        case ArtifactVerdict::Ok:         return "ok";
        case ArtifactVerdict::Mismatch:   return "mismatch";
        case ArtifactVerdict::Missing:    return "missing";
        case ArtifactVerdict::Unverified: return "unverified";
    }
    return "unverified";
}

ArtifactVerdict artifact_identity_verdict(
    const mcpp::manifest::RuntimeArtifact& artifact) {
    if (artifact.path.empty()) return ArtifactVerdict::Missing;

    std::error_code ec;
    if (!std::filesystem::exists(artifact.path, ec) || ec)
        return ArtifactVerdict::Missing;

    // The version the provenance CLAIMS. `<ns>:<name>@<version>` is the
    // ecosystem's address form; without a version there is nothing to check
    // against and the honest answer is Unverified.
    auto at = artifact.provenance.rfind('@');
    if (at == std::string::npos || at + 1 >= artifact.provenance.size())
        return ArtifactVerdict::Unverified;
    auto version = artifact.provenance.substr(at + 1);
    if (version.empty()) return ArtifactVerdict::Unverified;

    // FOLLOW THE SYMLINKS. The declaration is a promise about which payload
    // the loader will reach, and a payload directory is normally reached
    // through a symlink that some later install can silently repoint. Reading
    // the declared path alone would confirm the promise against itself.
    auto real = std::filesystem::weakly_canonical(artifact.path, ec);
    if (ec) real = artifact.path;

    // A path COMPONENT, not a substring: `0.1.1` must not satisfy `0.1.11`,
    // and a version appearing inside a file name is not the store directory
    // this is about.
    for (auto const& part : real) {
        if (part.string() == version) return ArtifactVerdict::Ok;
    }
    return ArtifactVerdict::Mismatch;
}

std::vector<mcpp::build::loader::TagFinding>
check_and_record_loader_tags(const mcpp::build::BuildPlan& plan,
                             const ArtifactSnapshot& before) {
    namespace loader = mcpp::build::loader;
    std::vector<loader::TagFinding> findings;
    if constexpr (!mcpp::platform::is_linux) return findings;

    const auto key = post_link_key(plan);
    const auto recorded =
        stored_post_link(read_cache(plan.outputDir), kLoaderTagsRecord, key);
    auto recorded_entry = [&](const std::string& rel) -> const nlohmann::json* {
        for (auto const& e : recorded)
            if (e.is_object() && e.value("path", "") == rel) return &e;
        return nullptr;
    };
    auto required_from = [](std::string_view s) {
        if (s == "DT_RPATH")   return loader::RequiredTag::Rpath;
        if (s == "DT_RUNPATH") return loader::RequiredTag::Runpath;
        return loader::RequiredTag::NotApplicable;
    };
    auto actual_from = [](std::string_view s) {
        using Tag = mcpp::platform::elf::SearchPathTag;
        if (s == "DT_RPATH")             return Tag::Rpath;
        if (s == "DT_RUNPATH")           return Tag::Runpath;
        if (s == "DT_RPATH+DT_RUNPATH")  return Tag::Both;
        return Tag::None;
    };

    for (auto const& [artifact, oldStamp] : before) {
        auto now = stamp(artifact);
        if (!now.exists) continue;

        std::error_code ec;
        auto rel = std::filesystem::relative(artifact, plan.outputDir, ec);
        auto relStr = (ec ? artifact : rel).lexically_normal().generic_string();

        if (now == oldStamp) {
            if (auto const* prev = recorded_entry(relStr)) {
                loader::TagFinding f;
                f.artifact = artifact;
                f.form = prev->value("form", "") == "executable"
                             ? loader::Form::Executable : loader::Form::SharedLibrary;
                f.required = required_from(prev->value("required", ""));
                f.actual   = actual_from(prev->value("actual", ""));
                auto st = prev->value("status", "");
                f.status = st == "ok"        ? loader::TagFinding::Status::Ok
                         : st == "violation" ? loader::TagFinding::Status::Violation
                                             : loader::TagFinding::Status::NotChecked;
                findings.push_back(std::move(f));
                continue;
            }
            // No stored verdict for an unchanged artifact: fall through and
            // read it, or the first build after this cache shape changed would
            // report "not checked" forever.
        }

        auto finding = loader::check_artifact(artifact);
        if (finding.form == loader::Form::NotElf) continue;
        findings.push_back(std::move(finding));
    }
    if (findings.empty()) return findings;

    nlohmann::json entries = nlohmann::json::array();
    for (auto const& finding : findings) {
        std::error_code ec;
        auto relative = std::filesystem::relative(
            finding.artifact, plan.outputDir, ec);
        entries.push_back({
            {"path", (ec ? finding.artifact : relative)
                         .lexically_normal().generic_string()},
            {"form", finding.form == loader::Form::Executable
                         ? "executable" : "shared_library"},
            {"required", loader::to_string(finding.required)},
            {"actual", std::string(
                mcpp::platform::elf::to_string(finding.actual))},
            {"status", finding.status == loader::TagFinding::Status::Ok
                           ? "ok"
                     : finding.status == loader::TagFinding::Status::Violation
                           ? "violation" : "not_checked"},
        });
    }
    auto merged = merge_post_link(recorded, std::move(entries), plan.outputDir);
    // The union is what gets stored, and the store is only rewritten when it
    // moved -- but the PUBLISHED copy in resolution.json is rewritten every
    // drive, because `prepare_build` regenerated that file from an empty object
    // at the start of this invocation. `anyFresh` was the wrong condition on
    // both counts: a drive that read every verdict back but contributed a link
    // unit the record had never seen would leave it out, and an absent entry
    // reads exactly like "checked and clean".
    persist_post_link(plan, kLoaderTagsRecord, key, merged);
    return findings;
}

std::vector<SymbolProvisionFinding>
check_symbol_provision(const mcpp::build::BuildPlan& plan,
                       const ArtifactSnapshot& before) {
    namespace sp = mcpp::build::symbol_provision;
    std::vector<SymbolProvisionFinding> findings;
    if constexpr (!mcpp::platform::is_linux) return findings;

    // The flags this build hands the linker, as ONE vector, because the
    // question is whether ANY of them took the export decision away from
    // mcpp. Per-unit flags join below; these are the whole-build ones.
    std::vector<std::string> globalFlags = plan.manifest.buildConfig.ldflags;

    auto searchDirs = runtime_search_dirs(plan);

    // WHAT AN UNCHANGED ARTIFACT KEEPS.
    //
    // Re-parsing every image on every drive is what made the loader-tag check
    // cost 158.7s of a 190s hot run, so an artifact whose stat did not move is
    // skipped here too. But skipping it must not DROP its verdict: `mcpp test`
    // drives the backend once per test on an already-built tree, and a
    // workspace relinks one member at a time. Without this read-back the
    // record would shrink to "whatever moved last", a conflict found on
    // Monday would stop being reported on Tuesday, and — worse — the absence
    // of an entry would read exactly like "checked and clean".
    const auto key = post_link_key(plan);
    const auto recorded =
        stored_post_link(read_cache(plan.outputDir), kSymbolProvisionRecord, key);
    auto stored_for = [&](const std::string& rel) -> const nlohmann::json* {
        for (auto const& entry : recorded)
            if (entry.is_object() && entry.value("path", "") == rel) return &entry;
        return nullptr;
    };

    // Symbol tables of closure objects, parsed at most once per file. Several
    // images in one build share almost their whole closure.
    std::map<std::filesystem::path, std::vector<std::string>> closureCache;
    auto defines_of = [&](const std::filesystem::path& object)
        -> const std::vector<std::string>& {
        auto it = closureCache.find(object);
        if (it != closureCache.end()) return it->second;
        std::vector<std::string> names;
        if (auto symbols = mcpp::platform::elf::inspect_dynamic_symbols(object)) {
            names.reserve(symbols->defined.size());
            for (auto const& symbol : symbols->defined) names.push_back(symbol.name);
            std::ranges::sort(names);
        }
        return closureCache.emplace(object, std::move(names)).first->second;
    };

    for (auto const& [artifact, oldStamp] : before) {
        auto now = stamp(artifact);
        if (!now.exists) continue;

        std::error_code relEc;
        auto rel = std::filesystem::relative(artifact, plan.outputDir, relEc);
        auto relStr = (relEc ? artifact : rel).lexically_normal().generic_string();

        // Only images this run actually produced get re-read. An unchanged one
        // keeps the verdict already on file (see the note above); with none on
        // file it falls through and is read, so the first build after this
        // record appeared does not report "never checked" forever.
        if (now == oldStamp) {
            if (auto const* prev = stored_for(relStr)) {
                sp::Report kept;
                auto status = prev->value("status", "");
                kept.status = status == "clean"          ? sp::Status::Clean
                            : status == "conflict"       ? sp::Status::Conflict
                            : status == "not-applicable" ? sp::Status::NotApplicable
                                                         : sp::Status::NotEvaluated;
                kept.exported = prev->value("exported", std::size_t{0});
                kept.total    = prev->value("dynamic_symbols", std::size_t{0});
                kept.reason   = prev->value("reason", "");
                if (auto c = prev->find("conflicts");
                    c != prev->end() && c->is_array()) {
                    for (auto const& entry : *c) {
                        sp::Conflict conflict;
                        conflict.name   = entry.value("symbol", "");
                        conflict.isFunc = entry.value("kind", "") == "func";
                        if (auto by = entry.find("also_provided_by");
                            by != entry.end() && by->is_array())
                            for (auto const& label : *by)
                                if (label.is_string())
                                    conflict.alsoProvidedBy.push_back(label);
                        kept.conflicts.push_back(std::move(conflict));
                    }
                }
                findings.push_back({artifact, std::move(kept)});
                continue;
            }
        }
        // Which link unit is this? Its own flags matter as much as the
        // global ones, and the static side of any report is attributed from
        // the objects it links. Spelled EXACTLY as `snapshot_link_artifacts`
        // spells it, so the two cannot disagree about which key names which
        // artifact — a mismatch here loses the unit's own flags silently.
        const mcpp::build::LinkUnit* unit = nullptr;
        for (auto const& lu : plan.linkUnits) {
            if (plan.outputDir / lu.output == artifact) { unit = &lu; break; }
        }

        auto facts = mcpp::platform::elf::inspect_elf_runtime(artifact);
        if (!facts) continue;                       // not ELF: no flat namespace
        // PT_INTERP, not the ELF type. A PIE executable is ET_DYN, exactly
        // like a shared library, and whether mcpp's toolchain emits PIE is the
        // payload compiler's default — mcpp passes neither -pie nor -no-pie.
        // Keying on ET_EXEC would make this check read "nothing to inspect"
        // the day that default flips, which is indistinguishable from "clean".
        // An interpreter is what makes an image a program the loader starts.
        if (facts->interp.empty()) continue;

        std::vector<std::string> flags = globalFlags;
        if (unit) flags.insert(flags.end(),
                               unit->linkFlags.begin(), unit->linkFlags.end());
        if (sp::export_dynamic_requested(flags)) {
            findings.push_back({artifact, sp::not_applicable(
                "the link requests exported dynamic symbols")});
            continue;
        }

        auto symbols = mcpp::platform::elf::inspect_dynamic_symbols(artifact);
        if (!symbols) {
            findings.push_back({artifact, sp::not_evaluated(symbols.error())});
            continue;
        }
        if (!symbols->present) {
            findings.push_back({artifact, sp::not_applicable(
                "statically linked: there is no dynamic symbol table")});
            continue;
        }
        auto exported = sp::exported_definitions(*symbols);
        if (!exported) {
            findings.push_back({artifact, sp::not_evaluated(
                "this machine's copy-relocation type is not known to mcpp")});
            continue;
        }

        sp::Report report;
        report.total = symbols->total;
        report.exported = exported->size();
        if (exported->empty()) {
            report.status = sp::Status::Clean;
            findings.push_back({artifact, std::move(report)});
            continue;
        }

        // Stage two. Only reached when the image exports something, which a
        // normal build does not.
        auto resolution = mcpp::platform::elf::resolve_runtime_closure(
            artifact, plan.runtimeBinding, searchDirs);
        std::vector<sp::Provider> providers;
        for (auto const& object : resolution.objects) {
            if (object.artifact == artifact) continue;
            auto const& names = defines_of(object.artifact);
            if (names.empty()) continue;
            providers.push_back(sp::Provider{
                .label = object.artifact.string(),
                .defines = names,
            });
        }
        report.conflicts = sp::conflicting_exports(*exported, providers);
        report.status = report.conflicts.empty() ? sp::Status::Clean
                                                 : sp::Status::Conflict;
        findings.push_back({artifact, std::move(report)});
    }

    // Record, for the same reason the loader-tag contract records: a warning
    // scrolls past, and the record is what CI, `mcpp why runtime` and a test
    // can read. It also gives a test a FIELD to assert on instead of a
    // substring of a message — a message whose wording is free to improve.
    // Stored in the sidecar and published into `resolution.json`; see the
    // module header for why those are two different files.
    if (!findings.empty()) {
        {
            {
                nlohmann::json entries = nlohmann::json::array();
                for (auto const& finding : findings) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(
                        finding.artifact, plan.outputDir, ec);
                    nlohmann::json entry{
                        {"path", (ec ? finding.artifact : rel)
                                     .lexically_normal().generic_string()},
                        {"status", std::string(
                             sp::to_string(finding.report.status))},
                        // Always both numbers. `exported: 0` is only evidence
                        // when `dynamic_symbols` is beside it.
                        {"exported", finding.report.exported},
                        {"dynamic_symbols", finding.report.total},
                    };
                    if (!finding.report.reason.empty())
                        entry["reason"] = finding.report.reason;
                    if (!finding.report.conflicts.empty()) {
                        nlohmann::json conflicts = nlohmann::json::array();
                        for (auto const& conflict : finding.report.conflicts)
                            conflicts.push_back({
                                {"symbol", conflict.name},
                                {"kind", conflict.isFunc ? "func" : "object"},
                                {"also_provided_by", conflict.alsoProvidedBy},
                            });
                        entry["conflicts"] = std::move(conflicts);
                    }
                    entries.push_back(std::move(entry));
                }
                auto merged = merge_post_link(recorded, std::move(entries),
                                              plan.outputDir);
                persist_post_link(plan, kSymbolProvisionRecord, key, merged);
            }
        }
    }
    return findings;
}

std::optional<StoredRuntimeSummary>
latest_stored_verdict(const std::filesystem::path& targetRoot) {
    std::error_code ec;
    if (!std::filesystem::is_directory(targetRoot, ec)) return std::nullopt;
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    bool found = false;
    for (auto it = std::filesystem::recursive_directory_iterator(
             targetRoot, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().filename() != kCacheFile) continue;
        auto time = it->last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (!found || time > newestTime) {
            found = true;
            newest = it->path();
            newestTime = time;
        }
    }
    if (!found) return std::nullopt;
    auto doc = read_cache(newest.parent_path());
    auto artifacts = doc.find("artifacts");
    if (artifacts == doc.end() || !artifacts->is_object()) return std::nullopt;

    StoredRuntimeSummary summary;
    summary.contractHash = doc.value("contract_hash", "");
    using Status = mcpp::platform::elf::RuntimeVerdict::Status;
    summary.verdict.status = Status::Pass;
    for (auto it = artifacts->begin(); it != artifacts->end(); ++it) {
        if (!it.value().is_object()) continue;
        auto status = parse_status(it.value().value("status", "inconclusive"));
        const bool worse = status == Status::ProvenMismatch
            || (status == Status::Inconclusive && summary.verdict.status == Status::Pass);
        if (!worse && !summary.artifact.empty()) continue;
        summary.artifact = newest.parent_path() / it.key();
        summary.verdict.status = status;
        summary.verdict.diagnostics = it.value().value(
            "diagnostics", std::vector<std::string>{});
        if (status == Status::ProvenMismatch) break;
    }
    if (summary.artifact.empty()) return std::nullopt;
    return summary;
}

} // namespace mcpp::build::runtime_validation
