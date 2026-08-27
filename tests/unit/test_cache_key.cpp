#include <gtest/gtest.h>

import std;
import mcpp.build.cache_key;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.toolchain.detect;

namespace ck = mcpp::build::cache_key;

namespace {

ck::BuildAxes axes() {
    ck::BuildAxes b;
    b.compilerId      = "gcc";
    b.compilerVersion = "16.1.0";
    b.driverIdentity  = "0123456789abcdef";
    b.targetTriple    = "x86_64-linux-gnu";
    b.stdlibId        = "libstdc++";
    b.stdlibVersion   = "16.1.0";
    b.cppStandard     = "c++23";
    b.cppStandardFlag = "-std=c++23";
    b.cStandard       = "c11";
    b.optLevel        = "2";
    b.debug           = false;
    return b;
}

ck::PackageAxes pkg() {
    ck::PackageAxes p;
    p.indexName   = "compat";
    p.packageName = "compat.zlib";
    p.version     = "1.3.2";
    p.cflags      = {"-D_GNU_SOURCE"};
    p.sources     = {"zlib-1.3.2/adler32.c", "zlib-1.3.2/deflate.c"};
    return p;
}

mcpp::modgraph::PackageRoot rootAt(const std::filesystem::path& at) {
    mcpp::modgraph::PackageRoot r;
    r.root = at;
    r.manifest.package.name = "zlib";
    r.manifest.package.namespace_ = "compat";
    r.manifest.package.version = "1.3.2";
    return r;
}

} // namespace

// The defect this key exists to fix: the consumer's identity used to be folded
// into every dependency's cache path (the whole-project fingerprint serializes
// every package in the graph, including the root's name and version), so
// `mcpp version bump` invalidated the whole cache and two projects never shared
// an entry. Axes A–F contain nothing about the consumer, so there is no input
// this test could vary to demonstrate the old behaviour — the closest positive
// statement is that a dependency's key is a pure function of its own axes.
TEST(CacheKey, IsStableAcrossCallsForTheSameInputs) {
    EXPECT_EQ(ck::key_hex(axes(), pkg()), ck::key_hex(axes(), pkg()));
}

TEST(CacheKey, KeyIsSixteenHexChars) {
    auto k = ck::key_hex(axes(), pkg());
    EXPECT_EQ(k.size(), 16u);
    for (char c : k)
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c))) << k;
}

// ── C axis: the profile. This is the one that was missing entirely, and the
// reason a --release build could be served -O0 -g dependency objects. ────────
TEST(CacheKey, ProfileOptLevelChangesTheKey) {
    auto a = axes();
    auto b = axes(); b.optLevel = "0";
    EXPECT_NE(ck::key_hex(a, pkg()), ck::key_hex(b, pkg()));
}

TEST(CacheKey, ProfileDebugLtoStripEachChangeTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.debug = true; EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.lto   = true; EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.strip = true; EXPECT_NE(ck::key_hex(b, pkg()), base); }
}

// ── A axis: toolchain identity ───────────────────────────────────────────────
TEST(CacheKey, ToolchainIdentityChangesTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.compilerId      = "clang";  EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.compilerVersion = "15.1.0"; EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.targetTriple    = "x86_64-linux-musl";
                                                     EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.stdlibId        = "libc++"; EXPECT_NE(ck::key_hex(b, pkg()), base); }
    // Two payloads that both call themselves "gcc 16.1.0" but are not the same
    // build must not share an entry.
    { auto b = axes(); b.driverIdentity  = "ffffffffffffffff";
                                                     EXPECT_NE(ck::key_hex(b, pkg()), base); }
}

