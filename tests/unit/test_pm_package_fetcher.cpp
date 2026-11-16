
import std;
import mcpplibs.mtest;
import mcpplibs.os.stdio;
import mcpp.pm.package_fetcher;
import mcpp.pm.index_contract;

using namespace mcpplibs::mtest;

// Regression for the compat.zlib vs upstream bare zlib.lua collision.
//
// read_xpkg_lua_from_project_data scans every index dir under the project's
// xlings data roots. Before the identity gate, it returned the FIRST file whose
// candidate filename existed — so a foreign `xim-pkgindex/pkgs/z/zlib.lua`
// (declares `name = "zlib"`, no namespace, no mcpp block) could satisfy a
// request for `compat.zlib` whenever it was scanned before the real
// `mcpplibs/pkgs/c/compat.zlib.lua`. Resolution must now key on the descriptor's
// DECLARED identity, independent of directory order.

namespace {

std::filesystem::path make_tempdir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("{}-{}", name,
                            std::chrono::steady_clock::now()
                                .time_since_epoch().count());
    std::filesystem::create_directories(base);
    return base;
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

constexpr std::string_view kCompatZlibLua = R"(
package = {
    namespace = "compat",
    name      = "compat.zlib",
    version   = "1.3.2",
    mcpp = { sources = { "*.c" } },
}
)";

constexpr std::string_view kUpstreamZlibLua = R"(
package = {
    name = "zlib",
    description = "upstream zlib, no mcpp block",
}
)";

}  // namespace

UTest resolves_compat_zlib_not_foreign_bare_zlib{"PmPackageFetcher/ResolvesCompatZlibNotForeignBareZlib", [] {
    auto project = make_tempdir("mcpp-fetcher-collision");
    auto dataRoot = project / ".mcpp" / "data";

    // Foreign index sorts BEFORE the owning index, so a naive first-hit scan
    // would return its bare zlib.lua.
    write_file(dataRoot / "aaa-foreign-index" / "pkgs" / "z" / "zlib.lua",
               kUpstreamZlibLua);
    write_file(dataRoot / "mcpplibs" / "pkgs" / "c" / "compat.zlib.lua",
               kCompatZlibLua);

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "compat", "zlib");

    expect(fatal(lua.has_value()));
    expect(ne(lua->find("compat.zlib"), std::string::npos))
        << "should return the compat-namespaced descriptor";
    expect(ne(lua->find("mcpp"), std::string::npos))
        << "the resolved descriptor must carry the mcpp block";

    std::filesystem::remove_all(project);
}};

UTest foreign_bare_zlib_alone_does_not_satisfy_compat_request{"PmPackageFetcher/ForeignBareZlibAloneDoesNotSatisfyCompatRequest", [] {
    auto project = make_tempdir("mcpp-fetcher-nomatch");
    auto dataRoot = project / ".mcpp" / "data";

    // Only the foreign bare zlib exists; a compat.zlib request must find nothing
    // rather than wrongly accept the unrelated package.
    write_file(dataRoot / "xim-pkgindex" / "pkgs" / "z" / "zlib.lua",
               kUpstreamZlibLua);

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "compat", "zlib");

    expect(!lua.has_value());

    std::filesystem::remove_all(project);
}};

UTest default_namespace_cannot_resolve_compat_descriptor{"PmPackageFetcher/DefaultNamespaceCannotResolveCompatDescriptor", [] {
    auto project = make_tempdir("mcpp-fetcher-compat-alias");
    auto dataRoot = project / ".mcpp" / "data";

    constexpr std::string_view compatGtest = R"(
package = {
    namespace = "compat",
    name      = "compat.gtest",
    version   = "1.15.2",
    mcpp = { sources = { "*.cc" } },
}
)";
    write_file(dataRoot / "mcpplibs" / "pkgs" / "c" / "compat.gtest.lua",
               compatGtest);

    auto defaultLua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "mcpplibs", "gtest");
    expect(!defaultLua.has_value());

    auto explicitLua = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "compat", "gtest");
    expect(fatal(explicitLua.has_value()));
    expect(ne(explicitLua->find("compat.gtest"), std::string::npos));

    std::filesystem::remove_all(project);
}};

