
import std;
import mcpplibs.mtest;
import mcpplibs.os.platform;
import mcpp.provider.xlings;
import mcpp.invocation;

using namespace mcpplibs::mtest;

namespace {

std::filesystem::path make_tempdir(std::string_view name) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    return base;
}

}  // namespace

// ─── find_usable_nasm / find_sandbox_nasm (issue #232) ────────────────
//
// #232: nasm used to go through a bespoke `ensure_nasm` that could trigger
// a side-effecting install of its own. The fix splits this into a pure,
// side-effect-free PATH+sandbox probe (`find_usable_nasm` /
// `find_sandbox_nasm`, tested here) and a SEPARATE synchronous
// provisioning step in mcpp.build.prepare that goes through the same
// `Fetcher::resolve_xpkg_path` gate the compiler toolchain uses (that step
// needs `mcpp.config`/`mcpp.fetcher`, which this LEAF module — mcpp.xlings
// — cannot import, so it isn't unit-testable from here without network;
// see tests/e2e/105_asm_sources_nasm.sh for the end-to-end coverage).

#if !defined(_WIN32)

namespace {

// A fake `nasm` shell script reporting a fixed `-v` version string, laid
// out at <dir>/nasm so it can be resolved either via PATH `which()` or via
// the sandbox layout (<sandboxRoot>/<version>/nasm or .../<version>/bin/nasm).
std::filesystem::path make_fake_nasm(const std::filesystem::path& dir,
                                     std::string_view versionLine) {
    std::filesystem::create_directories(dir);
    auto nasm = dir / "nasm";
    std::ofstream os(nasm);
    // `#!/bin/sh` (a single exec) rather than `#!/usr/bin/env bash` (an
    // extra `env` -> `bash` hop) — one less fork/exec generation for the
    // nested `mcpp test` -> test-binary -> capture_exec chain to cross.
    os << "#!/bin/sh\n"
       << "if [ \"$1\" = \"-v\" ]; then\n"
       << "  echo \"" << versionLine << "\"\n"
       << "  exit 0\n"
       << "fi\n"
       << "exit 1\n";
    os.close();
    std::filesystem::permissions(
        nasm,
        std::filesystem::perms::owner_exec
        | std::filesystem::perms::owner_read
        | std::filesystem::perms::owner_write);
    return nasm;
}

// The current PATH, minus any directory that already has a real `nasm` on
// it. `find_usable_nasm`'s version check shells out (`env`/`bash` must
// still be resolvable), so tests must NOT collapse PATH down to a single
// directory — only remove the entries that would shadow the fake binary
// under test.
std::string path_without_real_nasm() {
    auto path = mcpp::invocation::get("PATH").value_or("");
    std::vector<std::string> kept;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find(':', start);
        auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && !std::filesystem::exists(std::filesystem::path(dir) / "nasm")) {
            kept.push_back(dir);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string out;
    for (auto& d : kept) {
        if (!out.empty()) out += ':';
        out += d;
    }
    return out;
}

}  // namespace

UTest finds_on_path_when_version_is_adequate{"FindUsableNasm/FindsOnPathWhenVersionIsAdequate", [] {
    auto pathDir = make_tempdir("mcpp-nasm-path");
    auto expected = make_fake_nasm(pathDir, "NASM version 2.16.03 compiled on Jan  1 2026");

    // Prepend (not replace) PATH: the fake binary must shadow any real
    // system nasm ahead of it, but `env`/`bash` must stay resolvable for
    // the fake script's own shebang + the version-check subprocess.
    auto base = path_without_real_nasm();
    mcpp::invocation::ScopedEnv path("PATH", pathDir.string() + ":" + base);

    auto home = make_tempdir("mcpp-nasm-home");  // no sandbox nasm present
    mcpp::provider::xlings::Env env{.home = home};

    auto found = mcpp::provider::xlings::find_usable_nasm(env);
    expect(fatal(found.has_value()));
    expect(eq(*found, expected));

    std::filesystem::remove_all(pathDir);
    std::filesystem::remove_all(home);
}};

