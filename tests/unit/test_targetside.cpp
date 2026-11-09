// Target-side resolution: which layer comes from where.
//
// These tests exist because the decision they cover used to be three
// independent derivations with three different criteria, and the bug was that
// they disagreed on a case none of them had been written for: a C program whose
// system comes from the dependency graph. So the assertions below are about the
// TABLE's totality — every combination the ecosystem can produce gets a row,
// including the ones that were unrepresentable before.
//
// The module under test is a pure function of plain data on purpose. The
// capability it replaces (`hosted-standard-library`) drove seven behaviours
// from inside a 7000-line translation unit and had zero test coverage, because
// asserting it required running a whole build. Nothing here runs a build.


import std;
import mcpplibs.mtest;
import mcpp.targetside;

using namespace mcpplibs::mtest;

namespace ts = mcpp::targetside;

namespace {

// The traditional stack: a payload supplies every layer.
ts::Inputs payload_linux() {
    ts::Inputs in;
    in.llvmTriple          = "x86_64-unknown-linux-gnu";
    in.targetOs            = "linux";
    in.targetEnv           = "gnu";
    in.payloadSystemRef    = "xim-x-linux-headers@5.11.1";
    in.payloadLibcRef      = "xim-x-glibc@2.44";
    in.payloadCxxRef       = "xim-x-llvm@22.1.8";
    in.payloadCxxInterface = "libc++";
    return in;
}

ts::Provider provider(std::string name, std::string version,
                      std::string iface, bool stdModule = false) {
    ts::Provider p;
    p.name          = std::move(name);
    p.version       = std::move(version);
    p.interfaceName = std::move(iface);
    p.hasStdModule  = stdModule;
    return p;
}

} // namespace

// ── The capability grammar ───────────────────────────────────────────────────

UTest three_layer_names_are_accepted{"TargetSideCapability/ThreeLayerNamesAreAccepted", [] {
    for (auto [entry, layer] : std::initializer_list<
             std::pair<std::string_view, ts::CapLayer>>{
             {"mcpp:kernel-abi", ts::CapLayer::KernelAbi},
             {"mcpp:c-abi",      ts::CapLayer::CAbi},
             {"mcpp:c++-abi",    ts::CapLayer::CxxAbi}}) {
        auto r = ts::parse_capability(entry);
        expect(fatal(r.has_value())) << entry;
        expect(fatal(r->has_value())) << entry;
        expect(eq((*r)->layer, layer)) << entry;
        expect((*r)->interfaceName.empty()) << entry;
    }
}};

UTest interface_name_is_carried_by_the_declaration{"TargetSideCapability/InterfaceNameIsCarriedByTheDeclaration", [] {
    auto r = ts::parse_capability("mcpp:kernel-abi=openkal");
    expect(fatal(r.has_value()));
    expect(fatal(r->has_value()));
    expect(eq((*r)->layer, ts::CapLayer::KernelAbi));
    expect(eq((*r)->interfaceName, "openkal"));
}};

// The engine must not reject names it does not own. `provides` serves the
// feature system too, and a closed set over the whole array would break the
// allocator selection that already ships.
UTest names_outside_the_reserved_prefix_pass_through{"TargetSideCapability/NamesOutsideTheReservedPrefixPassThrough", [] {
    for (std::string_view entry : {"freestanding-allocator",
                                   "hosted-standard-library",
                                   "anything-a-package-invents"}) {
        auto r = ts::parse_capability(entry);
        expect(fatal(r.has_value())) << entry;
        expect(!r->has_value()) << entry;
    }
}};

// And the point of having a prefix at all: inside it, a typo is an error rather
// than a silently disabled behaviour.
UTest unknown_name_inside_the_reserved_prefix_is_an_error{"TargetSideCapability/UnknownNameInsideTheReservedPrefixIsAnError", [] {
    for (std::string_view entry : {"mcpp:kernel_abi",   // underscore, not hyphen
                                   "mcpp:cabi",
                                   "mcpp:c++abi",
                                   "mcpp:target-system"}) {
        auto r = ts::parse_capability(entry);
        expect(!r.has_value()) << entry;
        if (!r.has_value())
            expect(ne(r.error().find("mcpp:kernel-abi"), std::string::npos))
                << "the diagnostic must list the layers that do exist";
    }
}};

// ── The resolution table ─────────────────────────────────────────────────────

UTest traditional_stack_takes_every_layer_from_the_payload{"TargetSideResolve/TraditionalStackTakesEveryLayerFromThePayload", [] {
    auto r = ts::resolve(payload_linux());
    expect(eq(r.kernelAbi.origin, ts::Origin::Payload));
    expect(eq(r.cAbi.origin, ts::Origin::Payload));
    expect(eq(r.cxx.origin, ts::Origin::Payload));
    expect(eq(r.kernelAbi.interfaceName, "linux"));
    expect(eq(r.cAbi.interfaceName, "glibc"));
    expect(eq(r.cxx.interfaceName, "libc++"));
    expect(!r.system_from_graph());
}};