UTest local_path_index_attributes_own_namespace_to_no_ns_descriptor{"PmPackageFetcher/LocalPathIndexAttributesOwnNamespaceToNoNsDescriptor", [] {
    // e2e 49/51: a `[indices]` path index `local-dev` whose descriptor declares
    // only name="tinycfg" (no namespace field). The namespace is owned by the
    // index, so a `(local-dev, tinycfg)` request must match via index-owned
    // namespace attribution.
    auto index = make_tempdir("mcpp-local-path-index");
    write_file(index / "pkgs" / "t" / "tinycfg.lua",
               R"(package = { name = "tinycfg", version = "1.0.0", mcpp = { sources = {"*.c"} } })");

    auto lua = mcpp::pm::Fetcher::read_xpkg_lua_from_path(index, "local-dev", "tinycfg");
    expect(fatal(lua.has_value())) << "no-namespace descriptor must inherit the index namespace";
    expect(ne(lua->find("tinycfg"), std::string::npos));

    // A short name the index does not contain still resolves to nothing.
    auto miss = mcpp::pm::Fetcher::read_xpkg_lua_from_path(index, "local-dev", "absent");
    expect(!miss.has_value());

    std::filesystem::remove_all(index);
}};

// Coverage gap closed: a custom-namespace descriptor filed under a NON-canonical
// filename must still resolve for a qualified request, because identity is the
// declared (namespace, name) — never the filename (design doc
// 2026-06-26-identity-first-resolution-no-filename.md, P0/P2).
//
// Real-world trigger: `aimol.tensorvia-cpu` declares namespace="aimol",
// name="tensorvia-cpu" but is filed in the mcpplibs index as the BARE
// `pkgs/t/tensorvia-cpu.lua` (canonical would be `pkgs/a/aimol.tensorvia-cpu.lua`).
// Every prior fetcher fixture sat at its canonical path, so this seam was untested.
// `read_xpkg_lua*` already keys on declared identity, so this asserts the READ
// layer is correct and the production failure is isolated to candidate SELECTION
// (`selectDependencyCandidate`'s canonical-filename-only strict reader).
UTest resolves_custom_namespace_descriptor_under_non_canonical_filename{"PmPackageFetcher/ResolvesCustomNamespaceDescriptorUnderNonCanonicalFilename", [] {
    auto project = make_tempdir("mcpp-noncanonical-filename");
    auto dataRoot = project / ".mcpp" / "data";

    // Declared identity (aimol, tensorvia-cpu), but filed under the bare short
    // name in the mcpplibs index — filename does NOT encode the namespace.
    write_file(dataRoot / "mcpplibs" / "pkgs" / "t" / "tensorvia-cpu.lua",
               R"(package = {
                      namespace = "aimol",
                      name      = "tensorvia-cpu",
                      version   = "0.1.1",
                      mcpp = { sources = { "*.cppm" } },
                  })");

    // Qualified request for the custom namespace must resolve, filename be damned.
    auto hit = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "aimol", "tensorvia-cpu");
    expect(fatal(hit.has_value()))
        << "declared (aimol, tensorvia-cpu) must resolve regardless of filename";
    expect(ne(hit->find("tensorvia-cpu"), std::string::npos));

    // A foreign namespace for the same short name must NOT match it.
    auto wrongNs = mcpp::pm::Fetcher::read_xpkg_lua_from_project_data(
        project, "mcpplibs", "tensorvia-cpu");
    expect(!wrongNs.has_value())
        << "the descriptor is (aimol, …), so a (mcpplibs, …) request must miss";

    std::filesystem::remove_all(project);
}};

// ─── #238: install_packages failure diagnostics ─────────────────────
//
// The root cause of #238 is in xlings (its resolver silently exits 1 with ≥2
// project-level index_repos). mcpp cannot fix that; it can only make the
// failure diagnosable. These tests pin the message-formatting helper and the
// .xlings.json index_repos reader so the enriched, actionable error shape is
// locked without needing a live (failing) multi-repo xlings.

UTest install_failure_diagnostic_flags_multi_repo_gap{"PmPackageFetcher/InstallFailureDiagnosticFlagsMultiRepoGap", [] {
    std::vector<std::string> repos = {
        "mcpplibs -> https://example/mcpplibs",
        "myindex  -> https://example/myindex",
    };
    auto msg = mcpp::pm::format_install_failure_diagnostic(
        "acme.widget@1.2.3", 1, repos, /*childError=*/"");

    // Names the target + exit code.
    expect(ne(msg.find("acme.widget@1.2.3"), std::string::npos));
    expect(ne(msg.find("exit 1"), std::string::npos));
    // Enumerates the configured repos (count + identities).
    expect(ne(msg.find("2 index repos"), std::string::npos));
    expect(ne(msg.find("mcpplibs"), std::string::npos));
    expect(ne(msg.find("myindex"), std::string::npos));
    // Flags the known ≥2-repo xlings gap and links the tracking issue.
    expect(ne(msg.find("#238"), std::string::npos));
    expect(ne(msg.find("openxlings/xlings"), std::string::npos));
    // No child error → points at MCPP_VERBOSE for the raw output.
    expect(ne(msg.find("MCPP_VERBOSE"), std::string::npos));
}};

