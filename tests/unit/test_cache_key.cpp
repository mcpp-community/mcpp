
import std;
import mcpplibs.mtest;
import mcpp.build.cache_key;
import nlohmann.json;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.toolchain.detect;

using namespace mcpplibs::mtest;

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
UTest is_stable_across_calls_for_the_same_inputs{"CacheKey/IsStableAcrossCallsForTheSameInputs", [] {
    expect(eq(ck::key_hex(axes(), pkg()), ck::key_hex(axes(), pkg())));
}};

UTest key_is_sixteen_hex_chars{"CacheKey/KeyIsSixteenHexChars", [] {
    auto k = ck::key_hex(axes(), pkg());
    expect(eq(k.size(), 16u));
    for (char c : k)
        expect(bool(std::isxdigit(static_cast<unsigned char>(c)))) << k;
}};

// ── C axis: the profile. This is the one that was missing entirely, and the
// reason a --release build could be served -O0 -g dependency objects. ────────
UTest profile_opt_level_changes_the_key{"CacheKey/ProfileOptLevelChangesTheKey", [] {
    auto a = axes();
    auto b = axes(); b.optLevel = "0";
    expect(ne(ck::key_hex(a, pkg()), ck::key_hex(b, pkg())));
}};

UTest profile_debug_lto_strip_each_change_the_key{"CacheKey/ProfileDebugLtoStripEachChangeTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.debug = true; expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.lto   = true; expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.strip = true; expect(ne(ck::key_hex(b, pkg()), base)); }
}};

// ── A axis: toolchain identity ───────────────────────────────────────────────
UTest toolchain_identity_changes_the_key{"CacheKey/ToolchainIdentityChangesTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.compilerId      = "clang";  expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.compilerVersion = "15.1.0"; expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.targetTriple    = "x86_64-linux-musl";
                                                     expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.stdlibId        = "libc++"; expect(ne(ck::key_hex(b, pkg()), base)); }
    // Two payloads that both call themselves "gcc 16.1.0" but are not the same
    // build must not share an entry.
    { auto b = axes(); b.driverIdentity  = "ffffffffffffffff";
                                                     expect(ne(ck::key_hex(b, pkg()), base)); }
}};

// The axis that exists because an UPGRADE broke a build.
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
UTest target_implied_flags_change_the_key{"CacheKey/TargetImpliedFlagsChangeTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.targetImpliedFlags = {"-ffreestanding"};
                       expect(ne(ck::key_hex(b, pkg()), base)); }
    // Adding one flag to an existing set must move it too — the upgrade case
    // is never "empty becomes non-empty", it is "one more flag than before".
    auto before = axes(); before.targetImpliedFlags = {"-ffreestanding", "-nostdinc++"};
    auto after  = axes(); after.targetImpliedFlags  = {"-ffreestanding", "-nostdinc++",
                                                       "-fno-exceptions"};
    expect(ne(ck::key_hex(after, pkg()), ck::key_hex(before, pkg())));
}};

// ── B axis: language and dialect ─────────────────────────────────────────────
UTest language_and_dialect_change_the_key{"CacheKey/LanguageAndDialectChangeTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    { auto b = axes(); b.cppStandard     = "c++26";  expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.cppStandardFlag = "-std=c++2c";
                                                     expect(ne(ck::key_hex(b, pkg()), base)); }
    // A dependency built at c++20 must never be handed to a c++23 graph: BMIs
    // are not compatible across levels (GCC: "language dialect differs").
    { auto b = axes(); b.cppStandard     = "c++20";
      b.cppStandardFlag = "-std=c++20";               expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.dialectFlags    = {"-freflection"};
                                                     expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.cStandard       = "c17";    expect(ne(ck::key_hex(b, pkg()), base)); }
    // ONE SLOT, BOTH PLATFORMS. It was `macosDeploymentTarget`; Android's
    // minimum API level is the same quantity and is in this key for the same
    // reason -- it selects which bionic symbols are visible, so two levels are
    // two ABIs. A target is either Apple or Android, so one slot cannot be
    // asked to hold both at once.
    { auto b = axes(); b.minPlatformVersion = "14.0";
                                                     expect(ne(ck::key_hex(b, pkg()), base)); }
    { auto b = axes(); b.minPlatformVersion = "24";
                                                     expect(ne(ck::key_hex(b, pkg()), base)); }
    // AND TWO LEVELS ARE TWO KEYS, which is the property the Android row
    // depends on: without it `min_api_level = 21` and `= 24` would share a
    // build directory and the second build would serve the first's objects.
    { auto b21 = axes(); b21.minPlatformVersion = "21";
      auto b24 = axes(); b24.minPlatformVersion = "24";
      expect(ne(ck::key_hex(b21, pkg()), ck::key_hex(b24, pkg()))); }
}};