UTest rejects_path_nasm_below_min_version_and_finds_no_sandbox_fallback{"FindUsableNasm/RejectsPathNasmBelowMinVersionAndFindsNoSandboxFallback", [] {
    auto pathDir = make_tempdir("mcpp-nasm-path-old");
    make_fake_nasm(pathDir, "NASM version 2.10.00 compiled on Jan  1 2020");

    auto base = path_without_real_nasm();
    mcpp::invocation::ScopedEnv path("PATH", pathDir.string() + ":" + base);

    auto home = make_tempdir("mcpp-nasm-home-empty");  // no sandbox nasm
    mcpp::provider::xlings::Env env{.home = home};

    expect(!(mcpp::provider::xlings::find_usable_nasm(env).has_value()));

    std::filesystem::remove_all(pathDir);
    std::filesystem::remove_all(home);
}};

UTest falls_back_to_sandbox_when_path_has_no_nasm{"FindUsableNasm/FallsBackToSandboxWhenPathHasNoNasm", [] {
    // PATH has the usual system directories (so the sandbox script's own
    // shebang still resolves) but none of them contain a real `nasm`.
    mcpp::invocation::ScopedEnv path("PATH", path_without_real_nasm());

    auto home = make_tempdir("mcpp-nasm-home-sandbox");
    auto sandboxDir = home / "data" / "xpkgs" / "xim-x-nasm" / "3.02";
    auto expected = make_fake_nasm(sandboxDir, "NASM version 3.02 compiled on Jan  1 2026");

    mcpp::provider::xlings::Env env{.home = home};

    auto found = mcpp::provider::xlings::find_usable_nasm(env);
    expect(fatal(found.has_value()));
    expect(eq(*found, expected));

    std::filesystem::remove_all(home);
}};

UTest finds_under_versioned_bin_subdir{"FindSandboxNasm/FindsUnderVersionedBinSubdir", [] {
    auto home = make_tempdir("mcpp-nasm-home-bin-subdir");
    auto sandboxDir = home / "data" / "xpkgs" / "xim-x-nasm" / "3.02" / "bin";
    auto expected = make_fake_nasm(sandboxDir, "NASM version 3.02 compiled on Jan  1 2026");

    mcpp::provider::xlings::Env env{.home = home};

    auto found = mcpp::provider::xlings::find_sandbox_nasm(env);
    expect(fatal(found.has_value()));
    expect(eq(*found, expected));

    std::filesystem::remove_all(home);
}};

UTest rejects_installed_nasm_below_min_version{"FindSandboxNasm/RejectsInstalledNasmBelowMinVersion", [] {
    auto home = make_tempdir("mcpp-nasm-home-old-sandboxed");
    auto sandboxDir = home / "data" / "xpkgs" / "xim-x-nasm" / "2.10";
    make_fake_nasm(sandboxDir, "NASM version 2.10.00 compiled on Jan  1 2020");

    mcpp::provider::xlings::Env env{.home = home};

    expect(!(mcpp::provider::xlings::find_sandbox_nasm(env).has_value()));

    std::filesystem::remove_all(home);
}};

UTest no_sandbox_directory_yields_nullopt{"FindSandboxNasm/NoSandboxDirectoryYieldsNullopt", [] {
    auto home = make_tempdir("mcpp-nasm-home-absent");
    mcpp::provider::xlings::Env env{.home = home};

    expect(!(mcpp::provider::xlings::find_sandbox_nasm(env).has_value()));

    std::filesystem::remove_all(home);
}};

#endif  // !defined(_WIN32)

UTest requires_default_mcpplibs_index{"XlingsIndexFreshness/RequiresDefaultMcpplibsIndex", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");

    mcpp::provider::xlings::Env env{.home = home};

    expect(!mcpp::provider::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}};

UTest accepts_fresh_default_mcpplibs_index{"XlingsIndexFreshness/AcceptsFreshDefaultMcpplibsIndex", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    std::ofstream(home / "data" / "mcpplibs" / ".mcpp-index-updated") << "ok\n";

    mcpp::provider::xlings::Env env{.home = home};

    expect(mcpp::provider::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}};

UTest requires_refresh_marker_for_default_mcpplibs_index{"XlingsIndexFreshness/RequiresRefreshMarkerForDefaultMcpplibsIndex", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");

    mcpp::provider::xlings::Env env{.home = home};

    expect(!mcpp::provider::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}};

UTest rejects_stale_refresh_marker{"XlingsIndexFreshness/RejectsStaleRefreshMarker", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    auto marker = home / "data" / "mcpplibs" / ".mcpp-index-updated";
    std::ofstream(marker) << "ok\n";
    std::filesystem::last_write_time(
        marker, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(7200));

    mcpp::provider::xlings::Env env{.home = home};

    expect(!mcpp::provider::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}};

