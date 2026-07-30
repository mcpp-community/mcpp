#include <gtest/gtest.h>

import std;
import mcpp.xlings;
import mcpp.platform.env;

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
    auto path = mcpp::platform::env::get("PATH").value_or("");
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

TEST(FindUsableNasm, FindsOnPathWhenVersionIsAdequate) {
    auto pathDir = make_tempdir("mcpp-nasm-path");
    auto expected = make_fake_nasm(pathDir, "NASM version 2.16.03 compiled on Jan  1 2026");

    // Prepend (not replace) PATH: the fake binary must shadow any real
    // system nasm ahead of it, but `env`/`bash` must stay resolvable for
    // the fake script's own shebang + the version-check subprocess.
    auto base = path_without_real_nasm();
    mcpp::platform::env::ScopedEnv path("PATH", pathDir.string() + ":" + base);

    auto home = make_tempdir("mcpp-nasm-home");  // no sandbox nasm present
    mcpp::xlings::Env env{.home = home};

    auto found = mcpp::xlings::find_usable_nasm(env);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, expected);

    std::filesystem::remove_all(pathDir);
    std::filesystem::remove_all(home);
}

TEST(FindUsableNasm, RejectsPathNasmBelowMinVersionAndFindsNoSandboxFallback) {
    auto pathDir = make_tempdir("mcpp-nasm-path-old");
    make_fake_nasm(pathDir, "NASM version 2.10.00 compiled on Jan  1 2020");

    auto base = path_without_real_nasm();
    mcpp::platform::env::ScopedEnv path("PATH", pathDir.string() + ":" + base);

    auto home = make_tempdir("mcpp-nasm-home-empty");  // no sandbox nasm
    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::find_usable_nasm(env).has_value());

    std::filesystem::remove_all(pathDir);
    std::filesystem::remove_all(home);
}

TEST(FindUsableNasm, FallsBackToSandboxWhenPathHasNoNasm) {
    // PATH has the usual system directories (so the sandbox script's own
    // shebang still resolves) but none of them contain a real `nasm`.
    mcpp::platform::env::ScopedEnv path("PATH", path_without_real_nasm());

    auto home = make_tempdir("mcpp-nasm-home-sandbox");
    auto sandboxDir = home / "data" / "xpkgs" / "xim-x-nasm" / "3.02";
    auto expected = make_fake_nasm(sandboxDir, "NASM version 3.02 compiled on Jan  1 2026");

    mcpp::xlings::Env env{.home = home};

    auto found = mcpp::xlings::find_usable_nasm(env);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, expected);

    std::filesystem::remove_all(home);
}

TEST(FindSandboxNasm, FindsUnderVersionedBinSubdir) {
    auto home = make_tempdir("mcpp-nasm-home-bin-subdir");
    auto sandboxDir = home / "data" / "xpkgs" / "xim-x-nasm" / "3.02" / "bin";
    auto expected = make_fake_nasm(sandboxDir, "NASM version 3.02 compiled on Jan  1 2026");

    mcpp::xlings::Env env{.home = home};

    auto found = mcpp::xlings::find_sandbox_nasm(env);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, expected);

    std::filesystem::remove_all(home);
}

TEST(FindSandboxNasm, RejectsInstalledNasmBelowMinVersion) {
    auto home = make_tempdir("mcpp-nasm-home-old-sandboxed");
    auto sandboxDir = home / "data" / "xpkgs" / "xim-x-nasm" / "2.10";
    make_fake_nasm(sandboxDir, "NASM version 2.10.00 compiled on Jan  1 2020");

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::find_sandbox_nasm(env).has_value());

    std::filesystem::remove_all(home);
}

TEST(FindSandboxNasm, NoSandboxDirectoryYieldsNullopt) {
    auto home = make_tempdir("mcpp-nasm-home-absent");
    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::find_sandbox_nasm(env).has_value());

    std::filesystem::remove_all(home);
}

#endif  // !defined(_WIN32)