UTest openkal_cxx_takes_every_layer_from_the_graph{"TargetSideResolve/OpenkalCxxTakesEveryLayerFromTheGraph", [] {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-macos14.0";
    in.targetOs   = "macos";
    in.kernelAbi  = provider("openkal-macos", "0.3.1", "openkal");
    in.cAbi       = provider("openkal-musl", "0.3.1", "musl");
    in.cxxAbi     = provider("openkal-llvm-runtime", "0.1.0", "libc++", /*stdModule=*/true);

    auto r = ts::resolve(in);
    expect(eq(r.kernelAbi.origin, ts::Origin::Graph));
    expect(eq(r.cAbi.origin, ts::Origin::Graph));
    expect(eq(r.cxx.origin, ts::Origin::Graph));
    expect(eq(r.kernelAbi.impl, "openkal-macos@0.3.1"));
    expect(eq(r.cAbi.impl, "openkal-musl@0.3.1"));
    expect(!r.cxx.subset) << "a package that declares a std module supplies the whole library";
    expect(bool(r.system_from_graph()));
}};

// THE CASE THAT WAS UNREPRESENTABLE, AND THE MEASURED DEFECT.
//
// A C program over openkal has a kernel ABI and a C library from the graph and
// no C++ runtime at all. Before this module the absence of the third layer was
// read as "the target side is not from the graph", the payload's own libc++
// stayed on the link line, and a macOS cross ended in
// `libc++.so: unhandled file type`.
UTest pure_c_over_openkal_still_has_its_system_from_the_graph{"TargetSideResolve/PureCOverOpenkalStillHasItsSystemFromTheGraph", [] {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-macos14.0";
    in.targetOs   = "macos";
    in.kernelAbi  = provider("openkal-macos", "0.3.1", "openkal");
    in.cAbi       = provider("openkal-musl", "0.3.1", "musl");
    // no cxxAbi provider

    auto r = ts::resolve(in);
    expect(eq(r.cxx.origin, ts::Origin::None))
        << "a C program needs no C++ runtime, and that is not a failure to find one";
    expect(bool(r.system_from_graph()))
        << "the system still comes from the graph, which is the whole defect";
    expect(ne(r.cxx.origin, ts::Origin::Payload))
        << "the payload's C++ runtime must not be selected over a graph C library";
}};

// Only the platform implementation, and nothing above it.
UTest raw_openkal_has_only_a_kernel_abi{"TargetSideResolve/RawOpenkalHasOnlyAKernelAbi", [] {
    auto in = payload_linux();
    in.kernelAbi           = provider("openkal-linux", "0.5.1", "openkal");
    in.sysrootDeclaredEmpty = true;

    auto r = ts::resolve(in);
    expect(eq(r.kernelAbi.origin, ts::Origin::Graph));
    expect(bool(r.cAbi.absent()));
    expect(bool(r.cxx.absent()));
}};

UTest bare_metal_with_picolibc_has_no_kernel_abi{"TargetSideResolve/BareMetalWithPicolibcHasNoKernelAbi", [] {
    ts::Inputs in;
    in.llvmTriple          = "riscv64-none-elf";
    in.freestandingTarget  = true;
    in.sysrootXpkg         = "xim:picolibc-riscv@1.8.12";

    auto r = ts::resolve(in);
    expect(bool(r.kernelAbi.absent()))
        << "a bare machine has no kernel, and saying so is the information";
    expect(eq(r.cAbi.origin, ts::Origin::Xpkg));
    // The package name, not a prettier form of it. Stripping the `-riscv`
    // suffix would mean the engine knows how this ecosystem names its
    // packages, which is exactly the knowledge this design keeps out of it.
    expect(eq(r.cAbi.interfaceName, "picolibc-riscv"));
    expect(eq(r.cAbi.impl, "xim:picolibc-riscv@1.8.12"));
    expect(bool(r.cxx.absent()));
    expect(!r.system_from_graph());
}};

// THE MIXED CASE. It already ships, and no single boolean can express it.
UTest prebuilt_c_library_under_a_graph_supplied_cxx_subset{"TargetSideResolve/PrebuiltCLibraryUnderAGraphSuppliedCxxSubset", [] {
    ts::Inputs in;
    in.llvmTriple         = "riscv64-none-elf";
    in.freestandingTarget = true;
    in.sysrootXpkg        = "xim:picolibc-riscv@1.8.12";
    in.cxxAbi             = provider("std-freestanding", "0.5.0", "freestanding subset");

    auto r = ts::resolve(in);
    expect(eq(r.cAbi.origin, ts::Origin::Xpkg))  << "prebuilt";
    expect(eq(r.cxx.origin, ts::Origin::Graph)) << "composed at build time";
    expect(bool(r.cxx.subset)) << "no std module declared, so the library is a subset";
}};

// A hosted target whose C library is the payload's (a located SDK, or glibc)
// while a package supplies the whole standard library: the iOS rows over
// `llvm.libcxx` (mcpp#630). The C++ layer is the graph's and is not a subset,
// and the C library stays the payload's; nothing about one decides the other.
UTest prebuilt_c_library_under_a_graph_supplied_libcxx{"TargetSideResolve/PrebuiltCLibraryUnderAGraphSuppliedLibcxx", [] {
    auto in = payload_linux();
    in.cxxAbi = provider("libcxx", "22.1.8.1", "libc++", /*stdModule=*/true);

    auto r = ts::resolve(in);
    expect(eq(r.cAbi.origin, ts::Origin::Payload)) << "the SDK's or glibc's, prebuilt";
    expect(bool(r.cAbi.prebuilt()));
    expect(eq(r.cxx.origin, ts::Origin::Graph));
    expect(bool(r.cxx.fromGraph()));
    expect(!r.cxx.subset) << "a std module is declared, so this is the whole library";
    expect(!r.system_from_graph()) << "the system is still the payload's";
}};