UTest requires_official_xim_index_even_when_default_index_is_fresh{"XlingsIndexFreshness/RequiresOfficialXimIndexEvenWhenDefaultIndexIsFresh", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    std::ofstream(home / "data" / "mcpplibs" / ".mcpp-index-updated") << "ok\n";

    mcpp::provider::xlings::Env env{.home = home};

    expect(!mcpp::provider::xlings::is_official_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}};

UTest accepts_fresh_official_xim_index{"XlingsIndexFreshness/AcceptsFreshOfficialXimIndex", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";

    mcpp::provider::xlings::Env env{.home = home};

    expect(mcpp::provider::xlings::is_official_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}};

UTest requires_official_package_file_even_when_official_index_is_fresh{"XlingsIndexFreshness/RequiresOfficialPackageFileEvenWhenOfficialIndexIsFresh", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";

    mcpp::provider::xlings::Env env{.home = home};

    expect(!mcpp::provider::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}};

UTest accepts_fresh_official_package_file{"XlingsIndexFreshness/AcceptsFreshOfficialPackageFile", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs" / "m");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua") << "package = {}\n";

    mcpp::provider::xlings::Env env{.home = home};

    expect(mcpp::provider::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}};

UTest rejects_official_package_cache_with_foreign_path{"XlingsIndexFreshness/RejectsOfficialPackageCacheWithForeignPath", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    auto pkg = home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua";
    std::filesystem::create_directories(pkg.parent_path());
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(pkg) << "package = {}\n";
    std::ofstream(home / "data" / "xim-pkgindex" / ".xlings-index-cache.json")
        << R"({"entries":{"musl-gcc":{"path":"/tmp/foreign/xim-pkgindex/pkgs/m/musl-gcc.lua"}}})";

    mcpp::provider::xlings::Env env{.home = home};

    expect(!mcpp::provider::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}};