TEST(XlingsIndexFreshness, RequiresDefaultMcpplibsIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsFreshDefaultMcpplibsIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    std::ofstream(home / "data" / "mcpplibs" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RequiresRefreshMarkerForDefaultMcpplibsIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RejectsStaleRefreshMarker) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    auto marker = home / "data" / "mcpplibs" / ".mcpp-index-updated";
    std::ofstream(marker) << "ok\n";
    std::filesystem::last_write_time(
        marker, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(7200));

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RequiresOfficialXimIndexEvenWhenDefaultIndexIsFresh) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "mcpplibs" / "pkgs");
    std::ofstream(home / "data" / "mcpplibs" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_official_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsFreshOfficialXimIndex) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_official_index_fresh(env, 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RequiresOfficialPackageFileEvenWhenOfficialIndexIsFresh) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsFreshOfficialPackageFile) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    std::filesystem::create_directories(home / "data" / "xim-pkgindex" / "pkgs" / "m");
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua") << "package = {}\n";

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, RejectsOfficialPackageCacheWithForeignPath) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    auto pkg = home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua";
    std::filesystem::create_directories(pkg.parent_path());
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(pkg) << "package = {}\n";
    std::ofstream(home / "data" / "xim-pkgindex" / ".xlings-index-cache.json")
        << R"({"entries":{"musl-gcc":{"path":"/tmp/foreign/xim-pkgindex/pkgs/m/musl-gcc.lua"}}})";

    mcpp::xlings::Env env{.home = home};

    EXPECT_FALSE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexFreshness, AcceptsOfficialPackageCacheWithCurrentPath) {
    auto home = make_tempdir("mcpp-xlings-index-freshness");
    auto pkg = home / "data" / "xim-pkgindex" / "pkgs" / "m" / "musl-gcc.lua";
    std::filesystem::create_directories(pkg.parent_path());
    std::ofstream(home / "data" / "xim-pkgindex" / ".mcpp-index-updated") << "ok\n";
    std::ofstream(pkg) << "package = {}\n";
    std::ofstream(home / "data" / "xim-pkgindex" / ".xlings-index-cache.json")
        << std::format(R"({{"entries":{{"musl-gcc":{{"path":"{}"}}}}}})", pkg.string());

    mcpp::xlings::Env env{.home = home};

    EXPECT_TRUE(mcpp::xlings::is_official_package_index_fresh(env, "musl-gcc", 3600));

    std::filesystem::remove_all(home);
}

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

TEST(XlingsSiblingPackage, MetadataOnlyHuskIsNotContent) {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    // Only a husk exists: .xim-installed + .xpkg.lua, no payload.
    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    // Isolate from the host's ~/.xlings fallback.
    const char* oldHome = std::getenv("HOME");
    mcpp::platform::env::set("HOME", tmp.string());
    auto found = mcpp::xlings::paths::find_sibling_package(gccBin, "linux-headers");
    mcpp::platform::env::set("HOME", oldHome ? oldHome : "");

    EXPECT_FALSE(found.has_value());

    std::filesystem::remove_all(tmp);
}

TEST(XlingsSiblingPackage, SkipsHuskAndFindsPayloadUnderOtherPrefix) {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    auto found = mcpp::xlings::paths::find_sibling_package(gccBin, "linux-headers");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, real);

    std::filesystem::remove_all(tmp);
}

TEST(XlingsSiblingPackage, RequiredRelPathRejectsContentfulButWrongCandidate) {
    auto tmp = make_tempdir("mcpp-husk");
    auto xpkgs = tmp / "xpkgs";
    auto gccBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(gccBin);

    // Contentful but missing the payload that matters.
    auto stray = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(stray / "README.md");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    auto found = mcpp::xlings::paths::find_sibling_package(
        gccBin, "linux-headers", "include/linux/limits.h");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, real);

    std::filesystem::remove_all(tmp);
}

TEST(XlingsHomeTool, FindsPayloadUnderNonXimPrefix) {
    auto tmp = make_tempdir("mcpp-husk-home");
    auto xpkgs = tmp / "registry" / "data" / "xpkgs";

    auto husk = xpkgs / "xim-x-linux-headers" / "5.11.1";
    touch(husk / ".xim-installed");
    touch(husk / ".xpkg.lua", "package = {}");

    auto real = xpkgs / "scode-x-linux-headers" / "5.11.1";
    touch(real / "include" / "linux" / "limits.h");

    const char* oldMcppHome = std::getenv("MCPP_HOME");
    mcpp::platform::env::set("MCPP_HOME", tmp.string());
    auto found = mcpp::xlings::paths::find_home_tool(
        "linux-headers", "include/linux/limits.h");
    mcpp::platform::env::set("MCPP_HOME", oldMcppHome ? oldMcppHome : "");

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, real);

    std::filesystem::remove_all(tmp);
}

