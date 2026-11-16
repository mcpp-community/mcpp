
import std;
import mcpplibs.mtest;
import mcpplibs.os.process;
import mcpplibs.os.platform;
import mcpp.toolchain.platform;
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;
import mcpp.toolchain.payloads;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

static std::string host_musl() {
    return std::string(mcpp::toolchain::platform::host_arch) + "-linux-musl";
}

// The host-native `musl-gcc` package only exists for Linux hosts; on other
// hosts the payload mapping resolves the triple-named package (a linux-musl
// target from macOS/Windows is cross by definition).
static std::string expected_musl_xim() {
    if constexpr (mcpplibs::os::is_linux) return "musl-gcc";
    else                                    return host_musl() + "-gcc";
}

// Frontend candidates are host-aware for the same reason the mingw ones are:
// they are resolved with filesystem::exists, and on a Windows host the file on
// disk is `<triple>-g++.exe`. The .exe spelling comes first so it wins on a
// case-insensitive filesystem where both would match.
static std::string expected_musl_frontend(const std::string& triple) {
    if constexpr (mcpplibs::os::is_windows) return triple + "-g++.exe";
    else                                      return triple + "-g++";
}

// ── canonical two-axis identity ──────────────────────────────────────────────

UTest maps_gcc_spec_to_gcc_package{"ToolchainRegistry/MapsGccSpecToGccPackage", [] {
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    if (auto&& failed = expect(fatal(spec.has_value())); !failed) failed << spec.error();
    expect(eq(spec->family, Family::Gcc));
    expect(spec->is_host_target());
    expect(eq(spec->spec_str(), "gcc@16.1.0"));
    expect(eq(spec->display(), "gcc@16.1.0"));
    expect(spec->compatHint.empty());

    auto pkg = to_xim_package(*spec);
#if defined(_WIN32)
    // gcc family on a Windows host = MinGW-w64 (the GNU-env host toolchain).
    expect(eq(pkg.ximName, "mingw-gcc"));
#else
    expect(fatal(!pkg.frontendCandidates.empty()));
    if constexpr (mcpplibs::os::is_linux
                  && mcpp::toolchain::platform::host_arch != std::string_view("x86_64")) {
        // Non-x86 Linux native GCC payloads are musl-backed even when the host
        // process uses glibc. Its driver needs no glibc specs fixup.
        expect(eq(pkg.ximName, "musl-gcc"));
        expect(!pkg.needsGccPostInstallFixup);
        expect(eq(pkg.frontendCandidates.front(), host_musl() + "-g++"));
    } else {
        expect(eq(pkg.ximName, "gcc"));
        expect(bool(pkg.needsGccPostInstallFixup));
        expect(eq(pkg.frontendCandidates.front(), "g++"));
    }
#endif
    expect(eq(pkg.ximVersion, "16.1.0"));
    expect(eq(pkg.display_spec(), "gcc@16.1.0"));
}};

UTest legacy_musl_suffix_normalizes_to_musl_target{"ToolchainRegistry/LegacyMuslSuffixNormalizesToMuslTarget", [] {
    // "gcc@15.1.0-musl" — the variant moves out of the version and into the
    // target axis: (gcc, 15.1.0, <host>-linux-musl).
    auto spec = parse_toolchain_spec("gcc@15.1.0-musl");
    if (auto&& failed = expect(fatal(spec.has_value())); !failed) failed << spec.error();
    expect(eq(spec->family, Family::Gcc));
    expect(eq(spec->version, "15.1.0"));
    expect(eq(spec->target.str(), host_musl()));
    expect(!(spec->compatHint.empty()));      // legacy spelling → hint

    auto pkg = to_xim_package(*spec);
    expect(eq(pkg.ximName, expected_musl_xim()));
    expect(eq(pkg.ximVersion, "15.1.0"));
    expect(fatal(!pkg.frontendCandidates.empty()));
    expect(eq(pkg.frontendCandidates.front(), expected_musl_frontend(host_musl())));
    expect(!pkg.needsGccPostInstallFixup);
}};

UTest cross_arch_musl_target_picks_triple_named_package{"ToolchainRegistry/CrossArchMuslTargetPicksTripleNamedPackage", [] {
    // Target arch ≠ host arch → the triple-named cross package.
    ToolchainSpec spec;
    spec.family = Family::Gcc;
    spec.version = "16.1.0";
    spec.target = { "aarch64", "linux", "musl" };
    if (mcpp::toolchain::platform::host_arch == std::string_view("aarch64"))
        spec.target.arch = "riscv64";                 // stay cross on any host

    auto pkg = to_xim_package(spec);
    expect(eq(pkg.ximName, spec.target.str() + "-gcc"));
    expect(fatal(!pkg.frontendCandidates.empty()));
    expect(eq(pkg.frontendCandidates.front(), expected_musl_frontend(spec.target.str())));
    expect(!pkg.needsGccPostInstallFixup);
}};