// ── D axis: package identity ─────────────────────────────────────────────────
UTest package_identity_changes_the_key{"CacheKey/PackageIdentityChangesTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    { auto p = pkg(); p.version     = "1.3.1";  expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.packageName = "compat.zstd";
                                                expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.indexName   = "other";  expect(ne(ck::key_hex(axes(), p), base)); }
}};

// ── E axis: the package's own config ─────────────────────────────────────────
UTest own_build_config_changes_the_key{"CacheKey/OwnBuildConfigChangesTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    { auto p = pkg(); p.cflags.push_back("-DEXTRA"); expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.cxxflags = {"-fno-rtti"};    expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.defines  = {"FOO=1"};        expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.globFlags = {"glob:*.c"};    expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.generatedFiles = {"cfg.h=1"};expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.includeDirs = {"pub:<store>/x"};
                                                     expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.sources.push_back("zlib-1.3.2/gzread.c");
                                                     expect(ne(ck::key_hex(axes(), p), base)); }
    { auto p = pkg(); p.features = {"main"};         expect(ne(ck::key_hex(axes(), p), base)); }
}};

// ── F axis: upstream keys (Merkle) ───────────────────────────────────────────
// A dependency's artifacts are bound to the exact upstream artifacts they were
// built against — GCC embeds a CRC of an imported module's BMI into the
// importer's BMI, so a stale importer BMI fails with `module 'X' CRC mismatch`.
// The key therefore folds in the upstream's key, not a summary of its interface.
UTest upstream_key_changes_the_key{"CacheKey/UpstreamKeyChangesTheKey", [] {
    auto base = ck::key_hex(axes(), pkg());
    auto p = pkg();
    p.upstreamKeys = {"1111111111111111"};
    auto withUp = ck::key_hex(axes(), p);
    expect(ne(withUp, base));

    p.upstreamKeys = {"2222222222222222"};
    expect(ne(ck::key_hex(axes(), p), withUp));
}};

UTest upstream_order_does_not_matter_when_sorted{"CacheKey/UpstreamOrderDoesNotMatterWhenSorted", [] {
    auto p1 = pkg(); p1.upstreamKeys = {"1111111111111111", "2222222222222222"};
    auto p2 = pkg(); p2.upstreamKeys = {"1111111111111111", "2222222222222222"};
    expect(eq(ck::key_hex(axes(), p1), ck::key_hex(axes(), p2)));
}};

// Length-prefixed field joining: without it, ("a","bc") and ("ab","c") collide,
// and for a cache key a collision means serving one package's objects for
// another's.
UTest adjacent_fields_cannot_collide{"CacheKey/AdjacentFieldsCannotCollide", [] {
    auto p1 = pkg(); p1.packageName = "a";  p1.version = "bc";
    auto p2 = pkg(); p2.packageName = "ab"; p2.version = "c";
    expect(ne(ck::key_hex(axes(), p1), ck::key_hex(axes(), p2)));
}};

UTest list_boundaries_cannot_collide{"CacheKey/ListBoundariesCannotCollide", [] {
    auto p1 = pkg(); p1.cflags = {"-DA", "-DB"};
    auto p2 = pkg(); p2.cflags = {"-DA-DB"};
    expect(ne(ck::key_hex(axes(), p1), ck::key_hex(axes(), p2)));
}};

// entry.json carries these inputs verbatim so a hit can be validated field by
// field. Anything in the key must therefore also be in the JSON, or validation
// would pass on an input the key distinguishes.
UTest json_carries_every_axis{"CacheKey/JsonCarriesEveryAxis", [] {
    auto j = ck::to_json(axes(), pkg());
    expect(eq(j["epoch"], ck::kCacheEpoch));
    for (auto k : {"toolchain", "language", "profile", "package", "config", "upstream"})
        expect(j.contains(k)) << k;
    expect(eq(j["toolchain"]["compiler"], "gcc"));
    expect(eq(j["profile"]["opt_level"], "2"));
    expect(eq(j["package"]["version"], "1.3.2"));
    for (auto k : {"features", "cflags", "cxxflags", "ldflags", "defines",
                   "glob_flags", "generated_files", "include_dirs",
                   "source_globs", "sources"})
        expect(j["config"].contains(k)) << k;
}};