// ⚠️ The axis that exists because an UPGRADE broke a build.
//
// A freestanding triple silently implies -march/-mabi/-ffreestanding/
// -nostdinc++/-fno-exceptions/-fno-rtti, and WHICH ones is mcpp's decision —
// so the set changes between mcpp versions while the triple string does not.
// Keyed on the triple alone, an mcpp that adds a flag reuses BMIs built
// before it, and clang refuses them with a message that names a .pcm file:
//
//     error: exception handling was enabled in precompiled file
//     'mcpplibs.riscv_virt_rt.pcm' but is currently disabled
//
// Measured on exactly that upgrade, on a project that had built once before.
TEST(CacheKey, TargetImpliedFlagsChangeTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.targetImpliedFlags = {"-ffreestanding"};
                       EXPECT_NE(ck::key_hex(b, pkg()), base); }
    // Adding one flag to an existing set must move it too — the upgrade case
    // is never "empty becomes non-empty", it is "one more flag than before".
    auto before = axes(); before.targetImpliedFlags = {"-ffreestanding", "-nostdinc++"};
    auto after  = axes(); after.targetImpliedFlags  = {"-ffreestanding", "-nostdinc++",
                                                       "-fno-exceptions"};
    EXPECT_NE(ck::key_hex(after, pkg()), ck::key_hex(before, pkg()));
}

// ── B axis: language and dialect ─────────────────────────────────────────────
TEST(CacheKey, LanguageAndDialectChangeTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.cppStandard     = "c++26";  EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.cppStandardFlag = "-std=c++2c";
                                                     EXPECT_NE(ck::key_hex(b, pkg()), base); }
    // A dependency built at c++20 must never be handed to a c++23 graph: BMIs
    // are not compatible across levels (GCC: "language dialect differs").
    { auto b = axes(); b.cppStandard     = "c++20";
      b.cppStandardFlag = "-std=c++20";               EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.dialectFlags    = {"-freflection"};
                                                     EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.cStandard       = "c17";    EXPECT_NE(ck::key_hex(b, pkg()), base); }
    { auto b = axes(); b.macosDeploymentTarget = "14.0";
                                                     EXPECT_NE(ck::key_hex(b, pkg()), base); }
}

// ── D axis: package identity ─────────────────────────────────────────────────
TEST(CacheKey, PackageIdentityChangesTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    { auto p = pkg(); p.version     = "1.3.1";  EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.packageName = "compat.zstd";
                                                EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.indexName   = "other";  EXPECT_NE(ck::key_hex(axes(), p), base); }
}

// ── E axis: the package's own config ─────────────────────────────────────────
TEST(CacheKey, OwnBuildConfigChangesTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    { auto p = pkg(); p.cflags.push_back("-DEXTRA"); EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.cxxflags = {"-fno-rtti"};    EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.defines  = {"FOO=1"};        EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.globFlags = {"glob:*.c"};    EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.generatedFiles = {"cfg.h=1"};EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.includeDirs = {"pub:<store>/x"};
                                                     EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.sources.push_back("zlib-1.3.2/gzread.c");
                                                     EXPECT_NE(ck::key_hex(axes(), p), base); }
    { auto p = pkg(); p.features = {"main"};         EXPECT_NE(ck::key_hex(axes(), p), base); }
}

// ── F axis: upstream keys (Merkle) ───────────────────────────────────────────
// A dependency's artifacts are bound to the exact upstream artifacts they were
// built against — GCC embeds a CRC of an imported module's BMI into the
// importer's BMI, so a stale importer BMI fails with `module 'X' CRC mismatch`.
// The key therefore folds in the upstream's key, not a summary of its interface.
TEST(CacheKey, UpstreamKeyChangesTheKey) {
    auto base = ck::key_hex(axes(), pkg());
    auto p = pkg();
    p.upstreamKeys = {"1111111111111111"};
    auto withUp = ck::key_hex(axes(), p);
    EXPECT_NE(withUp, base);

    p.upstreamKeys = {"2222222222222222"};
    EXPECT_NE(ck::key_hex(axes(), p), withUp);
}

TEST(CacheKey, UpstreamOrderDoesNotMatterWhenSorted) {
    auto p1 = pkg(); p1.upstreamKeys = {"1111111111111111", "2222222222222222"};
    auto p2 = pkg(); p2.upstreamKeys = {"1111111111111111", "2222222222222222"};
    EXPECT_EQ(ck::key_hex(axes(), p1), ck::key_hex(axes(), p2));
}