UTest install_failure_diagnostic_single_repo_no_gap_flag{"PmPackageFetcher/InstallFailureDiagnosticSingleRepoNoGapFlag", [] {
    std::vector<std::string> repos = { "mcpplibs" };
    auto msg = mcpp::pm::format_install_failure_diagnostic(
        "acme.widget@1.2.3", 1, repos, /*childError=*/"");

    expect(ne(msg.find("1 index repo"), std::string::npos));
    // A single repo is NOT the #238 failure mode; do not misattribute it.
    expect(eq(msg.find("known xlings resolution gap"), std::string::npos));
    expect(eq(msg.find("#238"), std::string::npos));
}};

UTest install_failure_diagnostic_preserves_child_error{"PmPackageFetcher/InstallFailureDiagnosticPreservesChildError", [] {
    std::vector<std::string> repos = { "a", "b" };
    auto msg = mcpp::pm::format_install_failure_diagnostic(
        "acme.widget@1.2.3", 1, repos,
        /*childError=*/"[error] descriptor not found in index");

    // Child-emitted text must be surfaced, not swallowed.
    expect(ne(msg.find("descriptor not found in index"), std::string::npos));
    expect(ne(msg.find("xlings reported"), std::string::npos));
    // With a real child error we don't fall back to the MCPP_VERBOSE nudge.
    expect(eq(msg.find("MCPP_VERBOSE"), std::string::npos));
}};

UTest read_seeded_index_repos_parses_xlings_json{"PmPackageFetcher/ReadSeededIndexReposParsesXlingsJson", [] {
    auto project = make_tempdir("mcpp-238-xlings-json");
    auto xjson = project / ".xlings.json";
    write_file(xjson, R"({
  "index_repos": [
    { "name": "mcpplibs", "url": "https://example/mcpplibs" },
    { "name": "myindex", "url": "https://example/myindex" }
  ],
  "lang": "en"
})");

    auto repos = mcpp::pm::read_seeded_index_repos(xjson);
    expect(fatal(eq(repos.size(), 2u)));
    expect(ne(repos[0].find("mcpplibs"), std::string::npos));
    expect(ne(repos[0].find("https://example/mcpplibs"), std::string::npos));
    expect(ne(repos[1].find("myindex"), std::string::npos));

    // Missing file → empty (caller degrades to "unknown configuration").
    auto none = mcpp::pm::read_seeded_index_repos(project / "does-not-exist.json");
    expect(none.empty());

    std::filesystem::remove_all(project);
}};

UTest read_seeded_index_repos_tolerates_artifact_fields{"PmPackageFetcher/ReadSeededIndexReposToleratesArtifactFields", [] {
    // #269: entries seeded with per-repo artifact/source keys (xlings >=
    // 0.4.68) must still yield name+url — regression lock for the hand-
    // rolled index_repos reader.
    auto project = make_tempdir("mcpp-269-artifact-fields");
    auto xjson = project / ".xlings.json";
    write_file(xjson, R"({
  "index_repos": [
    { "name": "mcpplibs", "url": "https://github.com/mcpplibs/mcpp-index.git", "artifact": "https://github.com/xlings-res/mcpp-index", "source": "auto" }
  ],
  "lang": "en"
})");

    auto repos = mcpp::pm::read_seeded_index_repos(xjson);
    expect(fatal(eq(repos.size(), 1u)));
    expect(ne(repos[0].find("mcpplibs"), std::string::npos));
    expect(ne(repos[0].find("https://github.com/mcpplibs/mcpp-index.git"), std::string::npos));

    std::filesystem::remove_all(project);
}};

// A tree whose index.toml requires a newer mcpp answers no lookup, and the
// read prints nothing. The fact is recorded for the message of a run that then
// fails (unusable_index_hint), which carries the E0006 text itself; a run that
// resolves every package elsewhere has no error to report. Before this, the
// read site printed the E0006 text as `error:` at the start of such runs.
UTest an_index_that_requires_a_newer_mcpp_is_recorded_not_printed{"PackageFetcher/AnIndexThatRequiresANewerMcppIsRecordedNotPrinted", [] {
    auto root = make_tempdir("mcpp-floor-read");
    write_file(root / "index.toml",
               "[index]\nspec = \"1\"\nmin_mcpp = \"9999.9.9.9\"\n");
    write_file(root / "pkgs" / "f" / "floorpkg.lua",
               "package = { spec = \"1\", name = \"floorpkg\", type = \"package\" }\n");

    auto stderrCapture = mcpplibs::os::stdio::capture::begin(mcpplibs::os::stdio::stream::error).value();
    auto first  = mcpp::pm::Fetcher::read_xpkg_lua_from_path(root, "floorns", "floorpkg");
    auto second = mcpp::pm::Fetcher::read_xpkg_lua_from_path(root, "floorns", "floorpkg");
    auto err = stderrCapture.text();

    expect(!first.has_value());
    expect(!second.has_value()) << "a tree recorded as unusable answers no later lookup";
    expect(err.empty()) << err;
    expect(bool(mcpp::pm::index_marked_unusable(root)));

    auto hint = mcpp::pm::unusable_index_hint();
    expect(ne(hint.find("E0006"), std::string::npos)) << hint;
    expect(ne(hint.find("9999.9.9.9"), std::string::npos)) << hint;
    expect(eq(hint.find("error above"), std::string::npos)) << hint;
    mcpp::pm::reset_unusable_indexes_for_test();
    std::filesystem::remove_all(root);
}};