UTest json_differs_whenever_the_key_differs{"CacheKey/JsonDiffersWheneverTheKeyDiffers", [] {
    auto b = axes(); b.optLevel = "0";
    expect(ne(ck::to_json(axes(), pkg()), ck::to_json(b, pkg())));
}};

// Payload include dirs are absolute paths under <mcppHome>/registry/data/xpkgs.
// Left absolute, every entry would miss after a home relocation or on another
// machine — which is most of the point of a cross-project cache.
UTest include_dirs_are_relativized_against_the_store_root{"CacheKey/IncludeDirsAreRelativizedAgainstTheStoreRoot", [] {
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
    expect(eq(k1, k2));
    expect(fatal(!dirs1.empty()));
    expect(ne(dirs1.front().find("<store>"), std::string::npos)) << dirs1.front();
    expect(eq(dirs1, dirs2));
}};

// A path that is under the package root but not under the store (a generated
// directory, say) gets a package-relative placeholder for the same reason.
UTest package_root_relative_dirs_are_also_relativized{"CacheKey/PackageRootRelativeDirsAreAlsoRelativized", [] {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto pkgRoot = rootAt("/somewhere/else/zlib");
    pkgRoot.manifest.buildConfig.includeDirs = {"/somewhere/else/zlib/generated"};
    ck::PackageAxes p;
    ck::fill_package_config(p, pkgRoot, store);
    expect(fatal(!p.includeDirs.empty()));
    expect(ne(p.includeDirs.front().find("<pkg>"), std::string::npos))
        << p.includeDirs.front();
}};

UTest fill_package_config_carries_flags_and_generated_files{"CacheKey/FillPackageConfigCarriesFlagsAndGeneratedFiles", [] {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto pkgRoot = rootAt(store / "compat-x-compat.zlib" / "1.3.2");
    pkgRoot.manifest.buildConfig.cflags   = {"-D_GNU_SOURCE"};
    pkgRoot.manifest.buildConfig.cxxflags = {"-fno-exceptions"};
    pkgRoot.manifest.buildConfig.defines  = {"ZLIB_CONST"};
    pkgRoot.manifest.buildConfig.cStandard = "gnu11";
    pkgRoot.manifest.buildConfig.generatedFiles = {{"cfg.h", "#define A 1"}};

    ck::PackageAxes p;
    ck::fill_package_config(p, pkgRoot, store);
    expect(ne(std::ranges::find(p.cflags, "-D_GNU_SOURCE"), p.cflags.end()));
    expect(ne(std::ranges::find(p.cxxflags, "-fno-exceptions"), p.cxxflags.end()));
    expect(ne(std::ranges::find(p.defines, "ZLIB_CONST"), p.defines.end()));
    expect(fatal(eq(p.generatedFiles.size(), 1u)));
    expect(eq(p.generatedFiles.front(), "cfg.h=#define A 1"));
    // A package's own C standard reaches its own C units, so it must be in the
    // key; the B axis carries only the engine default (#695).
    bool sawCStd = false;
    for (auto& f : p.cflags) if (f.find("c_standard=gnu11") != std::string::npos) sawCStd = true;
    expect(bool(sawCStd));
}};

// Declaring the default is declaring nothing: the unit's command is the same,
// so the key is the same, and a package that spells `c11` shares its cached
// objects with one that leaves the key out (#695).
UTest declaring_the_default_c_standard_keys_nothing{"CacheKey/DeclaringTheDefaultCStandardKeysNothing", [] {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto declared = rootAt(store / "compat-x-compat.zlib" / "1.3.2");
    declared.manifest.buildConfig.cStandard = "c11";
    auto silent = rootAt(store / "compat-x-compat.zlib" / "1.3.2");

    ck::PackageAxes a, b;
    ck::fill_package_config(a, declared, store);
    ck::fill_package_config(b, silent, store);
    expect(eq(a.cflags, b.cflags));
    for (auto& f : a.cflags) expect(eq(f.find("c_standard="), std::string::npos)) << f;
}};