// Length-prefixed field joining: without it, ("a","bc") and ("ab","c") collide,
// and for a cache key a collision means serving one package's objects for
// another's.
TEST(CacheKey, AdjacentFieldsCannotCollide) {
    auto p1 = pkg(); p1.packageName = "a";  p1.version = "bc";
    auto p2 = pkg(); p2.packageName = "ab"; p2.version = "c";
    EXPECT_NE(ck::key_hex(axes(), p1), ck::key_hex(axes(), p2));
}

TEST(CacheKey, ListBoundariesCannotCollide) {
    auto p1 = pkg(); p1.cflags = {"-DA", "-DB"};
    auto p2 = pkg(); p2.cflags = {"-DA-DB"};
    EXPECT_NE(ck::key_hex(axes(), p1), ck::key_hex(axes(), p2));
}

// entry.json carries these inputs verbatim so a hit can be validated field by
// field. Anything in the key must therefore also be in the JSON, or validation
// would pass on an input the key distinguishes.
TEST(CacheKey, JsonCarriesEveryAxis) {
    auto j = ck::to_json(axes(), pkg());
    EXPECT_EQ(j["epoch"], ck::kCacheEpoch);
    for (auto k : {"toolchain", "language", "profile", "package", "config", "upstream"})
        EXPECT_TRUE(j.contains(k)) << k;
    EXPECT_EQ(j["toolchain"]["compiler"], "gcc");
    EXPECT_EQ(j["profile"]["opt_level"], "2");
    EXPECT_EQ(j["package"]["version"], "1.3.2");
    for (auto k : {"features", "cflags", "cxxflags", "ldflags", "defines",
                   "glob_flags", "generated_files", "include_dirs",
                   "source_globs", "sources"})
        EXPECT_TRUE(j["config"].contains(k)) << k;
}

TEST(CacheKey, JsonDiffersWheneverTheKeyDiffers) {
    auto b = axes(); b.optLevel = "0";
    EXPECT_NE(ck::to_json(axes(), pkg()), ck::to_json(b, pkg()));
}

// Payload include dirs are absolute paths under <mcppHome>/registry/data/xpkgs.
// Left absolute, every entry would miss after a home relocation or on another
// machine — which is most of the point of a cross-project cache.
TEST(CacheKey, IncludeDirsAreRelativizedAgainstTheStoreRoot) {
    auto makeKeyFor = [](const std::filesystem::path& store) {
        auto pkgRoot = rootAt(store / "compat-x-compat.zlib" / "1.3.2");
        pkgRoot.manifest.buildConfig.includeDirs = {
            store / "compat-x-compat.zlib" / "1.3.2" / "include",
        };
        ck::PackageAxes p;
        p.indexName = "compat"; p.packageName = "compat.zlib"; p.version = "1.3.2";
        ck::fill_package_config(p, pkgRoot, store);
        return std::pair{ck::key_hex(axes(), p), p.includeDirs};
    };
    auto [k1, dirs1] = makeKeyFor("/home/alice/.mcpp/registry/data/xpkgs");
    auto [k2, dirs2] = makeKeyFor("/opt/ci/mcpp/registry/data/xpkgs");
    EXPECT_EQ(k1, k2);
    ASSERT_FALSE(dirs1.empty());
    EXPECT_NE(dirs1.front().find("<store>"), std::string::npos) << dirs1.front();
    EXPECT_EQ(dirs1, dirs2);
}

// A path that is under the package root but not under the store (a generated
// directory, say) gets a package-relative placeholder for the same reason.
TEST(CacheKey, PackageRootRelativeDirsAreAlsoRelativized) {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto pkgRoot = rootAt("/somewhere/else/zlib");
    pkgRoot.manifest.buildConfig.includeDirs = {"/somewhere/else/zlib/generated"};
    ck::PackageAxes p;
    ck::fill_package_config(p, pkgRoot, store);
    ASSERT_FALSE(p.includeDirs.empty());
    EXPECT_NE(p.includeDirs.front().find("<pkg>"), std::string::npos)
        << p.includeDirs.front();
}