// The payload has no compiler runtime for this platform and the graph
// declares none: the layer is absent, and the report says so rather than
// naming the compiler's family for an archive that does not exist. With a
// graph provider the same input resolves to the graph.
UTest a_payload_without_a_compiler_runtime_for_the_platform{"TargetSideResolve/APayloadWithoutACompilerRuntimeForThePlatform", [] {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-ios18.0-simulator";
    in.targetOs   = "ios";
    in.targetEnv  = "sim";
    in.compilerFamily = "llvm";
    in.payloadCompilerRuntimeAbsent = true;

    auto absent = ts::resolve(in);
    expect(bool(absent.compilerRuntime.absent()));

    in.compilerRuntime = provider("compiler-rt-builtins", "22.1.8.3", "compiler-rt");
    auto supplied = ts::resolve(in);
    expect(eq(supplied.compilerRuntime.origin, ts::Origin::Graph));
    expect(!supplied.compilerRuntime.absent());

    // Negative direction: a payload that has the archive reports its own.
    in.compilerRuntime.reset();
    in.payloadCompilerRuntimeAbsent = false;
    auto own = ts::resolve(in);
    expect(eq(own.compilerRuntime.origin, ts::Origin::Payload));
}};

UTest zero_libc_tier_has_nothing_at_all{"TargetSideResolve/ZeroLibcTierHasNothingAtAll", [] {
    ts::Inputs in;
    in.llvmTriple           = "x86_64-none-elf";
    in.freestandingTarget   = true;
    in.sysrootDeclaredEmpty = true;

    auto r = ts::resolve(in);
    expect(bool(r.kernelAbi.absent()));
    expect(bool(r.cAbi.absent()));
    expect(bool(r.cxx.absent()));
}};

// ── The layering rule ────────────────────────────────────────────────────────
//
// An implementation must have been configured for the layer beneath it. The
// resolver enforces it by construction; this asserts that the construction
// really does, rather than that a later check catches it.
UTest payload_cxx_is_not_selected_over_a_non_payload_c_library{"TargetSideResolve/PayloadCxxIsNotSelectedOverANonPayloadCLibrary", [] {
    for (auto make : std::initializer_list<std::function<ts::Inputs()>>{
             [] { auto in = payload_linux(); in.sysrootXpkg = "xim:picolibc-riscv@1.8.12"; return in; },
             [] { auto in = payload_linux();
                  in.cAbi = provider("openkal-musl", "0.3.1", "musl"); return in; }}) {
        auto r = ts::resolve(make());
        expect(ne(r.cxx.origin, ts::Origin::Payload));
        expect(eq(ts::check_layering(r), std::nullopt))
            << "the default path must not be able to construct the violation";
    }
}};

UTest the_layering_rule_is_stated_for_the_override_path{"TargetSideResolve/TheLayeringRuleIsStatedForTheOverridePath", [] {
    ts::TargetSide bad;
    bad.cAbi = { ts::Origin::Graph,   "musl",   "openkal-musl@0.3.1", false };
    bad.cxx  = { ts::Origin::Payload, "libc++", "xim-x-llvm@22.1.8",  false };

    auto why = ts::check_layering(bad);
    expect(fatal(why.has_value()));
    expect(ne(why->find("never configured for this one"), std::string::npos))
        << "the diagnostic must state the reason, not merely the combination";
}};

// ── The report ───────────────────────────────────────────────────────────────

UTest shows_the_translated_triple_when_it_differs{"TargetSideReport/ShowsTheTranslatedTripleWhenItDiffers", [] {
    auto in = payload_linux();
    in.llvmTriple = "arm64-apple-macos14.0";
    auto text = ts::format_report(ts::resolve(in), "aarch64-macos");
    expect(ne(text.find("aarch64-macos → arm64-apple-macos14.0"), std::string::npos))
        << "the translation is load bearing: the untranslated form emits a Mach-O "
           "whose MinVersion carries no platform";
}};

UTest absent_layers_read_as_a_dash_rather_than_being_omitted{"TargetSideReport/AbsentLayersReadAsADashRatherThanBeingOmitted", [] {
    ts::Inputs in;
    in.llvmTriple           = "x86_64-none-elf";
    in.freestandingTarget   = true;
    in.sysrootDeclaredEmpty = true;
    in.compilerFamily       = "llvm";
    auto text = ts::format_report(ts::resolve(in), "x86_64-none-elf",
                                  /*verbose=*/true);
    expect(ne(text.find("kernel-abi"), std::string::npos));
    expect(ne(text.find("c-abi"), std::string::npos));
    expect(ne(text.find("c++-abi"), std::string::npos));
    expect(ne(text.find("—"), std::string::npos));
}};

UTest names_interface_and_implementation_separately{"TargetSideReport/NamesInterfaceAndImplementationSeparately", [] {
    auto in = payload_linux();
    in.kernelAbi = provider("openkal-opensbi", "0.1.0", "openkal");
    auto text = ts::format_report(ts::resolve(in), "riscv64-none-elf");
    expect(ne(text.find("openkal"), std::string::npos));
    expect(ne(text.find("openkal-opensbi@0.1.0"), std::string::npos))
        << "four packages answer to one interface name; collapsing them would "
           "hide why one source reaches four machines";
    expect(ne(text.find("graph"), std::string::npos));
}};

// ── What earns a line ────────────────────────────────────────────────────────
//
// A zero-configuration build resolves every layer from one compiler payload,
// and five lines reading `(payload)` answer a question nobody asked. What earns
// a line is a layer that came from somewhere else.
UTest a_zero_configuration_build_prints_only_the_target{"TargetSideReport/AZeroConfigurationBuildPrintsOnlyTheTarget", [] {
    auto in = payload_linux();
    in.compilerFamily  = "llvm";
    in.compilerVersion = "22.1.8";
    auto text = ts::format_report(ts::resolve(in), "x86_64-linux-gnu");
    expect(eq(text.find('\n'), std::string::npos))
        << "every layer came from the payload, so none of them is news:\n" << text;
}};