// Generated files are a map; iteration order must not leak into the key.
UTest generated_files_are_order_independent{"CacheKey/GeneratedFilesAreOrderIndependent", [] {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto build = [&](std::vector<std::pair<std::string, std::string>> entries) {
        auto pkgRoot = rootAt(store / "p" / "1");
        for (auto& [k, v] : entries)
            pkgRoot.manifest.buildConfig.generatedFiles[k] = v;
        ck::PackageAxes p;
        ck::fill_package_config(p, pkgRoot, store);
        return ck::key_hex(axes(), p);
    };
    expect(eq(build({{"a.h", "1"}, {"b.h", "2"}}), build({{"b.h", "2"}, {"a.h", "1"}})));
}};

// ── The key must describe the compilation that will happen ──────────────────
//
// THIS KEY ONCE DERIVED ITS OWN INPUTS INSTEAD OF READING THE BUILD'S.
//
// `freestanding::compile_flags` takes `targetCxxRuntime` because the answer
// changes with it: a freestanding target whose graph supplies a C++ runtime is
// compiled WITHOUT `-fno-exceptions`, one whose graph does not is compiled
// with it. `flags.cppm` passes it; `build_axes` did not, and so hashed the
// flags of the other configuration.
//
// THE FAILURE IS A HIT ACROSS AN INCOMPATIBILITY, NOT A MISS. Two
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
// IT WAS DORMANT UNTIL THE PARAMETER EXISTED. Before the flag became
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

UTest a_graph_supplied_cxx_runtime_changes_the_freestanding_flags{"CacheKey/AGraphSuppliedCxxRuntimeChangesTheFreestandingFlags", [] {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    const auto without = ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, "");
    const auto with    = ck::build_axes(freestanding_tc(true),  m, "-std=c++23", {}, "");

    // The flags themselves differ — this is the fact the key has to carry.
    expect(ne(without.targetImpliedFlags, with.targetImpliedFlags));

    // And the one that has no C++ runtime is the one that gets the pair.
    const auto has = [](const std::vector<std::string>& v, std::string_view f) {
        return std::ranges::find(v, f) != v.end();
    };
    expect(bool(has(without.targetImpliedFlags, "-fno-exceptions")));
    expect(!has(with.targetImpliedFlags,   "-fno-exceptions"));
}};

UTest the_two_freestanding_configurations_do_not_share_a_slot{"CacheKey/TheTwoFreestandingConfigurationsDoNotShareASlot", [] {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    expect(ne(ck::key_hex(ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, ""), pkg()), ck::key_hex(ck::build_axes(freestanding_tc(true),  m, "-std=c++23", {}, ""), pkg())));
}};

// AND A HOSTED TARGET IS UNAFFECTED, so the fix cannot be read as "the key
// now changes with something it should not". `freestanding::resolve` returns
// nothing for a hosted triple, and the flags stay empty either way.
UTest a_hosted_target_has_no_target_implied_flags_either_way{"CacheKey/AHostedTargetHasNoTargetImpliedFlagsEitherWay", [] {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    auto tc = freestanding_tc(false); tc.targetTriple = "x86_64-linux-gnu";
    auto a  = ck::build_axes(tc, m, "-std=c++23", {}, "");
    tc.targetCxxRuntime = true;
    auto b  = ck::build_axes(tc, m, "-std=c++23", {}, "");
    expect(a.targetImpliedFlags.empty()) << a.targetImpliedFlags.size();
    expect(eq(a.targetImpliedFlags, b.targetImpliedFlags));
}};