TEST(CacheKey, FillPackageConfigCarriesFlagsAndGeneratedFiles) {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto pkgRoot = rootAt(store / "compat-x-compat.zlib" / "1.3.2");
    pkgRoot.manifest.buildConfig.cflags   = {"-D_GNU_SOURCE"};
    pkgRoot.manifest.buildConfig.cxxflags = {"-fno-exceptions"};
    pkgRoot.manifest.buildConfig.defines  = {"ZLIB_CONST"};
    pkgRoot.manifest.buildConfig.cStandard = "c11";
    pkgRoot.manifest.buildConfig.generatedFiles = {{"cfg.h", "#define A 1"}};

    ck::PackageAxes p;
    ck::fill_package_config(p, pkgRoot, store);
    EXPECT_NE(std::ranges::find(p.cflags, "-D_GNU_SOURCE"), p.cflags.end());
    EXPECT_NE(std::ranges::find(p.cxxflags, "-fno-exceptions"), p.cxxflags.end());
    EXPECT_NE(std::ranges::find(p.defines, "ZLIB_CONST"), p.defines.end());
    ASSERT_EQ(p.generatedFiles.size(), 1u);
    EXPECT_EQ(p.generatedFiles.front(), "cfg.h=#define A 1");
    // A package's own C standard reaches its own C units, so it must be in the
    // key even though the whole-graph C standard is on the B axis.
    bool sawCStd = false;
    for (auto& f : p.cflags) if (f.find("c_standard=c11") != std::string::npos) sawCStd = true;
    EXPECT_TRUE(sawCStd);
}

// Generated files are a map; iteration order must not leak into the key.
TEST(CacheKey, GeneratedFilesAreOrderIndependent) {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto build = [&](std::vector<std::pair<std::string, std::string>> entries) {
        auto pkgRoot = rootAt(store / "p" / "1");
        for (auto& [k, v] : entries)
            pkgRoot.manifest.buildConfig.generatedFiles[k] = v;
        ck::PackageAxes p;
        ck::fill_package_config(p, pkgRoot, store);
        return ck::key_hex(axes(), p);
    };
    EXPECT_EQ(build({{"a.h", "1"}, {"b.h", "2"}}),
              build({{"b.h", "2"}, {"a.h", "1"}}));
}

// ── The key must describe the compilation that will happen ──────────────────
//
// ⚠️⚠️ THIS KEY ONCE DERIVED ITS OWN INPUTS INSTEAD OF READING THE BUILD'S.
//
// `freestanding::compile_flags` takes `targetCxxRuntime` because the answer
// changes with it: a freestanding target whose graph supplies a C++ runtime is
// compiled WITHOUT `-fno-exceptions`, one whose graph does not is compiled
// with it. `flags.cppm` passes it; `build_axes` did not, and so hashed the
// flags of the other configuration.
//
// ⭐ THE FAILURE IS A HIT ACROSS AN INCOMPATIBILITY, NOT A MISS. Two
// configurations that must not share a slot produced the same key, so the
// second build loaded the first's BMIs:
//
//     error: exception handling was enabled in precompiled file
//            'openkal.stream.pcm' but is currently disabled
//
// In the cache itself, `openkal@0.7.0` held six fingerprint slots with five
// differently-sized copies of that one BMI: slotting per configuration was
// working, choosing the slot was not.
//
// ⚠️ IT WAS DORMANT UNTIL THE PARAMETER EXISTED. Before the flag became
// conditional the two computations agreed for every input, so ignoring one of
// them was still correct. That is what makes the test worth writing against
// `build_axes` rather than against a hand-built `BuildAxes`: the fixture above
// cannot express the defect, because the defect is in the derivation.
namespace {
mcpp::toolchain::Toolchain freestanding_tc(bool cxxRuntimeFromGraph) {
    mcpp::toolchain::Toolchain tc;
    tc.compiler          = mcpp::toolchain::CompilerId::Clang;
    tc.version           = "22.1.8";
    tc.driverIdent       = "clang-22.1.8";
    tc.targetTriple      = "riscv64-none-elf";
    tc.targetCxxRuntime  = cxxRuntimeFromGraph;
    return tc;
}
}