UTest verbose_prints_all_five_including_the_payload_ones{"TargetSideReport/VerbosePrintsAllFiveIncludingThePayloadOnes", [] {
    auto in = payload_linux();
    in.compilerFamily  = "llvm";
    in.compilerVersion = "22.1.8";
    auto text = ts::format_report(ts::resolve(in), "x86_64-linux-gnu",
                                  /*verbose=*/true);
    for (auto* label : {"compiler", "compiler-runtime", "kernel-abi", "c-abi",
                        "c++-abi"})
        expect(ne(text.find(label), std::string::npos)) << label;
}};

// An absent layer is a statement, and whether the stack is shown at all must not
// depend on where that statement sits among the rows. A bare-metal build has an
// absent kernel interface ABOVE a prebuilt C library; deciding per row swallowed
// the first and printed the second.
UTest an_absent_layer_above_a_printed_one_is_still_printed{"TargetSideReport/AnAbsentLayerAboveAPrintedOneIsStillPrinted", [] {
    ts::Inputs in;
    in.llvmTriple         = "riscv64-none-elf";
    in.freestandingTarget = true;
    in.compilerFamily     = "llvm";
    in.sysrootXpkg        = "xim:picolibc-riscv@1.8.12";
    auto text = ts::format_report(ts::resolve(in), "riscv64-none-elf");
    expect(ne(text.find("c-abi"), std::string::npos)) << text;
    expect(ne(text.find("kernel-abi"), std::string::npos))
        << "the kernel interface is absent, which is the information:\n" << text;
}};

UTest only_the_layers_that_came_from_elsewhere_earn_a_line{"TargetSideReport/OnlyTheLayersThatCameFromElsewhereEarnALine", [] {
    auto in = payload_linux();
    in.compilerFamily = "llvm";
    in.cxxAbi = provider("openkal-llvm-runtime", "0.1.1", "libc++");
    auto text = ts::format_report(ts::resolve(in), "x86_64-linux-gnu");
    expect(ne(text.find("c++-abi"), std::string::npos));
    expect(eq(text.find("kernel-abi"), std::string::npos))
        << "the kernel interface is the payload's, which is not news";
}};

// ── The five layers ──────────────────────────────────────────────────────────

// A payload's C library is named by the triple's env field where the triple has
// one. macOS has none, and falling back to `glibc` named a library that does not
// exist on the platform — invisible while the report printed only the layers a
// build had something to say about.
UTest the_payload_c_library_is_named_for_its_platform{"TargetSideResolve/ThePayloadCLibraryIsNamedForItsPlatform", [] {
    auto mac = payload_linux();
    mac.targetOs  = "macos";
    mac.targetEnv = "";
    expect(eq(ts::resolve(mac).cAbi.interfaceName, "libSystem"));

    auto win = payload_linux();
    win.targetOs  = "windows";
    win.targetEnv = "";
    expect(eq(ts::resolve(win).cAbi.interfaceName, "ucrt"));

    // #540: the layer names the LIBRARY, never the triple's env segment. `musl`
    // is both spellings at once and hid the difference; `gnu` is not the name of
    // any C library, and on Windows it does not even mean the same one.
    expect(eq(ts::resolve(payload_linux()).cAbi.interfaceName, "glibc"))
        << "the env segment `gnu` asks for a C library; the layer answers glibc";

    auto winGnu = payload_linux();
    winGnu.targetOs  = "windows";
    winGnu.targetEnv = "gnu";
    expect(eq(ts::resolve(winGnu).cAbi.interfaceName, "ucrt"))
        << "on Windows `-gnu` names the MinGW toolchain flavour, not glibc";

    auto musl = payload_linux();
    musl.targetEnv = "musl";
    expect(eq(ts::resolve(musl).cAbi.interfaceName, "musl"))
        << "the one env segment that IS a C library name is unchanged";
}};

// The rename above must not turn every ordinary `-gnu` build into a reported
// request mismatch: the request keeps the triple's spelling by definition
// (docs/specs/target-side.md §3.4) and the answer now names the library.
UTest the_env_request_is_satisfied_by_the_library_it_names{"TargetSideResolve/TheEnvRequestIsSatisfiedByTheLibraryItNames", [] {
    expect(bool(ts::c_abi_request_satisfied("gnu",   "glibc")));
    expect(bool(ts::c_abi_request_satisfied("gnu",   "ucrt")));
    expect(bool(ts::c_abi_request_satisfied("musl",  "musl")));
    expect(bool(ts::c_abi_request_satisfied("glibc", "gnu")));
    expect(!ts::c_abi_request_satisfied("gnu",  "musl"));
    expect(!ts::c_abi_request_satisfied("musl", "glibc"));
}};

UTest the_compiler_is_a_layer_and_it_is_always_the_payloads{"TargetSideResolve/TheCompilerIsALayerAndItIsAlwaysThePayloads", [] {
    auto in = payload_linux();
    in.compilerFamily  = "gcc";
    in.compilerVersion = "16.1.0";
    auto r = ts::resolve(in);
    expect(eq(r.compiler.origin, ts::Origin::Payload));
    expect(eq(r.compiler.interfaceName, "gcc"));
    expect(eq(r.compiler.impl, "16.1.0"));
}};