// THE SAME CLASS OF DEFECT AS THE FREESTANDING ONE ABOVE, FOUND THE SAME WAY
// — A CONFIG BIT THAT REACHES THE COMPILE BUT NOT THE KEY.
//
// The realised [c-abi] environment (design 2026-09-18) does not reach a
// package's compile command line through its own declared
// `manifest.buildConfig.cflags`/`cxxflags` at all — it is an ENGINE
// BROADCAST, appended into `PackageRoot::privateBuild.cflags`/`cxxflags`/
// `asmflags` by `prepare.cppm` once the target side resolves (the same
// channel `targetSideUsage` and `-D__OPENKAL__` use). `fill_package_config`
// used to read only `manifest.buildConfig.cflags`/`cxxflags` — the
// package's OWN declaration — so two builds of the identical package,
// realising two DIFFERENT C environments (LP64 vs LLP64, say), produced the
// identical cache key.
//
// MEASURED, NOT HYPOTHETICAL (coordinator report): upgrading mcpp in place,
// with the global build cache (`~/.mcpp/build-cache/v1`) left in place,
// served an object compiled under the PREVIOUS realised environment into an
// image built under the new one — two C environments in one image, with
// nothing diagnosing it, which is the exact invariant this whole design
// exists to protect. `--cache=off`, or clearing the cache directory,
// produced the correct numbers reproducibly — because both routes bypass
// this key entirely, not because the key was telling the two apart.
UTest two_different_realised_c_environments_do_not_share_a_slot{"CacheKey/TwoDifferentRealisedCEnvironmentsDoNotShareASlot", [] {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto build = [&](std::vector<std::string> cEnvTokens) {
        auto pkgRoot = rootAt(store / "musl-x-openkal-musl" / "1.0.0");
        pkgRoot.usageResolved = true;
        // What `prepare.cppm`'s broadcast actually does: seed from the
        // declared flags, then append the realised tokens — exactly the
        // shape `fill_package_config`'s own comment documents.
        pkgRoot.privateBuild.cflags   = pkgRoot.manifest.buildConfig.cflags;
        pkgRoot.privateBuild.cxxflags = pkgRoot.manifest.buildConfig.cxxflags;
        pkgRoot.privateBuild.asmflags = cEnvTokens;
        for (auto& t : cEnvTokens) {
            pkgRoot.privateBuild.cflags.push_back(t);
            pkgRoot.privateBuild.cxxflags.push_back(t);
        }
        ck::PackageAxes p;
        p.indexName = "musl"; p.packageName = "openkal-musl"; p.version = "1.0.0";
        ck::fill_package_config(p, pkgRoot, store);
        return ck::key_hex(axes(), p);
    };
    auto lp64Key  = build({"--target=x86_64-pc-cygwin", "-fno-short-wchar"});
    auto llp64Key = build({});
    expect(ne(lp64Key, llp64Key));
}};

// A DURABLE GUARD FOR THE WHOLE CLASS THE PREVIOUS TEST FOUND ONE INSTANCE
// OF — this predates the c-abi wave (`targetSideUsage`'s own broadcast, and
// `-D__OPENKAL__`, had the identical exposure before [c-abi] existed) and
// will recur: `PackageRoot::privateBuild` (`mcpp.modgraph.scanner
// ::UsageRequirements`) is the engine's OWN channel for "this reaches a
// package's compile command line even though the package wrote nothing" —
// today's members are `includeDirs`, `includeDirsAfter`, `cflags`,
// `cxxflags`, `asmflags`, `ldflags`, `modules` — and every one of them has
// to move `fill_package_config`'s output, because the cache key's only job
// is to describe what actually reaches the compiler.
//
// C++ has no reflection this side of the standard that could enumerate
// `UsageRequirements`'s members and fail this test automatically the day a
// new one is added without a matching line below — so this is the nearest
// substitute the language allows: one assertion per CURRENT member, so the
// list itself is the checklist. Adding a member to `UsageRequirements`
// without adding its case here and its read in `fill_package_config` is
// exactly the shape of the defect `TwoDifferentRealisedCEnvironmentsDoNotSh
// areASlot` above caught — if you are adding one, add it in both places in
// the SAME change.
//
// `ldflags` and `modules` are asserted UNCOVERED, on purpose: nothing
// broadcasts into `privateBuild.ldflags` or `.modules` today (checked by
// grep across `src/` and `modules/` when this test was written), so there
// is nothing yet for `fill_package_config` to be missing on those two — the
// day something does broadcast into either, this pair of assertions must
// flip (start reading it) at the same time the broadcast is added, not
// after.
UTest every_private_build_broadcast_field_reaches_the_key{"CacheKey/EveryPrivateBuildBroadcastFieldReachesTheKey", [] {
    std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    auto keyFor = [&](auto mutate) {
        auto pkgRoot = rootAt(store / "p" / "1");
        pkgRoot.usageResolved = true;
        mutate(pkgRoot);
        ck::PackageAxes p;
        p.indexName = "p"; p.packageName = "p"; p.version = "1";
        ck::fill_package_config(p, pkgRoot, store);
        return ck::key_hex(axes(), p);
    };
    const auto baseline = keyFor([](auto&) {});

    expect(ne(baseline, keyFor([](auto& r) {
        r.privateBuild.includeDirs = {"/somewhere/broadcast-only-include"};
    }))) << "privateBuild.includeDirs";
    expect(ne(baseline, keyFor([](auto& r) {
        r.privateBuild.includeDirsAfter = {"/somewhere/broadcast-only-after"};
    }))) << "privateBuild.includeDirsAfter";
    expect(ne(baseline, keyFor([](auto& r) {
        r.privateBuild.cflags = {"-Dbroadcast_only_c"};
    }))) << "privateBuild.cflags";
    expect(ne(baseline, keyFor([](auto& r) {
        r.privateBuild.cxxflags = {"-Dbroadcast_only_cxx"};
    }))) << "privateBuild.cxxflags";
    expect(ne(baseline, keyFor([](auto& r) {
        r.privateBuild.asmflags = {"-Dbroadcast_only_asm"};
    }))) << "privateBuild.asmflags";

    // Documented gap, not an oversight — see the comment above.
    expect(eq(baseline, keyFor([](auto& r) {
        r.privateBuild.ldflags = {"-Wl,--broadcast-only"};
    }))) << "privateBuild.ldflags is not yet a live broadcast channel; if "
           "this starts failing, something now writes it, and "
           "fill_package_config must be taught to read it in the SAME change "
           "that flips this expectation";
    expect(eq(baseline, keyFor([](auto& r) {
        r.privateBuild.modules = {"broadcast.only.module"};
    }))) << "privateBuild.modules is not yet a live broadcast channel; see "
           "the ldflags case above for what to do when it becomes one";
}};