TEST(CacheKey, AGraphSuppliedCxxRuntimeChangesTheFreestandingFlags) {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    const auto without = ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, "");
    const auto with    = ck::build_axes(freestanding_tc(true),  m, "-std=c++23", {}, "");

    // The flags themselves differ — this is the fact the key has to carry.
    EXPECT_NE(without.targetImpliedFlags, with.targetImpliedFlags);

    // And the one that has no C++ runtime is the one that gets the pair.
    const auto has = [](const std::vector<std::string>& v, std::string_view f) {
        return std::ranges::find(v, f) != v.end();
    };
    EXPECT_TRUE(has(without.targetImpliedFlags, "-fno-exceptions"));
    EXPECT_FALSE(has(with.targetImpliedFlags,   "-fno-exceptions"));
}

TEST(CacheKey, TheTwoFreestandingConfigurationsDoNotShareASlot) {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    EXPECT_NE(ck::key_hex(ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, ""), pkg()),
              ck::key_hex(ck::build_axes(freestanding_tc(true),  m, "-std=c++23", {}, ""), pkg()));
}

// ⭐ AND A HOSTED TARGET IS UNAFFECTED, so the fix cannot be read as "the key
// now changes with something it should not". `freestanding::resolve` returns
// nothing for a hosted triple, and the flags stay empty either way.
TEST(CacheKey, AHostedTargetHasNoTargetImpliedFlagsEitherWay) {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    auto tc = freestanding_tc(false); tc.targetTriple = "x86_64-linux-gnu";
    auto a  = ck::build_axes(tc, m, "-std=c++23", {}, "");
    tc.targetCxxRuntime = true;
    auto b  = ck::build_axes(tc, m, "-std=c++23", {}, "");
    EXPECT_TRUE(a.targetImpliedFlags.empty()) << a.targetImpliedFlags.size();
    EXPECT_EQ(a.targetImpliedFlags, b.targetImpliedFlags);
}

// ⭐⭐ THE HEADER SET THE DRIVER IS POINTED AT IS PART OF THE IDENTITY.
//
// Everything else on axis A describes the COMPILER. Nothing described the
// LIBRARY it compiles against — and the two are separately installed: one clang
// payload sits above whichever `xim:glibc` and `xim:linux-headers` the home
// carries. `driverIdentity` cannot cover it by design, because
// `normalize_driver_output` strips paths so that one entry CAN be shared
// between two homes.
//
// Measured (mcpp#514 §B): two host builds resolving glibc 2.39 and 2.44 hit one
// entry, and the mixed BMIs crashed the clang frontend during deserialization
// rather than producing a diagnostic.
TEST(CacheKey, TheHeaderSetIsPartOfTheKey) {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";

    auto a = ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, "");
    auto b = a;
    // The axis is filled from the resolvers in a real build; here it is set
    // directly, because what this asserts is that the KEY reads it — not how
    // linkmodel discovers a payload, which is that module's own test.
    b.targetHeaderSet = { "-isystem<store>/xim-x-glibc/2.44/include" };
    a.targetHeaderSet = { "-isystem<store>/xim-x-glibc/2.39/include" };

    EXPECT_NE(ck::key_hex(a, pkg()), ck::key_hex(b, pkg()));
}

// ...and it travels in entry.json, so a suspected wrong hit can be read field
// by field rather than guessed at. `is_cached` compares `inputs.toolchain` as
// one object, which is also why an entry written before this axis existed is a
// miss without needing `kCacheEpoch` to move.
TEST(CacheKey, TheHeaderSetIsRecordedInTheEntry) {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    auto a = ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, "");
    a.targetHeaderSet = { "-isystem<store>/xim-x-glibc/2.44/include" };
    auto j = ck::to_json(a, pkg());
    ASSERT_TRUE(j["toolchain"].contains("target_header_set"));
    EXPECT_EQ(j["toolchain"]["target_header_set"][0],
              "-isystem<store>/xim-x-glibc/2.44/include");
}