UTest windows_gnu_target_is_host_split_at_distribution_layer{"ToolchainRegistry/WindowsGnuTargetIsHostSplitAtDistributionLayer", [] {
    // ONE identity (gcc → x86_64-windows-gnu); the payload is host-split:
    // winlibs mingw-gcc on Windows hosts, the Linux-hosted MSVCRT cross
    // elsewhere. "cross" appears only in the xim package name — never in
    // the user-facing spec.
    auto spec = parse_toolchain_spec("gcc@16.1.0");
    expect(fatal(spec.has_value()));
    spec->target = { "x86_64", "windows", "gnu" };

    auto pkg = to_xim_package(*spec);
    expect(eq(pkg.ximVersion, "16.1.0"));
    expect(fatal(!pkg.frontendCandidates.empty()));
#if defined(_WIN32)
    expect(eq(pkg.ximName, "mingw-gcc"));
    expect(eq(pkg.frontendCandidates.front(), "g++.exe"));
#else
    expect(eq(pkg.ximName, "mingw-cross-gcc"));
    expect(eq(pkg.frontendCandidates.front(), "x86_64-w64-mingw32-g++"));
#endif
    expect(!pkg.needsGccPostInstallFixup);
    expect(eq(pkg.display_spec(), "gcc@16.1.0 → x86_64-windows-gnu"));
}};

UTest legacy_mingw_cross_spelling_collapses{"ToolchainRegistry/LegacyMingwCrossSpellingCollapses", [] {
    auto spec = parse_toolchain_spec("mingw-cross@16.1.0");
    if (auto&& failed = expect(fatal(spec.has_value())); !failed) failed << spec.error();
    expect(eq(spec->family, Family::Gcc));
    expect(eq(spec->target.str(), "x86_64-windows-gnu"));
    expect(!(spec->compatHint.empty()));
}};

UTest maps_llvm_and_clang_aliases_to_llvm_package{"ToolchainRegistry/MapsLlvmAndClangAliasesToLlvmPackage", [] {
    auto llvmSpec = parse_toolchain_spec("llvm", "20.1.7");
    auto clangSpec = parse_toolchain_spec("clang@20.1.7");
    if (auto&& failed = expect(fatal(llvmSpec.has_value())); !failed) failed << llvmSpec.error();
    if (auto&& failed = expect(fatal(clangSpec.has_value())); !failed) failed << clangSpec.error();

    expect(eq(llvmSpec->family, Family::Llvm));
    expect(eq(clangSpec->family, Family::Llvm));      // alias family → llvm
    expect(llvmSpec->compatHint.empty());
    expect(!(clangSpec->compatHint.empty()));

    auto llvmPkg = to_xim_package(*llvmSpec);
    auto clangPkg = to_xim_package(*clangSpec);
    expect(eq(llvmPkg.ximName, "llvm"));
    expect(eq(clangPkg.ximName, "llvm"));
    expect(eq(clangPkg.display_spec(), "llvm@20.1.7"));
    expect(fatal(!clangPkg.frontendCandidates.empty()));
#if defined(_WIN32)
    expect(eq(clangPkg.frontendCandidates.front(), "clang++.exe"));
#else
    expect(eq(clangPkg.frontendCandidates.front(), "clang++"));
#endif
}};

UTest resolves_partial_musl_version{"ToolchainRegistry/ResolvesPartialMuslVersion", [] {
    auto spec = parse_toolchain_spec("gcc", "15-musl");
    if (auto&& failed = expect(fatal(spec.has_value())); !failed) failed << spec.error();
    expect(eq(spec->version, "15"));
    expect(eq(spec->target.str(), host_musl()));

    auto resolved = with_resolved_xim_version(*spec, "15.1.0");
    auto pkg = to_xim_package(resolved);
    expect(eq(resolved.version, "15.1.0"));
    expect(eq(pkg.ximName, expected_musl_xim()));
    expect(eq(pkg.ximVersion, "15.1.0"));
    expect(eq(pkg.display_spec(), std::format("gcc@15.1.0 → {}", host_musl())));
}};

UTest rejects_unknown_family{"ToolchainRegistry/RejectsUnknownFamily", [] {
    auto spec = parse_toolchain_spec("tcc@1.0");
    expect(!spec.has_value());
}};

// ── payload reverse mapping ──────────────────────────────────────────────────