// THE HEADER SET THE DRIVER IS POINTED AT IS PART OF THE IDENTITY.
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
UTest the_header_set_is_part_of_the_key{"CacheKey/TheHeaderSetIsPartOfTheKey", [] {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";

    auto a = ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, "");
    auto b = a;
    // The axis is filled from the resolvers in a real build; here it is set
    // directly, because what this asserts is that the KEY reads it — not how
    // linkmodel discovers a payload, which is that module's own test.
    b.targetHeaderSet = { "-isystem<store>/xim-x-glibc/2.44/include" };
    a.targetHeaderSet = { "-isystem<store>/xim-x-glibc/2.39/include" };

    expect(ne(ck::key_hex(a, pkg()), ck::key_hex(b, pkg())));
}};

// ...and it travels in entry.json, so a suspected wrong hit can be read field
// by field rather than guessed at. `is_cached` compares `inputs.toolchain` as
// one object, which is also why an entry written before this axis existed is a
// miss without needing `kCacheEpoch` to move.
UTest the_header_set_is_recorded_in_the_entry{"CacheKey/TheHeaderSetIsRecordedInTheEntry", [] {
    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    auto a = ck::build_axes(freestanding_tc(false), m, "-std=c++23", {}, "");
    a.targetHeaderSet = { "-isystem<store>/xim-x-glibc/2.44/include" };
    auto j = ck::to_json(a, pkg());
    expect(fatal(j["toolchain"].contains("target_header_set")));
    expect(eq(j["toolchain"]["target_header_set"][0], "-isystem<store>/xim-x-glibc/2.44/include"));
}};

// AND THE PATHS IN IT ARE RELATIVE — INCLUDING THE ONES OUTSIDE `<store>`.
//
// THE TWO TESTS ABOVE SET THE AXIS DIRECTLY, so neither of them runs the
// relativization, and a store-only rule passed both while leaving this
// developer's own home in every key. The toolchain that showed it is the
// ordinary one: `CLibMode::Sysroot`, whose single compile token is
//
//     --sysroot=<home>/registry/subos/default
//
// — under the HOME and not under `<store>`. An absolute key there is not
// WRONG, it is unshareable: two homes holding the same payloads stop hitting
// each other's entries, which is the property `normalize_driver_output` gives
// up path information to preserve.
//
// THE CRITERION IS THE ABSENCE OF THE HOME, not the presence of a tag.
// Asserting `starts_with("<home>")` would still pass if the rest of the string
// carried the absolute path behind it.
UTest the_header_set_carries_no_absolute_home{"CacheKey/TheHeaderSetCarriesNoAbsoluteHome", [] {
    const std::filesystem::path home  = "/home/somebody/.mcpp/registry";
    const std::filesystem::path store = home / "data" / "xpkgs";

    mcpp::toolchain::Toolchain tc;
    tc.compiler     = mcpp::toolchain::CompilerId::GCC;   // no cfg ⇒ Sysroot mode
    tc.version      = "16.1.0";
    tc.driverIdent  = "gcc-16.1.0";
    tc.targetTriple = "x86_64-linux-gnu";
    tc.base.root      = home / "subos" / "default";

    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    const auto a = ck::build_axes(tc, m, "-std=c++23", {}, "", store);

    // A denominator: with an empty axis every assertion below is vacuous, and
    // an empty axis is itself the bug in the other direction.
    expect(fatal(!a.targetHeaderSet.empty()));
    for (auto const& t : a.targetHeaderSet)
        expect(eq(t.find(home.generic_string()), std::string::npos)) << t;

    // Two subos on ONE home are still two keys — the relativization must not
    // flatten the distinction it exists to preserve.
    auto tc2 = tc;
    tc2.base.root = home / "subos" / "musl";
    const auto b = ck::build_axes(tc2, m, "-std=c++23", {}, "", store);
    expect(ne(a.targetHeaderSet, b.targetHeaderSet));
    expect(ne(ck::key_hex(a, pkg()), ck::key_hex(b, pkg())));
}};