// ─── seed_xlings_json — per-repo artifact/source emission (#269) ──────

namespace {

std::string read_seeded(const std::filesystem::path& home) {
    std::stringstream ss;
    ss << std::ifstream(home / ".xlings.json").rdbuf();
    return ss.str();
}

}  // namespace

TEST(XlingsSeed, PlainRepoEmissionIsByteStable) {
    // Zero-diff gate: repos without artifact/source must serialize exactly
    // as before the SeedRepo extension.
    auto dir = make_tempdir("mcpp-seed-plain");
    mcpp::xlings::Env env;
    env.home = dir;
    std::vector<mcpp::xlings::SeedRepo> repos{
        { "mcpplibs", "https://x.git", "", "" } };
    mcpp::xlings::seed_xlings_json(env, repos);
    auto text = read_seeded(dir);
    EXPECT_NE(text.find(
        "    { \"name\": \"mcpplibs\", \"url\": \"https://x.git\" }\n"),
        std::string::npos) << text;
    EXPECT_EQ(text.find("artifact"), std::string::npos);
    EXPECT_EQ(text.find("source"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(XlingsSeed, ArtifactAndSourceEmittedWhenSet) {
    auto dir = make_tempdir("mcpp-seed-art");
    mcpp::xlings::Env env;
    env.home = dir;
    std::vector<mcpp::xlings::SeedRepo> repos{
        { "mcpplibs", "https://x.git",
          "https://github.com/xlings-res/mcpp-index", "auto" } };
    mcpp::xlings::seed_xlings_json(env, repos);
    auto text = read_seeded(dir);
    EXPECT_NE(text.find(
        "\"url\": \"https://x.git\", \"artifact\": "
        "\"https://github.com/xlings-res/mcpp-index\", \"source\": \"auto\""),
        std::string::npos) << text;
    std::filesystem::remove_all(dir);
}

// ─── Index identity + clock-skew hardening (#315) ────────────────────────

TEST(XlingsIndexFreshness, FutureMarkerIsStaleNotEternallyFresh) {
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

    mcpp::xlings::Env env{.home = home};
    auto st = mcpp::xlings::default_index_status(env, 3600);

    EXPECT_FALSE(st.fresh);
    EXPECT_EQ(st.ageSeconds, -1);   // unusable timestamp reports as unknown

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexRevision, ReadsTrimmedValueAndDegradesToNullopt) {
    auto home = make_tempdir("mcpp-xlings-index-rev");
    auto dir  = home / "data" / "mcpplibs";
    std::filesystem::create_directories(dir / "pkgs");

    // Absent: a local `path` index has no such file — must not be an error.
    EXPECT_FALSE(mcpp::xlings::index_revision(dir).has_value());

    // Present. The observed files carry no trailing newline, but a writer that
    // added one (or a \r from Windows) must not change the value, or every run
    // would look like the index had changed.
    std::ofstream(dir / ".xlings-index-version") << "  8d67478\r\n";
    auto rev = mcpp::xlings::index_revision(dir);
    ASSERT_TRUE(rev.has_value());
    EXPECT_EQ(*rev, "8d67478");

    // Blank is not an identity.
    std::ofstream(dir / ".xlings-index-version", std::ios::trunc) << "\n";
    EXPECT_FALSE(mcpp::xlings::index_revision(dir).has_value());

    // The value is OPAQUE: sub-indexes carry a date version, not a sha.
    std::ofstream(dir / ".xlings-index-version", std::ios::trunc) << "2026.7.30.1";
    EXPECT_EQ(mcpp::xlings::index_revision(dir).value(), "2026.7.30.1");

    std::filesystem::remove_all(home);
}

TEST(XlingsIndexRevision, StatusCarriesTheRevision) {
    auto home = make_tempdir("mcpp-xlings-index-rev-status");
    auto dir  = home / "data" / "mcpplibs";
    std::filesystem::create_directories(dir / "pkgs");
    std::ofstream(dir / ".mcpp-index-updated") << "ok\n";
    std::ofstream(dir / ".xlings-index-version") << "abc1234";

    mcpp::xlings::Env env{.home = home};
    auto st = mcpp::xlings::default_index_status(env, 3600);

    ASSERT_TRUE(st.rev.has_value());
    EXPECT_EQ(*st.rev, "abc1234");

    std::filesystem::remove_all(home);
}