UTest identifies_toolchain_payloads_and_skips_others{"ToolchainRegistry/IdentifiesToolchainPayloadsAndSkipsOthers", [] {
    auto gcc = identify_xim_payload("gcc");
    expect(fatal(gcc.has_value()));
    expect(eq(gcc->family, Family::Gcc));
    expect(gcc->target.empty());

    auto musl = identify_xim_payload("musl-gcc");
    expect(fatal(musl.has_value()));
    expect(eq(musl->target.str(), host_musl()));

    auto crossMusl = identify_xim_payload("aarch64-linux-musl-gcc");
    expect(fatal(crossMusl.has_value()));
    expect(eq(crossMusl->target.str(), "aarch64-linux-musl"));

    auto llvm = identify_xim_payload("llvm");
    expect(fatal(llvm.has_value()));
    expect(eq(llvm->family, Family::Llvm));

    // Non-toolchain xpkgs must not be identified (list/doctor filter on this).
    expect(!(identify_xim_payload("ninja").has_value()));
    expect(!(identify_xim_payload("glibc").has_value()));
    expect(!(identify_xim_payload("python").has_value()));
    expect(!(identify_xim_payload("linux-headers").has_value()));
}};

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
// Asked of the payload table (data/builtin.toml) for a host this machine need
// not be, so the aarch64 answer is checkable from an x86_64 machine.
UTest native_gcc_payload_follows_what_the_arch_actually_publishes{"ToolchainRegistry/NativeGccPayloadFollowsWhatTheArchActuallyPublishes", [] {
    namespace payloads = mcpp::toolchain::payloads;
    using mcpp::toolchain::triple::Triple;
    auto gcc = [](std::string os, std::string arch, Triple target) {
        auto r = payloads::choose("gcc", {}, target, payloads::Host{ std::move(os), std::move(arch), "" });
        return r ? r->name : std::string{};
    };
    const Triple none{};

    // x86_64: unchanged — the glibc package is the one that exists.
    expect(eq(gcc("linux", "x86_64", none), "gcc"));
    expect(eq(gcc("linux", "x86_64", {"x86_64", "linux", "gnu"}), "gcc"));

    // aarch64 with no target: musl-gcc, the payload every version publishes.
    expect(eq(gcc("linux", "aarch64", none), "musl-gcc"));
    expect(eq(gcc("linux", "aarch64", {"aarch64", "linux", "musl"}), "musl-gcc"));
    // A target that names glibc asks for the glibc payload (xim:gcc publishes
    // aarch64 since 16.1.0; a Luban system's own gcc is that one).
    expect(eq(gcc("linux", "aarch64", {"aarch64", "linux", "gnu"}), "gcc"));

    // A CROSS target keeps its own payload rule.
    expect(eq(gcc("linux", "aarch64", {"x86_64", "linux", "gnu"}), "gcc"));
    expect(eq(gcc("linux", "x86_64", {"aarch64", "linux", "musl"}), "aarch64-linux-musl-gcc"));

    // MinGW: Windows' own GCC there (also for the machine itself), the cross
    // GCC elsewhere.
    expect(eq(gcc("windows", "x86_64", none), "mingw-gcc"));
    expect(eq(gcc("windows", "x86_64", {"x86_64", "windows", "gnu"}), "mingw-gcc"));
    expect(eq(gcc("linux", "x86_64", {"x86_64", "windows", "gnu"}), "mingw-cross-gcc"));
    expect(eq(gcc("macos", "aarch64", {"x86_64", "windows", "gnu"}), "mingw-cross-gcc"));

    // Non-Linux hosts building for themselves: the glibc-named package.
    expect(eq(gcc("macos", "aarch64", none), "gcc"));
}};