// Position-independent code was ABSENT from this key (issue #519).
//
// `-fPIC` is whole-build — one shared link unit anywhere and every object in
// the graph carries it — but the key hashes a package's DECLARED flags, not
// the ones the flag builder computes, so the two configurations collided.
// Survivable only while a package's form was fixed by its author. Once a
// consumer can write `dependency_linkage = "shared"`, a hit across that line
// hands non-PIC objects to a shared link, and the message names a relocation
// in a file nobody edited.
UTest pic_changes_the_key{"CacheKey/PicChangesTheKey", [] {
    auto b = axes();
    auto p = pkg();
    const auto without = ck::key_hex(b, p);
    b.pic = true;
    expect(ne(ck::key_hex(b, p), without));
}};

UTest pic_is_recorded_in_entry_json_so_a_hit_can_be_audited{"CacheKey/PicIsRecordedInEntryJsonSoAHitCanBeAudited", [] {
    auto b = axes();
    b.pic = true;
    auto j = ck::to_json(b, pkg());
    expect(fatal(j["profile"].contains("pic")));
    expect(eq(j["profile"]["pic"], true));
}};

// The CRT model on the MSVC ABI is whole-build like PIC (#718): switching
// `cxx_runtime` must not be served an object compiled against the other CRT.
// Off the MSVC ABI the axis is empty, so no other entry's key moves.
UTest the_msvc_crt_model_changes_the_key_only_on_the_msvc_abi{"CacheKey/TheMsvcCrtModelChangesTheKeyOnlyOnTheMsvcAbi", [] {
    auto key_for = [](std::string triple, mcpp::toolchain::CompilerId id,
                      std::string cxxRuntime) {
        mcpp::toolchain::Toolchain tc;
        tc.compiler = id;
        tc.targetTriple = std::move(triple);
        mcpp::manifest::Manifest m;
        m.buildConfig.cxxRuntime = std::move(cxxRuntime);
        auto b = ck::build_axes(tc, m, "-std=c++23", {}, {});
        return std::pair{b.crt, ck::key_hex(b, pkg())};
    };
    using mcpp::toolchain::CompilerId;
    for (auto id : {CompilerId::Clang, CompilerId::MSVC}) {
        auto [crtStatic, keyStatic]   = key_for("x86_64-pc-windows-msvc", id, "self-contained");
        auto [crtDynamic, keyDynamic] = key_for("x86_64-pc-windows-msvc", id, "toolchain-coupled");
        expect(!crtStatic.empty());
        expect(ne(crtStatic, crtDynamic));
        expect(ne(keyStatic, keyDynamic));
    }
    auto [crtA, keyA] = key_for("x86_64-linux-gnu", CompilerId::GCC, "self-contained");
    auto [crtB, keyB] = key_for("x86_64-linux-gnu", CompilerId::GCC, "toolchain-coupled");
    expect(crtA.empty());
    expect(crtB.empty());
}};

UTest pic_defaults_off_so_existing_entries_keep_their_identity{"CacheKey/PicDefaultsOffSoExistingEntriesKeepTheirIdentity", [] {
    ck::BuildAxes fresh;
    expect(!fresh.pic);
}};