// The builtins are what a PURE C PROGRAM needs. A package supplies them only
// when the payload's own are wrong for the target; absent from the graph means
// the payload's, not absent.
UTest the_compiler_runtime_defaults_to_the_compilers_own{"TargetSideResolve/TheCompilerRuntimeDefaultsToTheCompilersOwn", [] {
    auto in = payload_linux();
    in.compilerFamily = "llvm";
    auto r = ts::resolve(in);
    expect(eq(r.compilerRuntime.origin, ts::Origin::Payload));
    expect(eq(r.compilerRuntime.interfaceName, "llvm"));

    in.compilerRuntime = provider("openkal-llvm-runtime", "0.1.1", "compiler-rt");
    auto g = ts::resolve(in);
    expect(eq(g.compilerRuntime.origin, ts::Origin::Graph));
    expect(eq(g.compilerRuntime.interfaceName, "compiler-rt"));
}};

UTest the_compiler_is_a_layer_no_package_may_supply{"TargetSideCapability/TheCompilerIsALayerNoPackageMaySupply", [] {
    expect(!ts::layer_is_suppliable_by_package(ts::CapLayer::Compiler));
    for (auto l : {ts::CapLayer::CompilerRuntime, ts::CapLayer::KernelAbi,
                   ts::CapLayer::CAbi, ts::CapLayer::CxxAbi})
        expect(ts::layer_is_suppliable_by_package(l));
}};

UTest the_grammar_knows_all_five_layers{"TargetSideCapability/TheGrammarKnowsAllFiveLayers", [] {
    struct Row { const char* entry; ts::CapLayer layer; const char* iface; };
    for (auto const& row : std::initializer_list<Row>{
             {"mcpp:compiler=llvm",              ts::CapLayer::Compiler,        "llvm"},
             {"mcpp:compiler-runtime=compiler-rt", ts::CapLayer::CompilerRuntime, "compiler-rt"},
             {"mcpp:kernel-abi=openkal",         ts::CapLayer::KernelAbi,       "openkal"},
             {"mcpp:c-abi=musl",                 ts::CapLayer::CAbi,            "musl"},
             {"mcpp:c++-abi=libc++",             ts::CapLayer::CxxAbi,          "libc++"}}) {
        auto parsed = ts::parse_capability(row.entry);
        expect(fatal(parsed.has_value())) << row.entry;
        expect(fatal(parsed->has_value())) << row.entry;
        expect(eq((*parsed)->layer, row.layer)) << row.entry;
        expect(eq((*parsed)->interfaceName, row.iface)) << row.entry;
    }
    // Outside the reserved prefix the feature system owns the name.
    auto own = ts::parse_capability("freestanding-allocator");
    expect(fatal(own.has_value()));
    expect(!own->has_value());
    // Inside it, a misspelling is an error rather than a silently dead check.
    expect(!(ts::parse_capability("mcpp:c_abi=musl").has_value()));
}};

// ── The triple is a request; the target side is the fact ────────────────────

UTest a_filled_env_segment_states_nothing{"TargetSideRequest/AFilledEnvSegmentStatesNothing", [] {
    auto in = payload_linux();
    in.compilerFamily = "llvm";
    in.cAbi = provider("openkal-musl", "0.3.3", "musl");
    in.envAxis = ts::EnvAxis::CLibrary;
    // `--target x86_64-linux` — the project declined to name a C library, so
    // the parser's fill must not become a claim it can be held to.
    in.requestedCAbi.clear();
    expect(eq(ts::check_request(ts::resolve(in)), std::nullopt));
}};

// REPORTED, NOT REFUSED. The graph supplies the C library either way, so the
// segment is ignored rather than violated and the artifact is identical with or
// without it. Refusing was tried and broke every project spelling the host
// target `x86_64-linux-gnu` — which is what `mcpp toolchain list` prints.
UTest a_written_env_segment_that_disagrees_is_reported{"TargetSideRequest/AWrittenEnvSegmentThatDisagreesIsReported", [] {
    auto in = payload_linux();
    in.compilerFamily     = "llvm";
    in.cAbi               = provider("openkal-musl", "0.3.3", "musl");
    in.requestedCAbi      = "gnu";
    in.requestFreeTarget  = "x86_64-linux";
    in.envAxis            = ts::EnvAxis::CLibrary;
    auto why = ts::check_request(ts::resolve(in));
    expect(fatal(why.has_value()));
    expect(ne(why->find("`gnu`"), std::string::npos));
    expect(ne(why->find("musl"), std::string::npos));
    expect(ne(why->find("--target x86_64-linux"), std::string::npos))
        << "telling someone their target name is wrong is only useful once "
           "there is a right one to give them";
}};

UTest a_prebuilt_c_library_is_what_the_request_selected{"TargetSideRequest/APrebuiltCLibraryIsWhatTheRequestSelected", [] {
    auto in = payload_linux();
    in.compilerFamily = "llvm";
    in.requestedCAbi  = "musl";
    in.envAxis        = ts::EnvAxis::CLibrary;
    // No graph supplier: the payload's C library IS the request's answer, so
    // there is nothing to contradict even when the names differ.
    expect(eq(ts::check_request(ts::resolve(in)), std::nullopt));
}};