// The reverse reading and the offers come from the same entries.
UTest the_table_reads_both_ways{"ToolchainRegistry/TheTableReadsBothWays", [] {
    namespace payloads = mcpp::toolchain::payloads;
    const payloads::Host linux64{ "linux", "x86_64", "" }, win{ "windows", "x86_64", ".exe" };
    for (auto const& name : { "gcc", "musl-gcc", "mingw-gcc", "mingw-cross-gcc", "llvm", "msvc",
                              "aarch64-linux-musl-gcc" })
        expect(bool(payloads::identify(name, linux64)));
    expect(!payloads::identify("emsdk", linux64));        // its version is the SDK's
    expect(!payloads::identify("ninja", linux64));
    expect(eq(payloads::named("emsdk")->target.str(), "wasm32-emscripten"));

    auto offered = [](const payloads::Host& h) { return payloads::offered(h); };
    auto has = [](const std::vector<std::string>& v, std::string_view n) {
        return std::ranges::find(v, n) != v.end();
    };
    expect(has(offered(linux64), "mingw-cross-gcc") && !has(offered(linux64), "msvc"));
    expect(has(offered(win), "msvc") && has(offered(win), "x86_64-linux-musl-gcc"));
    expect(!has(offered({ "linux", "riscv64", "" }), "llvm"));
    expect(has(offered({ "linux", "aarch64", "" }), "llvm"));

    // A frontend named with the host's suffix.
    auto r = payloads::choose("gcc", {}, {"x86_64", "windows", "gnu"}, win);
    expect(bool(r) && eq(r->frontends.front(), "g++.exe"));
}};

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
UTest the_ndk_api_level_floor_is_read_from_the_payloads_own_metadata{"ToolchainRegistry/TheNdkApiLevelFloorIsReadFromThePayloadsOwnMetadata", [] {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path()
              / ("mcpp-ndk-meta-" + std::to_string(mcpplibs::os::process::self()->id));
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
    expect(eq(mcpp::toolchain::ndk_min_api_level(clangxx), 21));

    // A DIFFERENT PAYLOAD ANSWERS DIFFERENTLY, which is the whole point of
    // reading it: the same code must not return 21 for an NDK that says 24.
    {
        std::ofstream o(root / "meta" / "platforms.json");
        o << R"({"min": 24, "max": 40})";
    }
    expect(eq(mcpp::toolchain::ndk_min_api_level(clangxx), 24));

    // 0 WHEN IT CANNOT BE READ, and the caller turns that into a refusal
    // naming `min_api_level`. A guessed level would be worse than the refusal:
    // it selects which bionic symbols exist, so guessing produces an artefact
    // that links here and fails to load on a device.
    fs::remove(root / "meta" / "platforms.json");
    expect(eq(mcpp::toolchain::ndk_min_api_level(clangxx), 0));

    // Malformed rather than absent -- same answer, and no exception escapes.
    { std::ofstream o(root / "meta" / "platforms.json"); o << "{not json"; }
    expect(eq(mcpp::toolchain::ndk_min_api_level(clangxx), 0));

    // Present but not a number: still 0, never a silent 1 from a cast.
    { std::ofstream o(root / "meta" / "platforms.json"); o << R"({"min": "21"})"; }
    expect(eq(mcpp::toolchain::ndk_min_api_level(clangxx), 0));

    // A path that is not inside an NDK at all walks to the filesystem root and
    // stops; it must not loop.
    expect(eq(mcpp::toolchain::ndk_min_api_level(
                  fs::temp_directory_path() / "definitely-not-an-ndk" / "clang++"), 0));

    fs::remove_all(root);
}};

UTest msvc_is_the_only_family_with_a_system_spelling{"ToolchainOrigin/MsvcIsTheOnlyFamilyWithASystemSpelling", [] {
    auto msvcSystem = parse_toolchain_spec("msvc@system");
    if (auto&& failed = expect(fatal(msvcSystem.has_value())); !failed) failed << msvcSystem.error();
    expect(is_system_toolchain(*msvcSystem));
    expect(eq(origin_of(*msvcSystem), Origin::SystemMsvc));

    // A VERSIONED msvc spec is the other origin — that split is the whole
    // point of the version axis.
    auto pinned = parse_toolchain_spec("msvc@14.44.35207");
    expect(fatal(pinned.has_value()));
    expect(eq(origin_of(*pinned), Origin::Managed));

    auto gcc = parse_toolchain_spec("gcc@16.1.0");
    expect(fatal(gcc.has_value()));
    expect(eq(origin_of(*gcc), Origin::Managed));
}};

UTest non_msvc_system_is_rejected_where_it_is_read_and_offers_the_alternatives{"ToolchainOrigin/NonMsvcSystemIsRejectedWhereItIsReadAndOffersTheAlternatives", [] {
    // It used to parse, and then fail somewhere else entirely as
    // `xim:gcc@system` → "no such package" — which sends the reader looking
    // for a version that was never going to exist. The error has to name both
    // things the user might have meant.
    for (auto spec : {"gcc@system", "llvm@system"}) {
        auto r = parse_toolchain_spec(spec);
        expect(fatal(!r.has_value())) << spec << " was accepted";
        if (auto&& failed = expect(ne(r.error().find("only msvc"), std::string::npos)); !failed) failed << r.error();
        if (auto&& failed = expect(ne(r.error().find("@<version>"), std::string::npos)); !failed) failed << "the pin alternative is not offered: " << r.error();
        // The family-less PATH escape hatch is the OTHER thing they might
        // have wanted, and it is a different mechanism.
        expect(ne(r.error().find("PATH compiler"), std::string::npos))
            << "the escape hatch is not offered: " << r.error();
        if (auto&& failed = expect(eq(r.error().find("xim:"), std::string::npos)); !failed) failed << "still leaking the package spelling that cannot exist: "
            << r.error();
    }
}};