// #641 item 2: a C++-layer provider's implementation units are compiled at the
// level its manifest states, and the graph's level alone does not name it. The
// entry must move when that statement moves, and nothing else's entry may move
// because the statement exists.
UTest a_cxx_layer_providers_stated_standard_changes_the_key{"CacheKey/ACxxLayerProvidersStatedStandardChangesTheKey", [] {
    const std::filesystem::path store = "/home/u/.mcpp/registry/data/xpkgs";
    const auto pkgRoot = store / "llvm-x-libcxx/22.1.8.3/libcxx-22.1.8.3";
    auto key_for = [&](std::vector<std::string> provides, std::string standard) {
        auto r = rootAt(pkgRoot);
        r.manifest.provides = std::move(provides);
        if (!standard.empty()) {
            r.manifest.package.standard = standard;
            r.manifest.package.standardDeclared = true;
        }
        auto p = pkg();
        ck::fill_package_config(p, r, store);
        return ck::key_hex(axes(), p);
    };
    const std::vector<std::string> layer = {"hosted-standard-library"};
    const auto none = key_for({}, "");
    // A provider that states nothing, and a non-provider that states a level,
    // hash exactly as a package that says neither.
    expect(eq(key_for(layer, ""), none));
    expect(eq(key_for({}, "c++23"), none));
    // A stated level on a provider moves the key, and two levels are two keys.
    expect(ne(key_for(layer, "c++23"), none));
    expect(ne(key_for(layer, "c++23"), key_for(layer, "c++26")));
}};

// THE MSVC TOOLSET OF THE CLANG ROW IS IN THE KEY. Before it reached the
// command line the driver found the STL by itself and `stdlibVersion` held
// clang's version, so a Visual Studio update that changed the STL left the key
// unchanged. The toolset directory carries its version, so two toolsets are
// two keys.
UTest the_msvc_toolset_of_the_clang_row_changes_the_key{"CacheKey/TheMsvcToolsetOfTheClangRowChangesTheKey", [] {
    mcpp::toolchain::Toolchain tc;
    tc.compiler          = mcpp::toolchain::CompilerId::Clang;
    tc.version           = "20.1.7";
    tc.driverIdent       = "clang-20.1.7";
    tc.targetTriple      = "x86_64-pc-windows-msvc";
    tc.base.msvc.toolsDir      = "C:/VS/VC/Tools/MSVC/14.44.35207";
    tc.base.msvc.sdkRoot    = "C:/Kits/10";
    tc.base.msvc.sdkVersion = "10.0.26100.0";

    mcpp::manifest::Manifest m;
    m.package.standard = "c++23";
    const auto a = ck::build_axes(tc, m, "-std=c++23", {}, "");
    bool present = false;
    for (auto const& t : a.targetHeaderSet)
        present = present || t.find("14.44.35207") != std::string::npos;
    expect(bool(present)) << "the toolset directory is part of the header set";

    auto tc2 = tc;
    tc2.base.msvc.toolsDir = "C:/VS/VC/Tools/MSVC/14.51.36014";
    expect(ne(ck::key_hex(a, pkg()), ck::key_hex(ck::build_axes(tc2, m, "-std=c++23", {}, ""), pkg())));

    auto tc3 = tc;
    tc3.base.msvc.sdkVersion = "10.0.22621.0";
    expect(ne(ck::key_hex(a, pkg()), ck::key_hex(ck::build_axes(tc3, m, "-std=c++23", {}, ""), pkg())));
}};

// THE BMI PATH CONVENTION MOVES THE KEY OF A PACKAGE THAT HAS ONE, AND NO
// OTHER. A GCC BMI records the paths of the modules it imports as its module
// map spelt them; the map's spelling changed in 2026.10.26.2 (`$root`), and an
// entry written under the old one cannot be imported under the new. The axis
// is set only for a package that provides a module, so a package of object
// files keeps the key it had — the serialization of an unset axis is the
// serialization from before the axis existed.
UTest the_bmi_path_convention_moves_only_the_packages_that_have_one{"CacheKey/TheBmiPathConventionMovesOnlyThePackagesThatHaveOne", [] {
    auto withModules = pkg();
    withModules.bmiPathConvention = "$root=bmi-dir";
    expect(ne(ck::key_hex(axes(), pkg()), ck::key_hex(axes(), withModules)));

    const auto j = ck::to_json(axes(), withModules);
    expect(fatal(j["language"].contains("bmi_path_convention")));
    expect(eq(j["language"]["bmi_path_convention"], "$root=bmi-dir"));
    expect(!(ck::to_json(axes(), pkg())["language"].contains("bmi_path_convention")));
}};
