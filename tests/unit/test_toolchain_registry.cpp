#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;

using namespace mcpp::toolchain;

static std::string host_musl() {
    return std::string(mcpp::platform::host_arch) + "-linux-musl";
}

// The host-native `musl-gcc` package only exists for Linux hosts; on other
// hosts the payload mapping resolves the triple-named package (a linux-musl
// target from macOS/Windows is cross by definition).
static std::string expected_musl_xim() {
    if constexpr (mcpp::platform::is_linux) return "musl-gcc";
    else                                    return host_musl() + "-gcc";
}

// Frontend candidates are host-aware for the same reason the mingw ones are:
// they are resolved with filesystem::exists, and on a Windows host the file on
// disk is `<triple>-g++.exe`. The .exe spelling comes first so it wins on a
// case-insensitive filesystem where both would match.
static std::string expected_musl_frontend(const std::string& triple) {
    if constexpr (mcpp::platform::is_windows) return triple + "-g++.exe";
    else                                      return triple + "-g++";
}

// ── canonical two-axis identity ──────────────────────────────────────────────

TEST(ToolchainRegistry, MapsGccSpecToGccPackage) {
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_TRUE(spec->is_host_target());
    EXPECT_EQ(spec->spec_str(), "gcc@16.1.0");
    EXPECT_EQ(spec->display(), "gcc@16.1.0");
    EXPECT_TRUE(spec->compatHint.empty());

    auto pkg = to_xim_package(*spec);
#if defined(_WIN32)
    // gcc family on a Windows host = MinGW-w64 (the GNU-env host toolchain).
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
#else
    EXPECT_EQ(pkg.ximName, "gcc");
    EXPECT_TRUE(pkg.needsGccPostInstallFixup);
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++");
#endif
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    EXPECT_EQ(pkg.display_spec(), "gcc@16.1.0");
}