// ─── An SDK payload is chosen by the TARGET, and knows its own layout ──────
//
// `to_xim_package` returned the generic llvm payload for every `Family::Llvm`
// spec, which is right for the targets llvm itself serves and wrong for the two
// that arrive with their own clang. `em++` and the NDK's `clang++` ARE clang --
// same family, same flag vocabulary -- so no fourth `Family` value exists;
// what changes is which package answers and where its driver lives.
UTest the_target_chooses_the_package_and_the_package_knows_its_layout{"SdkPayloads/TheTargetChoosesThePackageAndThePackageKnowsItsLayout", [] {
    auto pkg_for = [](std::string_view target) {
        // A SPELLING THAT NAMES NO PAYLOAD, WHICH IS WHAT THIS TEST IS ABOUT.
        //
        // This fixture built every case from `emsdk@6.0.9` and replaced only
        // the target, which worked while the target was the ONLY thing read --
        // and then `payloadName` began to decide first (correctly: a spec that
        // names a payload has answered this question), and the Android cases
        // resolved the emsdk payload from a spelling that says emsdk.
        //
        // The fixture had picked its object by convenience. `llvm@22.1.8`
        // names the family and no payload, which is the shape a build has when
        // it passes `--target` and declares nothing -- the shape whose
        // resolution this test exists to state.
        auto spec = mcpp::toolchain::parse_toolchain_spec("llvm@22.1.8");
        auto s = *spec;
        if (auto t = mcpp::toolchain::triple::parse(target)) s.target = *t;
        return mcpp::toolchain::to_xim_package(s);
    };

    {   // Emscripten: `em++` is a wrapper in `emscripten/`, NOT the raw clang
        // in `bin/`. Naming the wrapper is the whole point -- `bin/clang`
        // compiles for wasm and then links like an ordinary clang, producing a
        // module with none of Emscripten's JavaScript glue.
        auto pkg = pkg_for("wasm32-emscripten");
        expect(eq(pkg.ximName, "emsdk"));
        expect(eq(pkg.frontendSubdir, "emscripten"));
        expect(fatal(!pkg.frontendCandidates.empty()));
        expect(eq(pkg.frontendCandidates.front(), "em++"));
    }
    {   // Android: ONE payload for both arches -- the arch arrives as
        // `--target=<arch>-linux-android<api>`, not as a different package --
        // and the host tuple in the path is the HOST's, not the target's.
        for (auto target : {"aarch64-linux-android", "x86_64-linux-android"}) {
            auto pkg = pkg_for(target);
            expect(eq(pkg.ximName, "android-ndk")) << target;
            expect(ne(pkg.frontendSubdir.find("toolchains/llvm/prebuilt/"), std::string::npos)) << target;
            // The HOST, so an aarch64 Linux machine cross-compiling still
            // reads `linux-x86_64`. Asserted as "not the target's arch" rather
            // than against a literal, so this test says the same thing on
            // every runner.
            expect(eq(pkg.frontendSubdir.find("aarch64-linux-android"), std::string::npos)) << target;
        }
    }
    {   // And every other target still gets the generic llvm payload in bin/.
        auto pkg = pkg_for("x86_64-linux-gnu");
        expect(ne(pkg.ximName, "emsdk"));
        expect(ne(pkg.ximName, "android-ndk"));
        expect(eq(pkg.frontendSubdir, "bin"));
    }
}};

// THE MESSAGE MUST NAME THE DIRECTORY THAT WAS SEARCHED.
//
// Five refusals printed `payload->binDir`, the directory they had composed
// themselves. Once the package decides where its frontend lives, `bin` is a
// directory nothing looked in -- and this codebase's most frequent defect is a
// fixed lookup with an unfixed message.
UTest the_searched_directory_is_available_for_the_diagnostic{"SdkPayloads/TheSearchedDirectoryIsAvailableForTheDiagnostic", [] {
    auto spec = mcpp::toolchain::parse_toolchain_spec("emsdk@6.0.9");
    expect(fatal(spec.has_value()));
    auto pkg = mcpp::toolchain::to_xim_package(*spec);
    auto dir = mcpp::toolchain::payload_frontend_dir("/p/xim-x-emsdk/6.0.9", pkg);
    expect(eq(dir, std::filesystem::path("/p/xim-x-emsdk/6.0.9/emscripten")));
}};