UTest accepts_official_package_cache_with_current_path{"XlingsIndexFreshness/AcceptsOfficialPackageCacheWithCurrentPath", [] {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    auto pkg = home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua";
    std::filesystem::create_directories(pkg.parent_path());
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(pkg) << "package = {}\n";
    std::ofstream(home / "data" / "xim-pkgindex" / ".xlings-index-cache.json")
        << std::format(R"({{"entries":{{"musl-gcc":{{"path":"{}"}}}}}})", pkg.string());

    mcpp::provider::xlings::Env env{.home = home};

    expect(mcpp::provider::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}};

// ─── Sibling/home payload discovery (issue #120) ─────────────────────
//
// A delegating index package (e.g. xim:linux-headers forwarding to
// scode:linux-headers) leaves a metadata-only husk dir under its own
// prefix (.xim-installed + .xpkg.lua, no payload). Discovery must not
// stop at the husk: the real payload lives under another prefix.

namespace {

void touch(const std::filesystem::path& p, std::string_view content = "x") {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

}  // namespace

UTest metadata_only_husk_is_not_content{"XlingsSiblingPackage/MetadataOnlyHuskIsNotContent", [] {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    // Only a husk exists: .xim-installed + .xpkg.lua, no payload.
    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    // Isolate from the host's ~/.xlings fallback.
    std::optional<std::filesystem::path> found;
    {
        mcpp::invocation::ScopedEnv home("HOME", tmp.string());
        found = mcpp::provider::xlings::paths::find_sibling_package(gccBin, "linux-headers");
    }

    expect(!found.has_value());

    std::filesystem::remove_all(tmp);
}};

UTest skips_husk_and_finds_payload_under_other_prefix{"XlingsSiblingPackage/SkipsHuskAndFindsPayloadUnderOtherPrefix", [] {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    auto found = mcpp::provider::xlings::paths::find_sibling_package(gccBin, "linux-headers");
    expect(fatal(found.has_value()));
    expect(eq(*found, real));

    std::filesystem::remove_all(tmp);
}};

UTest required_rel_path_rejects_contentful_but_wrong_candidate{"XlingsSiblingPackage/RequiredRelPathRejectsContentfulButWrongCandidate", [] {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    // Contentful but missing the payload that matters.
    auto stray = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(stray / "README.md");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    auto found = mcpp::provider::xlings::paths::find_sibling_package(
        gccBin, "linux-headers", "include/linux/limits.h");
    expect(fatal(found.has_value()));
    expect(eq(*found, real));

    std::filesystem::remove_all(tmp);
}};

UTest finds_payload_under_non_xim_prefix{"XlingsHomeTool/FindsPayloadUnderNonXimPrefix", [] {
    auto tmp = make_tempdir("mcpp-husk-home");
    auto xpkgs = tmp / "registry" / "data" / "xpkgs";

    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    std::optional<std::filesystem::path> found;
    {
        mcpp::invocation::ScopedEnv home("MCPP_HOME", tmp.string());
        found = mcpp::provider::xlings::paths::find_home_tool(
            "linux-headers", "include/linux/limits.h");
    }

    expect(fatal(found.has_value()));
    // The original home is normalized; macOS spells /var's target /private/var.
    expect(bool(std::filesystem::equivalent(*found, real)));

    std::filesystem::remove_all(tmp);
}};

// ─── seed_xlings_json — per-repo artifact/source emission (#269) ──────

namespace {

std::string read_seeded(const std::filesystem::path& home) {
    std::stringstream ss;
    ss << std::ifstream(home / ".xlings.json").rdbuf();
    return ss.str();
}

}  // namespace

UTest plain_repo_emission_is_byte_stable{"XlingsSeed/PlainRepoEmissionIsByteStable", [] {
    // Zero-diff gate: repos without artifact/source must serialize exactly
    // as before the SeedRepo extension.
    auto dir = make_tempdir("mcpp-seed-plain");
    mcpp::provider::xlings::Env env;
    env.home = dir;
    std::vector<mcpp::provider::xlings::SeedRepo> repos{
        { "mcpplibs", "https://x.git", "", "" } };
    mcpp::provider::xlings::seed_xlings_json(env, repos);
    auto text = read_seeded(dir);
    expect(ne(text.find(
        "    { \"name\": \"mcpplibs\", \"url\": \"https://x.git\" }\n"), std::string::npos)) << text;
    expect(eq(text.find("artifact"), std::string::npos));
    expect(eq(text.find("source"), std::string::npos));
    std::filesystem::remove_all(dir);
}};

UTest artifact_and_source_emitted_when_set{"XlingsSeed/ArtifactAndSourceEmittedWhenSet", [] {
    auto dir = make_tempdir("mcpp-seed-art");
    mcpp::provider::xlings::Env env;
    env.home = dir;
    std::vector<mcpp::provider::xlings::SeedRepo> repos{
        { "mcpplibs", "https://x.git",
          "https://github.com/xlings-res/mcpp-index", "auto" } };
    mcpp::provider::xlings::seed_xlings_json(env, repos);
    auto text = read_seeded(dir);
    expect(ne(text.find(
        "\"url\": \"https://x.git\", \"artifact\": "
        "\"https://github.com/xlings-res/mcpp-index\", \"source\": \"auto\""), std::string::npos)) << text;
    std::filesystem::remove_all(dir);
}};

// ─── Indices mcpp owns: their places (design 2026-10-16 §5) ──────────────

UTest seed_names_its_data_and_store{"XlingsOwnedIndex/SeedNamesItsDataAndStore", [] {
    auto dir = make_tempdir("mcpp-seed-owned");
    mcpp::provider::xlings::Env env;
    env.home = dir / "registry";
    mcpp::provider::xlings::SeedRepo repo{ "mcpplibs", "https://x.git", "", "" };
    repo.dir = dir / "indices" / "mcpplibs";
    repo.store = dir / "xpkgs";
    std::vector<mcpp::provider::xlings::SeedRepo> repos{ repo };
    mcpp::provider::xlings::seed_xlings_json(env, repos);
    const auto declared = mcpp::provider::xlings::paths::declared_indices(env);
    expect(fatal(eq(declared.size(), 1u)));
    expect(bool(declared[0].owned));
    expect(eq(declared[0].dir, (dir / "indices" / "mcpplibs").lexically_normal()));
    expect(eq(declared[0].store, (dir / "xpkgs").lexically_normal()));
    // Its store first, the home's after it (payloads from before, §5.3).
    const auto stores = mcpp::provider::xlings::paths::stores(env);
    expect(fatal(eq(stores.size(), 2u)));
    expect(eq(stores[0], (dir / "xpkgs").lexically_normal()));
    expect(eq(stores[1], env.home / "data" / "xpkgs"));
    std::filesystem::remove_all(dir);
}};

UTest a_home_serves_its_default_and_its_configured_indexes{"XlingsOwnedIndex/AHomeServesItsDefaultAndItsConfiguredIndexes", [] {
    auto dir = make_tempdir("mcpp-indices-of");
    expect(mcpp::provider::xlings::paths::indices_of(dir).empty());   // not a provider home
    std::ofstream(dir / ".xlings.json") << "{}";
    expect(eq(mcpp::provider::xlings::paths::indices_of(dir), std::vector<std::string>{"xim"}));
    std::ofstream(dir / ".xlings.json") << R"({"index_repos":[{"name":"mcpp-index","url":"u"},{"name":"xim","url":"v"}]})";
    expect(eq(mcpp::provider::xlings::paths::indices_of(dir), (std::vector<std::string>{"xim", "mcpp-index"})));
    std::filesystem::remove_all(dir);
}};

UTest a_home_from_before_has_its_index_moved_once{"XlingsOwnedIndex/AHomeFromBeforeHasItsIndexMovedOnce", [] {
    auto dir = make_tempdir("mcpp-own-indices");
    mcpp::provider::xlings::Env env;
    env.home = dir / "registry";
    std::vector<mcpp::provider::xlings::SeedRepo> plain{ { "mcpplibs", "https://x.git", "", "" } };
    mcpp::provider::xlings::seed_xlings_json(env, plain);
    const auto old = env.home / "data" / "mcpplibs";
    std::filesystem::create_directories(old / "pkgs" / "f");
    { std::ofstream(old / "pkgs" / "f" / "foo.lua") << "package = {}\n"; }
    std::filesystem::create_directories(env.home / "data" / "xim-pkgindex" / "pkgs");

    mcpp::provider::xlings::SeedRepo owned{ "mcpplibs", "https://x.git", "", "" };
    owned.dir = dir / "indices" / "mcpplibs";
    owned.store = dir / "xpkgs";
    std::vector<mcpp::provider::xlings::SeedRepo> repos{ owned };
    mcpp::provider::xlings::paths::own_indices(env, repos);
    expect(!std::filesystem::exists(old));
    expect(std::filesystem::exists(owned.dir / "pkgs" / "f" / "foo.lua"));
    const auto declared = mcpp::provider::xlings::paths::declared_indices(env);
    expect(fatal(eq(declared.size(), 1u)));
    expect(bool(declared[0].owned));
    // Read from both places: the provider's data dir and the owned one.
    const auto roots = mcpp::provider::xlings::paths::index_roots(env);
    expect(ne(std::ranges::find(roots, owned.dir.lexically_normal()), roots.end()));
    expect(ne(std::ranges::find(roots, env.home / "data" / "xim-pkgindex"), roots.end()));

    // Once: data that reappears at the old place is not moved over the new.
    std::filesystem::create_directories(old / "pkgs");
    mcpp::provider::xlings::paths::own_indices(env, repos);
    expect(std::filesystem::exists(old));
    expect(std::filesystem::exists(owned.dir / "pkgs" / "f" / "foo.lua"));
    // And it is not read as a second index.
    const auto again = mcpp::provider::xlings::paths::index_roots(env);
    expect(eq(std::ranges::find(again, old), again.end()));
    std::filesystem::remove_all(dir);
}};

UTest a_payload_is_found_in_its_index_store_then_in_the_homes{"XlingsOwnedIndex/APayloadIsFoundInItsIndexStoreThenInTheHomes", [] {
    auto dir = make_tempdir("mcpp-owned-payload");
    mcpp::provider::xlings::Env env;
    env.home = dir / "registry";
    mcpp::provider::xlings::SeedRepo owned{ "mcpplibs", "https://x.git", "", "" };
    owned.dir = dir / "indices" / "mcpplibs";
    owned.store = dir / "xpkgs";
    std::vector<mcpp::provider::xlings::SeedRepo> repos{ owned };
    mcpp::provider::xlings::seed_xlings_json(env, repos);
    // One installed before the index had a store of its own, one after.
    std::filesystem::create_directories(env.home / "data" / "xpkgs" / "compat-x-zlib" / "1.3.1" / "lib");
    std::filesystem::create_directories(dir / "xpkgs" / "mcpplibs-x-cmdline" / "0.0.1" / "src");
    using mcpp::provider::xlings::paths::xpkg_payload;
    auto zlib = xpkg_payload(env, { "compat", "zlib", "1.3.1" });
    expect(fatal(zlib.has_value()));
    expect(eq(*zlib, env.home / "data" / "xpkgs" / "compat-x-zlib" / "1.3.1"));
    auto cmdline = xpkg_payload(env, { "mcpplibs", "cmdline", "" });
    expect(fatal(cmdline.has_value()));
    expect(eq(*cmdline, (dir / "xpkgs").lexically_normal() / "mcpplibs-x-cmdline" / "0.0.1"));
    std::filesystem::remove_all(dir);
}};

// ─── Index identity + clock-skew hardening (#315) ────────────────────────

UTest future_marker_is_stale_not_eternally_fresh{"XlingsIndexFreshness/FutureMarkerIsStaleNotEternallyFresh", [] {
    auto home = make_tempdir("mcpp-xlings-index-future");
    auto dir  = home / "data" / "mcpplibs";
    std::filesystem::create_directories(dir / "pkgs");
    auto marker = dir / ".mcpp-index-updated";
    std::ofstream(marker) << "ok\n";
    // Clock skew in a container, a tar that preserved mtimes, a restored CI
    // cache: a marker in the future used to yield a negative age, which read as
    // "fresh" and stayed that way until the wall clock caught up.
    std::error_code ec;
    std::filesystem::last_write_time(
        marker, std::filesystem::file_time_type::clock::now() + std::chrono::hours{72}, ec);

    mcpp::provider::xlings::Env env{.home = home};
    auto st = mcpp::provider::xlings::default_index_status(env, 3600);

    expect(!st.fresh);
    expect(eq(st.ageSeconds, -1));   // unusable timestamp reports as unknown

    std::filesystem::remove_all(home);
}};

UTest reads_trimmed_value_and_degrades_to_nullopt{"XlingsIndexRevision/ReadsTrimmedValueAndDegradesToNullopt", [] {
    auto home = make_tempdir("mcpp-xlings-index-rev");
    auto dir  = home / "data" / "mcpplibs";
    std::filesystem::create_directories(dir / "pkgs");

    // Absent: a local `path` index has no such file — must not be an error.
    expect(!(mcpp::provider::xlings::index_revision(dir).has_value()));

    // Present. The observed files carry no trailing newline, but a writer that
    // added one (or a \r from Windows) must not change the value, or every run
    // would look like the index had changed.
    std::ofstream(dir / ".xlings-index-version") << "  8d67478\r\n";
    auto rev = mcpp::provider::xlings::index_revision(dir);
    expect(fatal(rev.has_value()));
    expect(eq(*rev, "8d67478"));

    // Blank is not an identity.
    std::ofstream(dir / ".xlings-index-version", std::ios::trunc) << "\n";
    expect(!(mcpp::provider::xlings::index_revision(dir).has_value()));

    // The value is OPAQUE: sub-indexes carry a date version, not a sha.
    std::ofstream(dir / ".xlings-index-version", std::ios::trunc) << "2026.7.30.1";
    expect(eq(mcpp::provider::xlings::index_revision(dir).value(), "2026.7.30.1"));

    std::filesystem::remove_all(home);
}};

UTest status_carries_the_revision{"XlingsIndexRevision/StatusCarriesTheRevision", [] {
    auto home = make_tempdir("mcpp-xlings-index-rev-status");
    auto dir  = home / "data" / "mcpplibs";
    std::filesystem::create_directories(dir / "pkgs");
    std::ofstream(dir / ".mcpp-index-updated") << "ok\n";
    std::ofstream(dir / ".xlings-index-version") << "abc1234";

    mcpp::provider::xlings::Env env{.home = home};
    auto st = mcpp::provider::xlings::default_index_status(env, 3600);

    expect(fatal(st.rev.has_value()));
    expect(eq(*st.rev, "abc1234"));

    std::filesystem::remove_all(home);
}};

// ─── stderr_error_tail (#614) ─────────────────────────────────────────
//
// xlings' own error lines follow mcpp's diagnostic when an install fails. The
// selection keeps the lines that name a rejection and bounds them, so a verbose
// child cannot bury the diagnostic they are attached to.

UTest keeps_only_error_level_lines{"XlingsStderrTail/KeepsOnlyErrorLevelLines", [] {
    auto tail = mcpp::provider::xlings::stderr_error_tail(
        "[xim] resolving acme.widget\r\n"
        "downloading 42%\n"
        "Error: checksum mismatch\n"
        "hint: E_NETWORK, retry later\n"
        "done\n");
    expect(eq(tail, (std::vector<std::string>{
        "[xim] resolving acme.widget",
        "Error: checksum mismatch",
        "hint: E_NETWORK, retry later"})));
}};

UTest keeps_the_last_lines_up_to_the_limit{"XlingsStderrTail/KeepsTheLastLinesUpToTheLimit", [] {
    std::string text;
    for (int i = 0; i < 30; ++i) text += std::format("error {}\n", i);
    auto tail = mcpp::provider::xlings::stderr_error_tail(text);
    expect(fatal(eq(tail.size(), 20u)));
    expect(eq(tail.front(), "error 10"));
    expect(eq(tail.back(), "error 29"));
    expect(eq(mcpp::provider::xlings::stderr_error_tail(text, 2), (std::vector<std::string>{"error 28", "error 29"})));
    expect(mcpp::provider::xlings::stderr_error_tail("").empty());
    expect(eq(mcpp::provider::xlings::stderr_error_tail("error without a newline"), (std::vector<std::string>{"error without a newline"})));
}};

// ─── invocation_env and the started xlings (#614) ─────────────────────
//
// One decision for the environment of an xlings invocation, rendered per
// platform. Global mode is an absent XLINGS_PROJECT_DIR on both platforms, and
// the process environment is the same after the invocation as before it.

namespace {

mcpp::provider::xlings::Env xlings_env(std::string_view projectDir) {
    mcpp::provider::xlings::Env env;
    env.home = std::filesystem::temp_directory_path() / "mcpp-xlings-home";
    env.binary = env.home / "bin" / "xlings";
    env.projectDir = std::filesystem::path(projectDir);
    return env;
}

// The decided entry for `key`; a default entry named "" when the decision omits
// the key.
mcpp::provider::xlings::InvocationVar decided(const mcpp::provider::xlings::Env& env, std::string_view key) {
    for (auto const& var : mcpp::provider::xlings::invocation_env(env))
        if (var.name == key) return var;
    return {};
}

}  // namespace

UTest global_mode_is_an_absent_project_directory{"XlingsInvocationEnv/GlobalModeIsAnAbsentProjectDirectory", [] {
    auto global = xlings_env("");
    auto scope = decided(global, "XLINGS_PROJECT_DIR");
    expect(fatal(eq(scope.name, "XLINGS_PROJECT_DIR")));
    expect(!scope.present);
    auto home = decided(global, "XLINGS_HOME");
    expect(fatal(eq(home.name, "XLINGS_HOME")));
    expect(bool(home.present));
    expect(eq(home.value, global.home.string()));

    auto project = xlings_env("proj-dir");
    auto projectScope = decided(project, "XLINGS_PROJECT_DIR");
    expect(bool(projectScope.present));
    expect(eq(projectScope.value, project.projectDir.string()));
}};

// What a started xlings receives, recorded by a stub in place of the binary:
// its working directory, then its environment.
struct Seen {
    std::string cwd;
    std::map<std::string, std::string> env;
};
Seen started_with(const mcpp::provider::xlings::Env& e) {
    std::filesystem::create_directories(e.binary.parent_path());
    const auto record = e.home / "seen";
    std::filesystem::remove(record);
    { std::ofstream(e.binary) << "#!/bin/sh\n{ pwd; env; } > \"" << record.string() << "\"\n"; }
    std::filesystem::permissions(e.binary, std::filesystem::perms::owner_all);
    (void)mcpp::provider::xlings::install_direct(e, "nothing", /*quiet=*/true);
    Seen seen;
    std::ifstream in(record);
    std::getline(in, seen.cwd);
    for (std::string line; std::getline(in, line);)
        if (auto eq = line.find('='); eq != std::string::npos)
            seen.env[line.substr(0, eq)] = line.substr(eq + 1);
    return seen;
}

// #614, #726: the decision reaches the started xlings, in its home, and this
// invocation's environment is the same afterwards. The sandbox's `bin` in
// front of PATH used to stay in the process on Windows, so every action of a
// build that had installed a payload found `xim:llvm`'s `cl`, `link` and `lib`
// shims in front of MSVC's tools, while a build that installed nothing did not.
UTest the_decision_reaches_the_started_xlings_only{"XlingsInvocationEnv/TheDecisionReachesTheStartedXlingsOnly", [] {
    if constexpr (mcpplibs::os::is_windows) skip("a POSIX stub records what xlings received");
    namespace env = mcpp::invocation;
    env::ScopedEnv home("XLINGS_HOME", std::string("shell-home"));
    env::ScopedEnv subos("XLINGS_ACTIVE_SUBOS", std::string("shell-subos"));
    env::ScopedEnv project("XLINGS_PROJECT_DIR", std::string("prior-project"));
    const auto pathBefore = env::get("PATH");

    auto global = xlings_env("");
    auto g = started_with(global);
    expect(eq(std::filesystem::path(g.cwd), std::filesystem::weakly_canonical(global.home)));
    expect(eq(g.env["XLINGS_HOME"], global.home.string()));
    expect(!g.env.contains("XLINGS_PROJECT_DIR"));
    expect(!g.env.contains("XLINGS_ACTIVE_SUBOS"));
    const auto bin = (global.home / "subos" / "default" / "bin").string();
    if (auto&& failed = expect(eq(g.env["PATH"].substr(0, bin.size()), bin)); !failed) failed << g.env["PATH"];

    auto inProject = xlings_env("proj-dir");
    auto p = started_with(inProject);
    expect(eq(p.env["XLINGS_PROJECT_DIR"], inProject.projectDir.string()));
    expect(!p.env.contains("XLINGS_ACTIVE_SUBOS"));

    expect(eq(env::get("PATH"), pathBefore));
    expect(eq(env::get("XLINGS_HOME"), std::optional<std::string>("shell-home")));
    expect(eq(env::get("XLINGS_ACTIVE_SUBOS"), std::optional<std::string>("shell-subos")));
    expect(eq(env::get("XLINGS_PROJECT_DIR"), std::optional<std::string>("prior-project")));
}};

// #726: on Windows too the registry's xlings starts in its home, never in the
// directory mcpp was started from, so a project's `.xlings.json` cannot put it
// in that project's mode.
UTest the_windows_prefix_starts_in_the_home{"XlingsInvocationEnv/TheWindowsPrefixStartsInTheHome", [] {
    mcpp::provider::xlings::Env e;
    e.home = "C:\\Users\\a b\\.mcpp\\registry";
    e.binary = "C:\\Users\\a b\\.mcpp\\registry\\bin\\xlings.exe";
    expect(eq(mcpp::provider::xlings::windows_command_prefix(e), "cd /d \"C:\\Users\\a b\\.mcpp\\registry\" && "
              "\"C:\\Users\\a b\\.mcpp\\registry\\bin\\xlings.exe\""));
}};

UTest the_posix_prefix_renders_the_decision{"XlingsInvocationEnv/ThePosixPrefixRendersTheDecision", [] {
    if constexpr (mcpplibs::os::is_windows) skip("the POSIX spelling");
    auto global = mcpp::provider::xlings::build_command_prefix(xlings_env(""));
    expect(ne(global.find("env -u XLINGS_PROJECT_DIR -u XLINGS_ACTIVE_SUBOS PATH="), std::string::npos)) << global;
    expect(eq(global.find("XLINGS_PROJECT_DIR="), std::string::npos)) << global;

    auto project = mcpp::provider::xlings::build_command_prefix(xlings_env("/work/proj"));
    expect(eq(project.find("-u XLINGS_PROJECT_DIR"), std::string::npos)) << project;
    expect(ne(project.find("env -u XLINGS_ACTIVE_SUBOS PATH="), std::string::npos)) << project;
    expect(ne(project.find("XLINGS_PROJECT_DIR="), std::string::npos)) << project;
    expect(ne(project.find("/work/proj"), std::string::npos)) << project;
}};

// A shell that ran `xlings subos use <name>` exports XLINGS_ACTIVE_SUBOS, which
// names a SubOS of the shell's xlings home and not of mcpp's registry; no
// invocation carries it, in either mode.
UTest the_shells_active_subos_is_never_inherited{"XlingsInvocationEnv/TheShellsActiveSubosIsNeverInherited", [] {
    auto global = decided(xlings_env(""), "XLINGS_ACTIVE_SUBOS");
    expect(fatal(eq(global.name, "XLINGS_ACTIVE_SUBOS")));
    expect(!global.present);
    auto project = decided(xlings_env("proj-dir"), "XLINGS_ACTIVE_SUBOS");
    expect(fatal(eq(project.name, "XLINGS_ACTIVE_SUBOS")));
    expect(!project.present);

}};
