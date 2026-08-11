// mcpp.build.runtime_validation — validate only freshly linked Linux ELFs.
//
// The backend snapshots link outputs before ninja and compares their stat
// fingerprints afterwards.  An unchanged no-op build therefore performs zero
// ELF parses.  Verdicts are persisted beside build.ninja and keyed by artifact
// stat + RuntimeBinding contract so doctor can explain the last result without
// probing the host again.

export module mcpp.build.runtime_validation;

import std;
import mcpp.build.loader_contract;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.libs.json;
import mcpp.platform;
import mcpp.platform.elf_runtime;
import mcpp.platform.runtime_binding;
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

    // Any artifact PROVEN bad — payloads mixed, or a DT_NEEDED that the
    // artifact's own loader will not find. Asks the verdict rather than
    // enumerating states here, so a fifth state cannot be added without this
    // gate deciding what it means.
    bool has_blocking_failure() const {
        return std::ranges::any_of(artifacts, [](auto const& artifact) {
            return artifact.verdict.blocking();
        });
    }
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
std::vector<mcpp::build::loader::TagFinding>
check_and_record_loader_tags(const mcpp::build::BuildPlan& plan,
                             const ArtifactSnapshot& produced);

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
    if (plan.runtimeBinding.platform != "linux"
        || !plan.runtimeBinding.runtimeId.starts_with("glibc@"))
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
    std::set<std::string> currentKeys;
    for (auto const& [artifact, ignored] : before) {
        (void)ignored;
        currentKeys.insert(cache_key(plan, artifact));
    }
    if (auto artifacts = doc.find("artifacts");
        artifacts != doc.end() && artifacts->is_object()) {
        for (auto it = artifacts->begin(); it != artifacts->end();) {
            if (!currentKeys.contains(it.key())) {
                it = artifacts->erase(it);
                changedCache = true;
            } else {
                ++it;
            }
        }
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
                             const ArtifactSnapshot& produced) {
    namespace loader = mcpp::build::loader;
    std::vector<loader::TagFinding> findings;
    if constexpr (!mcpp::platform::is_linux) return findings;

    for (auto const& [artifact, ignored] : produced) {
        (void)ignored;
        auto finding = loader::check_artifact(artifact);
        if (finding.form == loader::Form::NotElf) continue;
        findings.push_back(std::move(finding));
    }
    if (findings.empty()) return findings;

    const auto path = plan.outputDir / "resolution.json";
    std::ifstream input(path);
    auto resolution = nlohmann::json::parse(input, nullptr, false);
    if (resolution.is_discarded() || !resolution.is_object()) return findings;
    auto runtime = resolution.find("runtime");
    if (runtime == resolution.end() || !runtime->is_object()) return findings;

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
    (*runtime)["loader_tags"] = std::move(entries);

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