// AN SDK IS SERVED WHERE THE SDK IS PUBLISHED, and the first version of this
// said "wherever" -- the same over-broad shape as the branches it sits above.
// The target matrix caught it: declaring the row servable on macOS and Windows
// would have claimed a payload that does not exist there.
UTest served_on_every_host_the_sdk_is_published_for{"SdkPayloads/ServedOnEveryHostTheSdkIsPublishedFor", [] {
    // THIS ASSERTION USED TO BE TRUE BY ARITHMETIC ON ONE HOST.
    //
    // It read `EXPECT_EQ(host_can_serve(*wasm), mcpplibs::os::is_linux)`,
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
        expect(fatal(t.has_value())) << name;
        expect(mcpp::toolchain::host_can_serve(*t)) << name;
    }

    // AND THE PREDICATE IS STILL ABLE TO SAY NO, which is what keeps the
    // paragraph above from being a tautology. macOS's SDK and MSVC are
    // host-only and no package substitutes for either, so a Linux host cannot
    // serve them -- the exclusion this function exists to make.
    if constexpr (mcpplibs::os::is_linux) {
        auto mac = mcpp::toolchain::triple::parse("aarch64-macos");
        expect(fatal(mac.has_value()));
        expect(!mcpp::toolchain::host_can_serve(*mac));
    }

    // AND APPLE IS ONE ANSWER FOR BOTH OF ITS PLATFORMS, which this function
    // did not say until there were two.
    //
    // It asked `os == "macos"`, the same question as "is this Apple" while
    // macOS was the only Apple target mcpp had. An iOS target then fell
    // through to the final `return false` -- on EVERY host, including the one
    // that serves it. The consequence was not a refused build (the SDK gate in
    // prepare answers first) but a `toolchain list` that dropped the three iOS
    // rows on macOS while `graphCouldServe` listed them on linux-x86_64, and a
    // target-matrix scan that reached three fewer cells than the table
    // declares.
    //
    // ASSERTED IN BOTH DIRECTIONS, because one of them is what the earlier
    // paragraph in this test calls an exclusion that must not become a
    // tautology: every Apple row is servable exactly where the SDK is.
    for (auto name : {"aarch64-macos", "aarch64-ios", "aarch64-ios-sim",
                      "x86_64-ios-sim"}) {
        auto t = mcpp::toolchain::triple::parse(name);
        expect(fatal(t.has_value())) << name;
        expect(t->is_apple()) << name;
        expect(eq(mcpp::toolchain::host_can_serve(*t), bool(mcpplibs::os::is_macos))) << name;
    }
}};

// ─── The payload is SAID, not only resolved (R3) ───────────────────────────
//
// `emsdk@6.0.9` normalises to the llvm family because `em++` IS clang, and a
// fourth family value would be a false claim about the compiler. The
// consequence was `Resolved llvm@6.0.9` -- indistinguishable from the real
// `xim:llvm`, and not what the user typed. The family and the payload are two
// questions, and `to_xim_package` already answered the second; this is the
// field that lets it be printed.
UTest the_display_names_the_payload_and_not_only_the_family{"SdkPayloads/TheDisplayNamesThePayloadAndNotOnlyTheFamily", [] {
    auto em = mcpp::toolchain::parse_toolchain_spec("emsdk@6.0.9");
    expect(fatal(em.has_value()));
    expect(eq(em->family, mcpp::toolchain::Family::Llvm))
        << "em++ is clang; a fourth family would be a false claim";
    expect(eq(em->payloadName, "emsdk"));
    if (auto&& failed = expect(ne(em->display().find("emsdk@6.0.9"), std::string::npos)); !failed) failed << em->display();
    if (auto&& failed = expect(eq(em->display().find("llvm@"), std::string::npos)); !failed) failed << em->display();

    auto ndk = mcpp::toolchain::parse_toolchain_spec("android-ndk@30.0.16248370");
    expect(fatal(ndk.has_value()));
    expect(eq(ndk->family, mcpp::toolchain::Family::Llvm));
    expect(eq(ndk->payloadName, "android-ndk"));
    if (auto&& failed = expect(ne(ndk->display().find("android-ndk@"), std::string::npos)); !failed) failed << ndk->display();

    // AND NOTHING ELSE MOVES. Empty `payloadName` means the family's own
    // payload, which is every row but these two -- so no existing output
    // changes, which is what makes this additive.
    for (auto spelled : {"llvm@22.1.8", "gcc@16.1.0", "msvc@14.44.35207"}) {
        auto sp = mcpp::toolchain::parse_toolchain_spec(spelled);
        expect(fatal(sp.has_value())) << spelled;
        expect(sp->payloadName.empty()) << spelled;
        if (auto&& failed = expect(ne(sp->display().find(spelled), std::string::npos)); !failed) failed << sp->display();
    }
}};