// On Windows the same segment names the OBJECT ABI — `gnu` is PE with the
// GNU ABI, `msvc` is PE with Microsoft's — and both are compatible with more
// than one C library. Reporting such a build as "asking for the `gnu` C ABI"
// describes an axis the name never addressed, and the correction it suggested
// named a target that does not exist.
// AND THE OBJECT-ABI AXIS REPORTS TOO, WHICH IS THE REVERSAL.
//
// It was exempted on the grounds that `gnu` on Windows names the Itanium C++
// ABI rather than a C library. True, and incomplete: the segment bundles the
// object ABI, which IS honoured, with MinGW's C runtime, which a graph-supplied
// C library replaces. The second half is a name/fact disagreement of exactly
// the shape this function reports, and it went unreported — while the identical
// shape on Linux warned.
UTest the_object_abi_axis_reports_the_c_library_half_too{"TargetSideRequest/TheObjectAbiAxisReportsTheCLibraryHalfToo", [] {
    auto in = payload_linux();
    in.targetOs          = "windows";
    in.compilerFamily    = "llvm";
    in.cAbi              = provider("openkal-musl", "0.3.3", "musl");
    in.requestedCAbi     = "gnu";
    in.requestFreeTarget = "x86_64-windows";
    in.envAxis           = ts::EnvAxis::ObjectAbi;

    auto why = ts::check_request(ts::resolve(in));
    expect(fatal(ne(why, std::nullopt)));
    if (auto&& failed = expect(ne(why->find("musl"), std::string::npos)); !failed) failed << *why;
    // And it says the ABI half was honoured, so the reader does not conclude
    // the object ABI changed as well.
    expect(ne(why->find("object ABI"), std::string::npos)) << *why;
    if (auto&& failed = expect(ne(why->find("--target x86_64-windows"), std::string::npos)); !failed) failed << *why;
}};

// The object-FORMAT axis stays exempt, and not for symmetry: `elf` names no C
// library on any platform, so "asks for the `elf` C ABI" would be nonsense
// rather than merely noisy.
UTest the_object_format_axis_stays_exempt{"TargetSideRequest/TheObjectFormatAxisStaysExempt", [] {
    auto in = payload_linux();
    in.targetOs          = "none";
    in.compilerFamily    = "llvm";
    in.cAbi              = provider("openkal-musl", "0.3.3", "musl");
    in.requestedCAbi     = "elf";
    in.requestFreeTarget = "riscv64-none";
    in.envAxis           = ts::EnvAxis::ObjectFormat;
    expect(eq(ts::check_request(ts::resolve(in)), std::nullopt));
}};

// AND WHEN IT DOES NOT NAME ONE, THE REPORT SAYS WHAT IT DOES NAME.
//
// Staying silent is correct as a DIAGNOSTIC and insufficient as a REPORT. The
// reader sees `x86_64-windows-gnu` above a line reading `c-abi musl`, finds no
// row called `gnu`, and maps it to the nearest thing that looks like a C
// library name. Measured twice, by the same reader, on two different days.
// THE GLOSS IS NOW THE OBJECT-FORMAT AXIS ALONE. The object-ABI axis warns
// instead (see `TheObjectAbiAxisReportsTheCLibraryHalfToo`), and leaving both
// in place would state one finding twice — once as an aside, once as a warning.
UTest the_object_abi_axis_is_no_longer_glossed{"TargetSideReport/TheObjectAbiAxisIsNoLongerGlossed", [] {
    auto in = payload_linux();
    in.targetOs          = "windows";
    in.compilerFamily    = "llvm";
    in.cAbi              = provider("openkal-musl", "0.3.3", "musl");
    in.requestedCAbi     = "gnu";
    in.requestFreeTarget = "x86_64-windows";
    in.envAxis           = ts::EnvAxis::ObjectAbi;

    auto r = ts::format_report(ts::resolve(in), "x86_64-windows-gnu");
    expect(eq(r.find("not a C library"), std::string::npos)) << r;
}};

// A bare-metal target's segment names the object FORMAT, and the same gloss
// applies with a different noun. One enum, three platforms, no `#if`.
UTest on_bare_metal_the_segment_names_the_object_format{"TargetSideReport/OnBareMetalTheSegmentNamesTheObjectFormat", [] {
    auto in = payload_linux();
    in.targetOs          = "none";
    in.compilerFamily    = "llvm";
    in.cAbi              = provider("openkal-musl", "0.3.3", "musl");
    in.requestedCAbi     = "elf";
    in.requestFreeTarget = "riscv64-none";
    in.envAxis           = ts::EnvAxis::ObjectFormat;

    auto r = ts::format_report(ts::resolve(in), "riscv64-none-elf");
    expect(ne(r.find("elf selects the object format, not a C library"), std::string::npos)) << r;
}};

// AND IT DOES NOT FIRE WHEN THE C LIBRARY CAME FROM A PAYLOAD.
//
// A payload C library IS what the triple selected — the triple is how it was
// selected — so `gnu → ucrt` follows visibly and a gloss would be noise on
// every ordinary Windows build.
UTest no_gloss_when_the_triple_itself_chose_the_c_library{"TargetSideReport/NoGlossWhenTheTripleItselfChoseTheCLibrary", [] {
    auto in = payload_linux();
    in.targetOs          = "windows";
    in.compilerFamily    = "llvm";
    in.requestedCAbi     = "gnu";
    in.requestFreeTarget = "x86_64-windows";
    in.envAxis           = ts::EnvAxis::ObjectAbi;
    // cAbi left as the payload's — not from the graph.

    auto r = ts::format_report(ts::resolve(in), "x86_64-windows-gnu");
    expect(eq(r.find("not a C library"), std::string::npos)) << r;
}};