TEST(ToolchainRegistry, LegacyMuslSuffixNormalizesToMuslTarget) {
    // "gcc@15.1.0-musl" — the variant moves out of the version and into the
    // target axis: (gcc, 15.1.0, <host>-linux-musl).
    auto spec = parse_toolchain_spec("gcc@15.1.0-musl");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_EQ(spec->version, "15.1.0");
    EXPECT_EQ(spec->target.str(), host_musl());
    EXPECT_FALSE(spec->compatHint.empty());      // legacy spelling → hint

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximName, expected_musl_xim());
    EXPECT_EQ(pkg.ximVersion, "15.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), expected_musl_frontend(host_musl()));
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(ToolchainRegistry, CrossArchMuslTargetPicksTripleNamedPackage) {
    // Target arch ≠ host arch → the triple-named cross package.
    ToolchainSpec spec;
    spec.family = Family::Gcc;
    spec.version = "16.1.0";
    spec.target = { "aarch64", "linux", "musl" };
    if (mcpp::platform::host_arch == std::string_view("aarch64"))
        spec.target.arch = "riscv64";                 // stay cross on any host

    auto pkg = to_xim_package(spec);
    EXPECT_EQ(pkg.ximName, spec.target.str() + "-gcc");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
    EXPECT_EQ(pkg.frontendCandidates.front(), expected_musl_frontend(spec.target.str()));
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
}

TEST(ToolchainRegistry, WindowsGnuTargetIsHostSplitAtDistributionLayer) {
    // ONE identity (gcc → x86_64-windows-gnu); the payload is host-split:
    // winlibs mingw-gcc on Windows hosts, the Linux-hosted MSVCRT cross
    // elsewhere. "cross" appears only in the xim package name — never in
    // the user-facing spec.
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(spec.has_value());
    spec->target = { "x86_64", "windows", "gnu" };

    auto pkg = to_xim_package(*spec);
    EXPECT_EQ(pkg.ximVersion, "16.1.0");
    ASSERT_FALSE(pkg.frontendCandidates.empty());
#if defined(_WIN32)
    EXPECT_EQ(pkg.ximName, "mingw-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "g++.exe");
#else
    EXPECT_EQ(pkg.ximName, "mingw-cross-gcc");
    EXPECT_EQ(pkg.frontendCandidates.front(), "x86_64-w64-mingw32-g++");
#endif
    EXPECT_FALSE(pkg.needsGccPostInstallFixup);
    EXPECT_EQ(pkg.display_spec(), "gcc@16.1.0 → x86_64-windows-gnu");
}

TEST(ToolchainRegistry, LegacyMingwCrossSpellingCollapses) {
    auto spec = parse_toolchain_spec("mingw-cross@16.1.0");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->family, Family::Gcc);
    EXPECT_EQ(spec->target.str(), "x86_64-windows-gnu");
    EXPECT_FALSE(spec->compatHint.empty());
}

TEST(ToolchainRegistry, MapsLlvmAndClangAliasesToLlvmPackage) {
    auto llvmSpec = parse_toolchain_spec("llvm", "20.1.7");
    auto clangSpec = parse_toolchain_spec("clang@20.1.7");
    ASSERT_TRUE(llvmSpec.has_value()) << llvmSpec.error();
    ASSERT_TRUE(clangSpec.has_value()) << clangSpec.error();

    EXPECT_EQ(llvmSpec->family, Family::Llvm);
    EXPECT_EQ(clangSpec->family, Family::Llvm);      // alias family → llvm
    EXPECT_TRUE(llvmSpec->compatHint.empty());
    EXPECT_FALSE(clangSpec->compatHint.empty());

    auto llvmPkg = to_xim_package(*llvmSpec);
    auto clangPkg = to_xim_package(*clangSpec);
    EXPECT_EQ(llvmPkg.ximName, "llvm");
    EXPECT_EQ(clangPkg.ximName, "llvm");
    EXPECT_EQ(clangPkg.display_spec(), "llvm@20.1.7");
    ASSERT_FALSE(clangPkg.frontendCandidates.empty());
#if defined(_WIN32)
    EXPECT_EQ(clangPkg.frontendCandidates.front(), "clang++.exe");
#else
    EXPECT_EQ(clangPkg.frontendCandidates.front(), "clang++");
#endif
}

TEST(ToolchainRegistry, ResolvesPartialMuslVersion) {
    auto spec = parse_toolchain_spec("gcc", "15-musl");
    ASSERT_TRUE(spec.has_value()) << spec.error();
    EXPECT_EQ(spec->version, "15");
    EXPECT_EQ(spec->target.str(), host_musl());

    auto resolved = with_resolved_xim_version(*spec, "15.1.0");
    auto pkg = to_xim_package(resolved);
    EXPECT_EQ(resolved.version, "15.1.0");
    EXPECT_EQ(pkg.ximName, expected_musl_xim());
    EXPECT_EQ(pkg.ximVersion, "15.1.0");
    EXPECT_EQ(pkg.display_spec(),
              std::format("gcc@15.1.0 → {}", host_musl()));
}

TEST(ToolchainRegistry, RejectsUnknownFamily) {
    auto spec = parse_toolchain_spec("tcc@1.0");
    EXPECT_FALSE(spec.has_value());
}

// ── payload reverse mapping ──────────────────────────────────────────────────

TEST(ToolchainRegistry, IdentifiesToolchainPayloadsAndSkipsOthers) {
    auto gcc = identify_xim_payload("gcc");
    ASSERT_TRUE(gcc.has_value());
    EXPECT_EQ(gcc->family, Family::Gcc);
    EXPECT_TRUE(gcc->target.empty());

    auto musl = identify_xim_payload("musl-gcc");
    ASSERT_TRUE(musl.has_value());
    EXPECT_EQ(musl->target.str(), host_musl());

    auto crossMusl = identify_xim_payload("aarch64-linux-musl-gcc");
    ASSERT_TRUE(crossMusl.has_value());
    EXPECT_EQ(crossMusl->target.str(), "aarch64-linux-musl");

    auto llvm = identify_xim_payload("llvm");
    ASSERT_TRUE(llvm.has_value());
    EXPECT_EQ(llvm->family, Family::Llvm);

    // Non-toolchain xpkgs must not be identified (list/doctor filter on this).
    EXPECT_FALSE(identify_xim_payload("ninja").has_value());
    EXPECT_FALSE(identify_xim_payload("glibc").has_value());
    EXPECT_FALSE(identify_xim_payload("python").has_value());
    EXPECT_FALSE(identify_xim_payload("linux-headers").has_value());
}

// #367: which GCC payload backs a NATIVE Linux build.
//
// `xim:gcc` declares `archs = { "x86_64" }` and publishes assets for that arch
// only; the GCC the ecosystem ships for other Linux architectures is
// `musl-gcc` (xlings-res carries musl-gcc-16.1.0-linux-aarch64.tar.gz). Asking
// for `gcc` on aarch64 therefore 404s — which is what made every project whose
// graph contains a `build.mcpp` unbuildable there, since a build program's
// host compile resolves the spec with no target injection and landed on the
// glibc package.
//
// Tested through a free function taking the host arch rather than the
// compile-time constant, precisely so the aarch64 answer is checkable from an
// x86_64 machine.
TEST(ToolchainRegistry, NativeGccPayloadFollowsWhatTheArchActuallyPublishes) {
    using mcpp::toolchain::gcc_native_payload_is_musl;
    const mcpp::toolchain::triple::Triple none{};

    // x86_64: unchanged — the glibc package is the one that exists.
    EXPECT_FALSE(gcc_native_payload_is_musl("x86_64", true, none));
    EXPECT_FALSE(gcc_native_payload_is_musl(
        "x86_64", true, {"x86_64", "linux", "gnu"}));

    // aarch64: the glibc package has no asset, musl-gcc does.
    EXPECT_TRUE(gcc_native_payload_is_musl("aarch64", true, none));
    EXPECT_TRUE(gcc_native_payload_is_musl(
        "aarch64", true, {"aarch64", "linux", "gnu"}));

    // A CROSS target keeps its own payload rule — this is about the native
    // one, and `<triple>-gcc` packages answer for the rest.
    EXPECT_FALSE(gcc_native_payload_is_musl(
        "aarch64", true, {"x86_64", "linux", "gnu"}));

    // Non-Linux hosts are out of scope: macOS uses llvm, Windows mingw/msvc.
    EXPECT_FALSE(gcc_native_payload_is_musl("aarch64", false, none));
}

// ─── the origin axis ─────────────────────────────────────────────────────
//
// `@system` is not a general spelling and must not become one. mcpp is built
// on xlings, a user-space OS, and the design drives host dependencies to a
// minimum: a toolchain comes from a payload the manifest names, so every
// machine compiles with the same compiler. `msvc@system` is a concession to
// ONE platform — Visual Studio is very often already installed and cannot
// always be redistributed.

// THE NDK'S OWN FLOOR, READ FROM THE PAYLOAD RATHER THAN COMPILED IN.
//
// Android's API level is not optional -- bionic's <sys/cdefs.h> stops the build
// with "Unversioned target triples are not supported!" -- so a project that
// declares no `min_api_level` still needs one. The number comes from
// `meta/platforms.json`, which is upstream's own declaration of the range it
// supports, so a newer NDK changes the default by being installed.
//
// A CONSTANT HERE WOULD BE THE DEFECT THIS AVOIDS: this repository has
// recorded more than once that a version written into a comment becomes a
// version in a diagnostic and then in somebody's install command.
TEST(ToolchainRegistry, TheNdkApiLevelFloorIsReadFromThePayloadsOwnMetadata) {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path()
              / ("mcpp-ndk-meta-" + std::to_string(::getpid()));
    fs::remove_all(root);
    // The real layout: the compiler sits four directories below the NDK root,
    // and `meta/` is a sibling of `toolchains/`.
    auto bin = root / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin";
    fs::create_directories(bin);
    fs::create_directories(root / "meta");
    auto clangxx = bin / "clang++";
    { std::ofstream o(clangxx); o << "#!/bin/sh\n"; }

    // r30's actual values.
    {
        std::ofstream o(root / "meta" / "platforms.json");
        o << R"({"min": 21, "max": 37, "aliases": {"N": 24}})";
    }
    EXPECT_EQ(mcpp::toolchain::ndk_min_api_level(clangxx), 21);

    // A DIFFERENT PAYLOAD ANSWERS DIFFERENTLY, which is the whole point of
    // reading it: the same code must not return 21 for an NDK that says 24.
    {
        std::ofstream o(root / "meta" / "platforms.json");
        o << R"({"min": 24, "max": 40})";
    }
    EXPECT_EQ(mcpp::toolchain::ndk_min_api_level(clangxx), 24);

    // 0 WHEN IT CANNOT BE READ, and the caller turns that into a refusal
    // naming `min_api_level`. A guessed level would be worse than the refusal:
    // it selects which bionic symbols exist, so guessing produces an artefact
    // that links here and fails to load on a device.
    fs::remove(root / "meta" / "platforms.json");
    EXPECT_EQ(mcpp::toolchain::ndk_min_api_level(clangxx), 0);

    // Malformed rather than absent -- same answer, and no exception escapes.
    { std::ofstream o(root / "meta" / "platforms.json"); o << "{not json"; }
    EXPECT_EQ(mcpp::toolchain::ndk_min_api_level(clangxx), 0);

    // Present but not a number: still 0, never a silent 1 from a cast.
    { std::ofstream o(root / "meta" / "platforms.json"); o << R"({"min": "21"})"; }
    EXPECT_EQ(mcpp::toolchain::ndk_min_api_level(clangxx), 0);

    // A path that is not inside an NDK at all walks to the filesystem root and
    // stops; it must not loop.
    EXPECT_EQ(mcpp::toolchain::ndk_min_api_level(
                  fs::temp_directory_path() / "definitely-not-an-ndk" / "clang++"), 0);

    fs::remove_all(root);
}

TEST(ToolchainOrigin, MsvcIsTheOnlyFamilyWithASystemSpelling) {
    auto msvcSystem = parse_toolchain_spec("msvc@system");
    ASSERT_TRUE(msvcSystem.has_value()) << msvcSystem.error();
    EXPECT_TRUE(is_system_toolchain(*msvcSystem));
    EXPECT_EQ(origin_of(*msvcSystem), Origin::SystemMsvc);

    // A VERSIONED msvc spec is the other origin — that split is the whole
    // point of the version axis.
    auto pinned = parse_toolchain_spec("msvc@14.44.35207");
    ASSERT_TRUE(pinned.has_value());
    EXPECT_EQ(origin_of(*pinned), Origin::Managed);

    auto gcc = parse_toolchain_spec("gcc@16.1.0");
    ASSERT_TRUE(gcc.has_value());
    EXPECT_EQ(origin_of(*gcc), Origin::Managed);
}

TEST(ToolchainOrigin, NonMsvcSystemIsRejectedWhereItIsReadAndOffersTheAlternatives) {
    // It used to parse, and then fail somewhere else entirely as
    // `xim:gcc@system` → "no such package" — which sends the reader looking
    // for a version that was never going to exist. The error has to name both
    // things the user might have meant.
    for (auto spec : {"gcc@system", "llvm@system"}) {
        auto r = parse_toolchain_spec(spec);
        ASSERT_FALSE(r.has_value()) << spec << " was accepted";
        EXPECT_NE(r.error().find("only msvc"), std::string::npos) << r.error();
        EXPECT_NE(r.error().find("@<version>"), std::string::npos)
            << "the pin alternative is not offered: " << r.error();
        // The family-less PATH escape hatch is the OTHER thing they might
        // have wanted, and it is a different mechanism.
        EXPECT_NE(r.error().find("PATH compiler"), std::string::npos)
            << "the escape hatch is not offered: " << r.error();
        EXPECT_EQ(r.error().find("xim:"), std::string::npos)
            << "still leaking the package spelling that cannot exist: "
            << r.error();
    }
}

// The Linux sysroot payloads (`xim:glibc` + `xim:linux-headers`) had two
// derivations, and a comment on one claimed it mirrored the other. It did
// not: the PE term was missing from the second.
TEST(ToolchainSysrootDeps, OneDerivationForTheGlibcSysrootPayloads) {
    mcpp::toolchain::triple::Triple host{};                 // empty = host
    mcpp::toolchain::triple::Triple musl{std::string(mcpp::platform::host_arch), "linux", "musl"};
    mcpp::toolchain::triple::Triple mingw{"x86_64", "windows", "gnu"};

    if constexpr (mcpp::platform::is_linux) {
        EXPECT_TRUE(needs_linux_sysroot_payloads(host));
        // Self-contained: a musl payload carries its own C library.
        EXPECT_FALSE(needs_linux_sysroot_payloads(musl));
        // THE TERM THAT WAS MISSING. A PE target brings its own CRT, whether
        // it is a native MinGW or the Linux-hosted cross, so a Linux sysroot
        // is not part of installing one.
        EXPECT_FALSE(needs_linux_sysroot_payloads(mingw));
    } else {
        // No Linux sysroot exists to want.
        for (auto const& t : {host, musl, mingw})
            EXPECT_FALSE(needs_linux_sysroot_payloads(t));
    }
}

// ─── An SDK payload is chosen by the TARGET, and knows its own layout ──────
//
// `to_xim_package` returned the generic llvm payload for every `Family::Llvm`
// spec, which is right for the targets llvm itself serves and wrong for the two
// that arrive with their own clang. `em++` and the NDK's `clang++` ARE clang --
// same family, same flag vocabulary -- so no fourth `Family` value exists;
// what changes is which package answers and where its driver lives.
TEST(SdkPayloads, TheTargetChoosesThePackageAndThePackageKnowsItsLayout) {
    auto pkg_for = [](std::string_view target) {
        auto spec = mcpp::toolchain::parse_toolchain_spec("emsdk@6.0.9");
        // The spec's own target is replaced, because the NDK case must be
        // reachable from the same family with a different triple.
        auto s = *spec;
        if (auto t = mcpp::toolchain::triple::parse(target)) s.target = *t;
        return mcpp::toolchain::to_xim_package(s);
    };

    {   // Emscripten: `em++` is a wrapper in `emscripten/`, NOT the raw clang
        // in `bin/`. Naming the wrapper is the whole point -- `bin/clang`
        // compiles for wasm and then links like an ordinary clang, producing a
        // module with none of Emscripten's JavaScript glue.
        auto pkg = pkg_for("wasm32-emscripten");
        EXPECT_EQ(pkg.ximName, "emsdk");
        EXPECT_EQ(pkg.frontendSubdir, "emscripten");
        ASSERT_FALSE(pkg.frontendCandidates.empty());
        EXPECT_EQ(pkg.frontendCandidates.front(), "em++");
    }
    {   // Android: ONE payload for both arches -- the arch arrives as
        // `--target=<arch>-linux-android<api>`, not as a different package --
        // and the host tuple in the path is the HOST's, not the target's.
        for (auto target : {"aarch64-linux-android", "x86_64-linux-android"}) {
            auto pkg = pkg_for(target);
            EXPECT_EQ(pkg.ximName, "android-ndk") << target;
            EXPECT_NE(pkg.frontendSubdir.find("toolchains/llvm/prebuilt/"),
                      std::string::npos) << target;
            // The HOST, so an aarch64 Linux machine cross-compiling still
            // reads `linux-x86_64`. Asserted as "not the target's arch" rather
            // than against a literal, so this test says the same thing on
            // every runner.
            EXPECT_EQ(pkg.frontendSubdir.find("aarch64-linux-android"),
                      std::string::npos) << target;
        }
    }
    {   // And every other target still gets the generic llvm payload in bin/.
        auto pkg = pkg_for("x86_64-linux-gnu");
        EXPECT_NE(pkg.ximName, "emsdk");
        EXPECT_NE(pkg.ximName, "android-ndk");
        EXPECT_EQ(pkg.frontendSubdir, "bin");
    }
}

// THE MESSAGE MUST NAME THE DIRECTORY THAT WAS SEARCHED.
//
// Five refusals printed `payload->binDir`, the directory they had composed
// themselves. Once the package decides where its frontend lives, `bin` is a
// directory nothing looked in -- and this codebase's most frequent defect is a
// fixed lookup with an unfixed message.
TEST(SdkPayloads, TheSearchedDirectoryIsAvailableForTheDiagnostic) {
    auto spec = mcpp::toolchain::parse_toolchain_spec("emsdk@6.0.9");
    ASSERT_TRUE(spec.has_value());
    auto pkg = mcpp::toolchain::to_xim_package(*spec);
    auto dir = mcpp::toolchain::payload_frontend_dir("/p/xim-x-emsdk/6.0.9", pkg);
    EXPECT_EQ(dir, std::filesystem::path("/p/xim-x-emsdk/6.0.9/emscripten"));
}

// AN SDK IS SERVED WHERE THE SDK IS PUBLISHED, and the first version of this
// said "wherever" -- the same over-broad shape as the branches it sits above.
// The target matrix caught it: declaring the row servable on macOS and Windows
// would have claimed a payload that does not exist there.
TEST(SdkPayloads, ServedOnEveryHostTheSdkIsPublishedFor) {
    // THIS ASSERTION USED TO BE TRUE BY ARITHMETIC ON ONE HOST.
    //
    // It read `EXPECT_EQ(host_can_serve(*wasm), mcpp::platform::is_linux)`,
    // which was the right claim while `xim:emsdk` and `xim:android-ndk`
    // declared only `xpm.linux`. Both now publish for all three hosts, and the
    // engine's constant was the stale half -- but the assertion kept passing
    // on Linux, because there `is_linux` IS `true`. A criterion whose expected
    // value is the host it runs on cannot report a change on the other two.
    //
    // Stated unconditionally now: these rows are servable everywhere, and this
    // test fails on macOS or Windows if the constant comes back.
    for (auto name : {"wasm32-emscripten", "aarch64-linux-android",
                      "x86_64-linux-android"}) {
        auto t = mcpp::toolchain::triple::parse(name);
        ASSERT_TRUE(t.has_value()) << name;
        EXPECT_TRUE(mcpp::toolchain::host_can_serve(*t)) << name;
    }

    // AND THE PREDICATE IS STILL ABLE TO SAY NO, which is what keeps the
    // paragraph above from being a tautology. macOS's SDK and MSVC are
    // host-only and no package substitutes for either, so a Linux host cannot
    // serve them -- the exclusion this function exists to make.
    if constexpr (mcpp::platform::is_linux) {
        auto mac = mcpp::toolchain::triple::parse("aarch64-macos");
        ASSERT_TRUE(mac.has_value());
        EXPECT_FALSE(mcpp::toolchain::host_can_serve(*mac));
    }
}

// ─── The payload is SAID, not only resolved (R3) ───────────────────────────
//
// `emsdk@6.0.9` normalises to the llvm family because `em++` IS clang, and a
// fourth family value would be a false claim about the compiler. The
// consequence was `Resolved llvm@6.0.9` -- indistinguishable from the real
// `xim:llvm`, and not what the user typed. The family and the payload are two
// questions, and `to_xim_package` already answered the second; this is the
// field that lets it be printed.
TEST(SdkPayloads, TheDisplayNamesThePayloadAndNotOnlyTheFamily) {
    auto em = mcpp::toolchain::parse_toolchain_spec("emsdk@6.0.9");
    ASSERT_TRUE(em.has_value());
    EXPECT_EQ(em->family, mcpp::toolchain::Family::Llvm)
        << "em++ is clang; a fourth family would be a false claim";
    EXPECT_EQ(em->payloadName, "emsdk");
    EXPECT_NE(em->display().find("emsdk@6.0.9"), std::string::npos)
        << em->display();
    EXPECT_EQ(em->display().find("llvm@"), std::string::npos) << em->display();

    auto ndk = mcpp::toolchain::parse_toolchain_spec("android-ndk@30.0.16248370");
    ASSERT_TRUE(ndk.has_value());
    EXPECT_EQ(ndk->family, mcpp::toolchain::Family::Llvm);
    EXPECT_EQ(ndk->payloadName, "android-ndk");
    EXPECT_NE(ndk->display().find("android-ndk@"), std::string::npos)
        << ndk->display();

    // AND NOTHING ELSE MOVES. Empty `payloadName` means the family's own
    // payload, which is every row but these two -- so no existing output
    // changes, which is what makes this additive.
    for (auto spelled : {"llvm@22.1.8", "gcc@16.1.0", "msvc@14.44.35207"}) {
        auto sp = mcpp::toolchain::parse_toolchain_spec(spelled);
        ASSERT_TRUE(sp.has_value()) << spelled;
        EXPECT_TRUE(sp->payloadName.empty()) << spelled;
        EXPECT_NE(sp->display().find(spelled), std::string::npos) << sp->display();
    }
}

// ─── The C compiler beside a C++ one ───────────────────────────────────────
//
// Every frontend this engine can resolve, and the C driver beside it. The
// DENOMINATOR IS `to_xim_package`'s own candidate lists: each spelling below
// appears in one of its branches, so a payload whose frontend is added there
// without a row here is a payload whose C units compile with a program that
// does not exist.
//
// That is not hypothetical. Measured 2026-09-11 on the conformance suite's one
// C translation unit for `wasm32-emscripten`:
//
//   /bin/sh: 1: .../xim-x-emsdk/6.0.9/emscripten/em: not found
//
// `em++` had become `em`, because "drop the `++`" was the rule and it is only
// clang's rule. `g` was already a special case for exactly this reason, which
// is what makes a table the right shape: the property is "this driver names
// its C compiler with a different word".
TEST(SdkPayloads, EveryFrontendNamesTheCCompilerBesideIt) {
    struct Case { std::string_view cxx, c; };
    const Case cases[] = {
        // clang and its triple-prefixed forms: dropping `++` is correct.
        { "clang++",                     "clang"                     },
        { "aarch64-linux-gnu-clang++",   "aarch64-linux-gnu-clang"   },
        // GCC: a different word, native and prefixed.
        { "g++",                         "gcc"                       },
        { "x86_64-w64-mingw32-g++",      "x86_64-w64-mingw32-gcc"     },
        { "aarch64-linux-musl-g++",      "aarch64-linux-musl-gcc"     },
        // Emscripten: a different word, and the row this test was written for.
        { "em++",                        "emcc"                      },
        // A name that is already a C driver stays itself.
        { "clang",                       "clang"                     },
        { "emcc",                        "emcc"                      },
    };
    for (auto const& c : cases) {
        const std::filesystem::path cxx =
            std::filesystem::path("/p/bin") / std::string(c.cxx);
        EXPECT_EQ(mcpp::toolchain::derive_c_compiler_path(cxx).filename().string(),
                  std::string(c.c)) << c.cxx;
    }

    // AND THE EXECUTABLE SUFFIX SURVIVES, which is the part a stem-based
    // rewrite loses: on Windows the payload ships `g++.exe`, and a C compile
    // spawning `gcc` with no suffix finds nothing.
    EXPECT_EQ(mcpp::toolchain::derive_c_compiler_path("/p/bin/g++.exe")
                  .filename().string(), "gcc.exe");
    EXPECT_EQ(mcpp::toolchain::derive_c_compiler_path("/p/bin/clang++.exe")
                  .filename().string(), "clang.exe");

    // `clang++` MUST NOT MATCH THE `g++` ROW. It ends in `g++`'s last two
    // characters plus nothing, and a suffix test without the separator would
    // turn it into `clanccc` or worse -- a name that is nothing.
    EXPECT_EQ(mcpp::toolchain::derive_c_compiler_path("/p/bin/clang++")
                  .filename().string(), "clang");
}

// ─── One payload, one spelling (B1) ────────────────────────────────────────
//
// `ndk` was accepted as an alias for `android-ndk`, and the capability gate
// refused it -- because that gate compares the DECLARED SPELLING against the
// row's pin (`android-ndk@30.0.16248370`), and `ndk@30.0.16248370` does not
// contain `android-ndk`. So the alias parsed and was then rejected at the
// point of use, which a reader sees as a defect rather than as a naming
// choice: the name was good enough for the parser and not for the build.
//
// Withdrawn rather than completed. Normalising payload names inside the gate
// would make two spellings work and would put the comparison in a second
// mechanism; one name keeps the gate correct by construction. The name kept is
// the one the index uses, so a single string names this payload ecosystem-wide.
//
// The criterion is WHERE the refusal happens, not that one happens.
TEST(SdkPayloads, TheWithdrawnAliasIsRefusedAtParseAndNotByTheCapabilityGate) {
    auto aliased = mcpp::toolchain::parse_toolchain_spec("ndk@30.0.16248370");
    ASSERT_FALSE(aliased.has_value())
        << "an alias that parses is an alias the capability gate must refuse "
           "later, by comparing spellings";
    EXPECT_NE(aliased.error().find("unknown toolchain"), std::string::npos)
        << aliased.error();
    EXPECT_NE(aliased.error().find("'ndk'"), std::string::npos)
        << "the refusal quotes what was typed: " << aliased.error();
    // AND IT SAYS WHICH SPELLING TO USE. A refusal that names the accepted
    // payload is one edit away from a build; one that does not sends the
    // reader to the documentation.
    EXPECT_NE(aliased.error().find("android-ndk"), std::string::npos)
        << aliased.error();

    // The kept spelling is unaffected, and it is the one the row pins.
    auto kept = mcpp::toolchain::parse_toolchain_spec("android-ndk@30.0.16248370");
    ASSERT_TRUE(kept.has_value()) << kept.error();
    EXPECT_EQ(kept->payloadName, "android-ndk");

    // THE MECHANISM THAT NO LONGER HAS TO BE TAUGHT. Both Android rows pin the
    // kept spelling, and the gate's test is `pin's name is a substring of what
    // was declared` -- true for the kept spelling, false for the alias. With
    // the alias gone, the gate never sees the case it would have to special.
    for (auto triple : {"aarch64-linux-android", "x86_64-linux-android"}) {
        auto parsed = mcpp::toolchain::triple::parse(triple);
        ASSERT_TRUE(parsed.has_value()) << triple;
        auto info = mcpp::toolchain::triple::find_known_target(*parsed);
        ASSERT_NE(info, nullptr) << triple;
        const std::string pin(info->pin);
        const std::string pinName = pin.substr(0, pin.find('@'));
        EXPECT_EQ(pinName, "android-ndk") << triple << " pins " << pin;
        EXPECT_EQ(std::string("ndk@30.0.16248370").find(pinName),
                  std::string::npos)
            << "the alias could only pass the gate by a second mechanism";
    }
}