// ─── A spec that names a payload has answered which payload ───────────────
//
// `to_xim_package` decided the payload from the TARGET, which is right for the
// spelling a build uses -- `--target wasm32-emscripten` with nothing declared
// -- and wrong for the spelling an install uses, where there is no target at
// all. Measured:
//
//   $ mcpp toolchain install emsdk 6.0.9
//     error: installed package has no known C++ frontend in
//            '.../xim-x-emsdk/6.0.9/bin'
//
// The archive was fetched correctly and then searched for `clang++` in `bin/`,
// because `frontendSubdir` had been decided by a target nobody gave. Both SDK
// payloads had it; neither `mcpp toolchain install emsdk` nor
// `mcpp toolchain install android-ndk` had ever worked.
//
// THE DENOMINATOR IS BOTH PAYLOADS AND BOTH SPELLINGS, because the defect is
// in the AXIS and not in one row: a payload named with a target, and the same
// payload named without one, must answer identically.
UTest the_payload_the_spec_named_decides_without_a_target{"SdkPayloads/ThePayloadTheSpecNamedDecidesWithoutATarget", [] {
    struct Case {
        std::string_view spelling, ximName, subdir, frontend, target;
    };
    const Case cases[] = {
        { "emsdk@6.0.9", "emsdk", "emscripten", "em++", "" },
        { "emsdk@6.0.9", "emsdk", "emscripten", "em++", "wasm32-emscripten" },
        { "android-ndk@30.0.16248370", "android-ndk",
          "toolchains/llvm/prebuilt", "clang++", "" },
        { "android-ndk@30.0.16248370", "android-ndk",
          "toolchains/llvm/prebuilt", "clang++", "aarch64-linux-android" },
    };
    for (auto const& c : cases) {
        auto spec = mcpp::toolchain::parse_toolchain_spec(std::string(c.spelling));
        expect(fatal(spec.has_value())) << c.spelling;
        if (!c.target.empty())
            if (auto t = mcpp::toolchain::triple::parse(c.target)) spec->target = *t;
        auto pkg = mcpp::toolchain::to_xim_package(*spec);
        const std::string where = std::string(c.spelling) + " target='"
                                + std::string(c.target) + "'";
        expect(eq(pkg.ximName, c.ximName)) << where;
        expect(ne(pkg.frontendSubdir.find(c.subdir), std::string::npos))
            << where << " subdir=" << pkg.frontendSubdir;
        // The candidate list names the payload's own driver rather than the
        // family's, which is the half that sent the install looking for
        // `clang++` in the emsdk payload.
        bool named = false;
        for (auto const& cand : pkg.frontendCandidates)
            if (cand.find(c.frontend) != std::string::npos) named = true;
        expect(bool(named)) << where << " candidates do not name " << c.frontend;
    }

    // AND A SPEC THAT NAMES NO PAYLOAD STILL READS THE TARGET. This is the
    // other half of the axis: `llvm@22.1.8` with an Android target is the
    // escape-hatch spelling, and it must resolve the generic llvm payload
    // rather than the NDK -- the capability gate is what refuses it, and a
    // gate cannot refuse what was silently rewritten.
    auto plain = mcpp::toolchain::parse_toolchain_spec("llvm@22.1.8");
    expect(fatal(plain.has_value()));
    expect(plain->payloadName.empty());
    if (auto t = mcpp::toolchain::triple::parse("aarch64-macos")) plain->target = *t;
    expect(eq(mcpp::toolchain::to_xim_package(*plain).ximName, "llvm"));
}};

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
UTest every_frontend_names_the_c_compiler_beside_it{"SdkPayloads/EveryFrontendNamesTheCCompilerBesideIt", [] {
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
        expect(eq(mcpp::toolchain::derive_c_compiler_path(cxx).filename().string(), std::string(c.c))) << c.cxx;
    }

    // AND THE EXECUTABLE SUFFIX SURVIVES, which is the part a stem-based
    // rewrite loses: on Windows the payload ships `g++.exe`, and a C compile
    // spawning `gcc` with no suffix finds nothing.
    expect(eq(mcpp::toolchain::derive_c_compiler_path("/p/bin/g++.exe")
                  .filename().string(), "gcc.exe"));
    expect(eq(mcpp::toolchain::derive_c_compiler_path("/p/bin/clang++.exe")
                  .filename().string(), "clang.exe"));

    // `clang++` MUST NOT MATCH THE `g++` ROW. It ends in `g++`'s last two
    // characters plus nothing, and a suffix test without the separator would
    // turn it into `clanccc` or worse -- a name that is nothing.
    expect(eq(mcpp::toolchain::derive_c_compiler_path("/p/bin/clang++")
                  .filename().string(), "clang"));
}};

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
UTest the_withdrawn_alias_is_refused_at_parse_and_not_by_the_capability_gate{"SdkPayloads/TheWithdrawnAliasIsRefusedAtParseAndNotByTheCapabilityGate", [] {
    auto aliased = mcpp::toolchain::parse_toolchain_spec("ndk@30.0.16248370");
    expect(fatal(!aliased.has_value()))
        << "an alias that parses is an alias the capability gate must refuse "
           "later, by comparing spellings";
    if (auto&& failed = expect(ne(aliased.error().find("unknown toolchain"), std::string::npos)); !failed) failed << aliased.error();
    if (auto&& failed = expect(ne(aliased.error().find("'ndk'"), std::string::npos)); !failed) failed << "the refusal quotes what was typed: " << aliased.error();
    // AND IT SAYS WHICH SPELLING TO USE. A refusal that names the accepted
    // payload is one edit away from a build; one that does not sends the
    // reader to the documentation.
    expect(ne(aliased.error().find("android-ndk"), std::string::npos))
        << aliased.error();

    // The kept spelling is unaffected, and it is the one the row pins.
    auto kept = mcpp::toolchain::parse_toolchain_spec("android-ndk@30.0.16248370");
    if (auto&& failed = expect(fatal(kept.has_value())); !failed) failed << kept.error();
    expect(eq(kept->payloadName, "android-ndk"));

    // THE MECHANISM THAT NO LONGER HAS TO BE TAUGHT. Both Android rows pin the
    // kept spelling, and the gate's test is `pin's name is a substring of what
    // was declared` -- true for the kept spelling, false for the alias. With
    // the alias gone, the gate never sees the case it would have to special.
    for (auto triple : {"aarch64-linux-android", "x86_64-linux-android"}) {
        auto parsed = mcpp::toolchain::triple::parse(triple);
        expect(fatal(parsed.has_value())) << triple;
        auto info = mcpp::toolchain::triple::find_known_target(*parsed);
        expect(fatal(ne(info, nullptr))) << triple;
        const std::string pin(info->pin);
        const std::string pinName = pin.substr(0, pin.find('@'));
        expect(eq(pinName, "android-ndk")) << triple << " pins " << pin;
        expect(eq(std::string("ndk@30.0.16248370").find(pinName), std::string::npos))
            << "the alias could only pass the gate by a second mechanism";
    }
}};