// On Linux the segment DOES name a C library, so there is nothing to gloss —
// the disagreement is reported as a warning instead, which is a different
// mechanism and must not double up.
UTest no_gloss_where_the_segment_names_ac_library{"TargetSideReport/NoGlossWhereTheSegmentNamesACLibrary", [] {
    auto in = payload_linux();
    in.compilerFamily    = "llvm";
    in.cAbi              = provider("openkal-musl", "0.3.3", "musl");
    in.requestedCAbi     = "gnu";
    in.requestFreeTarget = "x86_64-linux";
    in.envAxis           = ts::EnvAxis::CLibrary;

    auto r = ts::format_report(ts::resolve(in), "x86_64-linux-gnu");
    expect(eq(r.find("not a C library"), std::string::npos)) << r;
    expect(ne(ts::check_request(ts::resolve(in)), std::nullopt));
}};

// ── Rule two: declared requirements ──────────────────────────────────────────

UTest a_requirement_is_checked_against_what_resolved{"TargetSideRequirements/ARequirementIsCheckedAgainstWhatResolved", [] {
    auto in = payload_linux();
    in.compilerFamily = "gcc";
    in.cxxAbi = provider("acme-runtime", "0.1.0", "libc++");
    auto r = ts::resolve(in);

    std::vector<ts::Requirement> reqs{
        {"acme-runtime@0.1.0", ts::CapLayer::Compiler, "llvm"}};
    auto why = ts::check_requirements(r, reqs);
    expect(fatal(why.has_value()));
    expect(ne(why->find("requires the compiler to be `llvm`"), std::string::npos));
    expect(ne(why->find("default = \"llvm\""), std::string::npos))
        << "a diagnostic that names no next step is a diagnostic the reader "
           "must still go and research";
    // AND THE STEP IT NAMES MUST NOT BE A GLOBAL ONE. Until 2026.8.26.2 the
    // first remedy offered was `mcpp toolchain default llvm` — the default for
    // every project on the machine, changed because ONE project's dependency
    // asked. mcpp now applies the graph's requirement itself wherever its own
    // answer was revisable, so a global change is both unnecessary and, in the
    // only case that still reaches here, ineffective.
    expect(eq(why->find("mcpp toolchain default"), std::string::npos))
        << "the remedy must stay inside the project that has the problem";

    // With the caller naming where the compiler was stated, the advice points
    // at that statement instead of at the generic pair of spellings.
    auto stated = ts::check_requirements(r, reqs, "[toolchain] in mcpp.toml");
    expect(fatal(stated.has_value()));
    expect(ne(stated->find("[toolchain] in mcpp.toml"), std::string::npos));
    expect(ne(stated->find("remove it"), std::string::npos))
        << "removing the statement is what lets mcpp take the graph's compiler";

    in.compilerFamily = "llvm";
    expect(eq(ts::check_requirements(ts::resolve(in), reqs), std::nullopt));
}};

UTest an_unsupplied_layer_is_named_as_such{"TargetSideRequirements/AnUnsuppliedLayerIsNamedAsSuch", [] {
    auto in = payload_linux();
    in.compilerFamily       = "llvm";
    in.freestandingTarget   = true;
    in.sysrootDeclaredEmpty = true;
    std::vector<ts::Requirement> reqs{
        {"acme-board@0.1.0", ts::CapLayer::CAbi, "picolibc"}};
    auto why = ts::check_requirements(ts::resolve(in), reqs);
    expect(fatal(why.has_value()));
    expect(ne(why->find("nothing supplies that layer"), std::string::npos));
    expect(ne(why->find("mcpp:c-abi=picolibc"), std::string::npos));
}};

// ── Rule one: one supplier per layer ─────────────────────────────────────────

UTest two_suppliers_are_named_together_with_how_each_arrived{"TargetSideConflict/TwoSuppliersAreNamedTogetherWithHowEachArrived", [] {
    ts::Conflict c;
    c.layer     = ts::CapLayer::CAbi;
    c.first     = "mcpplibs/openkal-musl@0.3.3";
    c.firstVia  = "a transitive dependency";
    c.second    = "acme/tinylibc@0.2.0";
    auto text = ts::format_conflict(c);
    expect(ne(text.find("openkal-musl@0.3.3"), std::string::npos));
    expect(ne(text.find("tinylibc@0.2.0"), std::string::npos));
    expect(ne(text.find("a transitive dependency"), std::string::npos));
    expect(ne(text.find("a direct dependency"), std::string::npos));
    expect(ne(text.find("choice rather than a contribution"), std::string::npos))
        << "the reason matters: choosing wrong does not fail the link, it "
           "produces a program that runs and occasionally does not";
}};

// ── Which layer decides the payload's C-library flags ───────────────────────
//
// THE PREDICATE THIS REPLACES WAS AN `OR` OVER TWO LAYERS, AND SHIPPED.
//
//     bool system_from_graph() const {
//         return kernelAbi.fromGraph() || cAbi.fromGraph();
//     }
//
// The link side replaced `f.ld` when that was true, dropping the `-B` that
// lets a driver find startup files. The two layers move together for an
// openkal target and come apart for a backend that implements openkal on top
// of a platform whose C library the program still uses. Measured, on three
// lines of manifest:
//
//     error: hermetic link check failed
//              crt1.o (bare name — the linker cannot resolve it)
//
// ONE TEST PER `Origin`, BECAUSE THE QUESTION HAS ONE ANSWER PER VALUE.
// A predicate written as a list of cases answers the ones its author thought
// of; four tests against a four-valued enum make the fifth value's absence
// visible when someone adds it.
namespace {
mcpp::targetside::TargetSide side_with(mcpp::targetside::Origin cAbiOrigin,
                                       mcpp::targetside::Origin kernelOrigin
                                           = mcpp::targetside::Origin::Graph) {
    using namespace mcpp::targetside;
    TargetSide ts;
    ts.kernelAbi = { kernelOrigin, "openkal", "openkal-linux@0.5.4", false };
    ts.cAbi      = { cAbiOrigin,
                     cAbiOrigin == Origin::None ? "" : "glibc", "", false };
    return ts;
}
}