// ─── What a caller's intent does with a payload (design 2026-10-17 D3) ───
//
// The user's home: llvm@23.1.3 recorded at packaging revision 0, the index at
// revision 1 with no floor -- complete, usable, not current. A build USES it;
// only `mcpp toolchain install` replaces it. 2026.10.16.x replaced it inside
// the build (and, with one dangling record in the home, failed every run).
UTest an_older_packaging_is_used_by_a_build_and_replaced_only_when_provisioned{"PmPayloadIntent/AnOlderPackagingIsUsedByABuildAndReplacedOnlyWhenProvisioned", [] {
    using mcpp::pm::PayloadAction;
    using mcpp::pm::PayloadIntent;
    using mcpp::pm::payload_action;
    // present, usable, current
    expect(eq(payload_action(PayloadIntent::Use,       true, true, false), PayloadAction::Take));
    expect(eq(payload_action(PayloadIntent::Locate,    true, true, false), PayloadAction::Take));
    expect(eq(payload_action(PayloadIntent::Provision, true, true, false), PayloadAction::Replace));
    expect(eq(payload_action(PayloadIntent::Provision, true, true, true), PayloadAction::Take));
}};

// Below the recipe's floor, or incomplete: not usable, so even a build
// replaces it -- the author said the old packaging must go.
UTest an_unusable_payload_is_replaced_by_every_intent_that_may_install{"PmPayloadIntent/AnUnusablePayloadIsReplacedByEveryIntentThatMayInstall", [] {
    using mcpp::pm::PayloadAction;
    using mcpp::pm::PayloadIntent;
    using mcpp::pm::payload_action;
    expect(eq(payload_action(PayloadIntent::Use,       true, false, false), PayloadAction::Replace));
    expect(eq(payload_action(PayloadIntent::Provision, true, false, false), PayloadAction::Replace));
    expect(eq(payload_action(PayloadIntent::Locate,    true, false, false), PayloadAction::Refuse));
}};

UTest nothing_there_is_installed_unless_only_located{"PmPayloadIntent/NothingThereIsInstalledUnlessOnlyLocated", [] {
    using mcpp::pm::PayloadAction;
    using mcpp::pm::PayloadIntent;
    using mcpp::pm::payload_action;
    expect(eq(payload_action(PayloadIntent::Use,       false, false, false), PayloadAction::Install));
    expect(eq(payload_action(PayloadIntent::Provision, false, false, false), PayloadAction::Install));
    expect(eq(payload_action(PayloadIntent::Locate,    false, false, false), PayloadAction::Refuse));
}};

// ─── What a failed install calls for (D5) ───
//
// The user's failure was `E_INTERNAL` (a metadata transaction refused): mcpp
// refreshed the index with the reason "not found locally (one-time)" for a
// descriptor that was local, retried the same doomed replacement through the
// command line, and told the user to retry. A deterministic refusal is said
// once; only a package the provider could not find refreshes the index.
UTest a_deterministic_refusal_is_neither_refreshed_nor_retried{"PmInstallFailurePolicy/ADeterministicRefusalIsNeitherRefreshedNorRetried", [] {
    for (std::string_view code : {"E_INTERNAL", "E_PERMISSION", "E_DISK_FULL", "E_CANCELLED"}) {
        const auto p = mcpp::pm::install_failure_policy(code);
        expect(!p.refreshIndex) << code;
        expect(!p.retry) << code;
    }
}};

UTest only_a_missing_package_refreshes_the_index{"PmInstallFailurePolicy/OnlyAMissingPackageRefreshesTheIndex", [] {
    expect(bool(mcpp::pm::install_failure_policy("E_NOT_FOUND").refreshIndex));
    expect(bool(mcpp::pm::install_failure_policy("E_NOT_FOUND").retry));
    expect(!(mcpp::pm::install_failure_policy("E_NETWORK").refreshIndex));
    expect(!(mcpp::pm::install_failure_policy("E_NETWORK").retry));
    expect(bool(mcpp::pm::install_failure_policy("E_INVALID_INPUT").retry));
    // A provider that said nothing (#238): the old conservative path.
    expect(bool(mcpp::pm::install_failure_policy("").refreshIndex));
    expect(bool(mcpp::pm::install_failure_policy("").retry));
}};