// ── A required family's version comes from pins of the same payload (#641) ──
//
// `resolve_required_family` falls back to the versions the target rows pin when
// nothing is installed. Those pins include `android-ndk@...` and `emsdk@...`,
// which are llvm-family pins of other payloads. Compared by family, the highest
// "llvm" pin was the NDK's, and a requirement on `emsdk` or `android-ndk`
// matched nothing. The expectations are read from the row table itself, so the
// test follows the table when a pin moves.
namespace {
// The answer `resolve_required_family` takes: the highest of the pinned
// versions, chosen by the same function.
std::string highest_pin(std::string_view spelling) {
    auto spec = parse_toolchain_spec(std::string(spelling));
    if (!spec) return "unparsed: " + spec.error();
    return mcpp::toolchain::resolve_version_match("", pinned_versions_for(*spec))
        .value_or("");
}

std::string pinned_row_version(std::string_view payload) {
    for (auto const& row : triple::known_targets()) {
        std::string_view pin = row.pin;
        if (pin.starts_with(std::string(payload) + "@"))
            return std::string(pin.substr(payload.size() + 1));
    }
    return "";
}
} // namespace

UTest each_family_takes_the_pins_of_its_own_payload{"RequiredFamilyPins/EachFamilyTakesThePinsOfItsOwnPayload", [] {
    for (auto payload : {"llvm", "emsdk", "android-ndk", "gcc"}) {
        const auto expected = pinned_row_version(payload);
        expect(fatal(!expected.empty())) << "no row pins " << payload;
        expect(eq(highest_pin(payload), expected)) << payload;
    }
}};

UTest no_llvm_pin_comes_from_another_payload{"RequiredFamilyPins/NoLlvmPinComesFromAnotherPayload", [] {
    auto spec = parse_toolchain_spec("llvm");
    if (auto&& failed = expect(fatal(spec.has_value())); !failed) failed << spec.error();
    const auto versions = pinned_versions_for(*spec);
    expect(!versions.empty());
    for (auto const& v : versions) {
        expect(ne(v, pinned_row_version("android-ndk")));
        expect(ne(v, pinned_row_version("emsdk")));
    }
}};