UTest the_payloads_c_library_is_prebuilt{"TargetSideCLibrary/ThePayloadsCLibraryIsPrebuilt", [] {
    // The shape that failed: the graph supplies the kernel interface and the C
    // library is still the payload's, so the payload's flags must survive.
    expect(bool(side_with(mcpp::targetside::Origin::Payload).cAbi.prebuilt()));
}};

UTest an_xpkg_sysroot_is_prebuilt_too{"TargetSideCLibrary/AnXpkgSysrootIsPrebuiltToo", [] {
    // The value a hand-written `fromGraph() || absent()` was silent about. A
    // sysroot from an xpkg is a directory that existed before resolution, like
    // a payload and unlike a package.
    expect(bool(side_with(mcpp::targetside::Origin::Xpkg).cAbi.prebuilt()));
}};

UTest a_graph_built_c_library_is_not_prebuilt{"TargetSideCLibrary/AGraphBuiltCLibraryIsNotPrebuilt", [] {
    expect(!(side_with(mcpp::targetside::Origin::Graph).cAbi.prebuilt()));
}};

UTest no_c_library_is_not_prebuilt{"TargetSideCLibrary/NoCLibraryIsNotPrebuilt", [] {
    // Nothing to reach, and the link line adds `-nostdlib -static` for it.
    expect(!(side_with(mcpp::targetside::Origin::None).cAbi.prebuilt()));
    expect(bool(side_with(mcpp::targetside::Origin::None).cAbi.absent()));
}};

UTest the_kernel_interface_does_not_decide_it{"TargetSideCLibrary/TheKernelInterfaceDoesNotDecideIt", [] {
    // The defect itself, as an assertion: every kernel-interface origin leaves
    // the answer to the C-ABI layer alone.
    using namespace mcpp::targetside;
    for (auto k : { Origin::Payload, Origin::Xpkg, Origin::Graph, Origin::None }) {
        expect(bool(side_with(Origin::Payload, k).cAbi.prebuilt()));
        expect(!(side_with(Origin::Graph,  k).cAbi.prebuilt()));
    }
}};

// ── interfaces_not_provided — the whole of the engine's participation ───────
//
// openkal SPEC 0.14 §3.3 withdrew `hosted`, the one name it had given to a set
// of interfaces, and replaced it with enumeration by the consumer. This engine
// implements that as a set difference over opaque strings and learns no
// member of either set; these tests use openkal's real names in one place and
// an invented one in another, and the function behaves identically.

UTest every_required_interface_provided_yields_no_gap{"TargetSideInterfaces/EveryRequiredInterfaceProvidedYieldsNoGap", [] {
    std::vector<std::string> required{"openkal.fs", "openkal.stream"};
    std::vector<std::string> provided{"openkal.abort", "openkal.stream",
                                      "openkal.memory", "openkal.fs"};
    expect(mcpp::targetside::interfaces_not_provided(required, provided)
                    .empty());
}};

UTest a_missing_interface_is_reported_by_name{"TargetSideInterfaces/AMissingInterfaceIsReportedByName", [] {
    std::vector<std::string> required{"openkal.fs", "openkal.net"};
    std::vector<std::string> provided{"openkal.fs"};
    auto gap = mcpp::targetside::interfaces_not_provided(required, provided);
    expect(fatal(eq(gap.size(), 1u)));
    expect(eq(gap[0], "openkal.net"));
}};

UTest a_name_this_engine_has_never_heard_of_behaves_the_same{"TargetSideInterfaces/ANameThisEngineHasNeverHeardOfBehavesTheSame", [] {
    // The point of the opaque strings: a specification may add an interface
    // without a release of this engine, and a name that does not exist is
    // missing for the same reason a name that does exist and is not provided
    // is missing. Neither is a special case here.
    std::vector<std::string> required{"openkal.event", "some.other.spec/iface"};
    std::vector<std::string> provided{"openkal.fs"};
    auto gap = mcpp::targetside::interfaces_not_provided(required, provided);
    expect(fatal(eq(gap.size(), 2u)));
    expect(eq(gap[0], "openkal.event"));
    expect(eq(gap[1], "some.other.spec/iface"));
}};

UTest requiring_nothing_is_never_a_gap{"TargetSideInterfaces/RequiringNothingIsNeverAGap", [] {
    expect(mcpp::targetside::interfaces_not_provided({}, {}).empty());
    expect(mcpp::targetside::interfaces_not_provided({}, {"openkal.fs"}).empty());
}};

UTest providing_nothing_makes_every_requirement_a_gap{"TargetSideInterfaces/ProvidingNothingMakesEveryRequirementAGap", [] {
    // The shape a graph takes when the provider predates the key. The caller
    // (mcpp.build.prepare) does not reach this function in that case — a
    // provider that states nothing is not a provider that provides nothing —
    // and this test pins the function's own answer so that distinction stays
    // the caller's and does not migrate here by accident.
    std::vector<std::string> required{"openkal.fs"};
    auto gap = mcpp::targetside::interfaces_not_provided(required, {});
    expect(fatal(eq(gap.size(), 1u)));
}};
