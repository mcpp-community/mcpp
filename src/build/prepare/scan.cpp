// scan.cpp -- P11 and P12: the module scan and its validation, the
// standard-module gate, and the fingerprint.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.platform.axis;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;      // is_msvc_target — the MSVC-ABI default (#718)
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.stdmod;
import mcpp.freestanding.target;   // the target sysroot layout (libdir)
import mcpp.freestanding.linkline; // the ISA profile, for the std module command
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.toolchain.triple;
import mcpp.build.plan;
import mcpp.build.schedule.policy;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.xlings;
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.platform.macos;
import mcpp.log;

namespace mcpp::build {

// Scans this graph's sources and validates the result, returning whether the
// graph (or one of its extra targets) imports `std` -- the single value
// several of the steps below need, computed here because it depends on the
// scan this step performs.
static std::expected<bool, std::string> step11_scan_sources(PrepareState& state) {

    // mcpp#225 (E2): observability marker for the source-discovery phase —
    // `mcpp run`'s fast path (build_run_target/try_fast_run in execute.cppm)
    // skips prepare_build ENTIRELY on a cache hit, so this line's absence
    // under MCPP_VERBOSE=1 on a second `mcpp run` is the "did we re-scan"
    // signal the e2e test asserts on (tests/e2e/114_run_scan_scope.sh).
    mcpp::log::verbose("scan", "scanning module sources");

    // Modgraph: regex scanner by default; opt-in to compiler-driven P1689
    // scanner via env var MCPP_SCANNER=p1689 (see docs/27).
    //
    // A dependency whose declared targets are all programs compiles nothing in
    // this build (#649 E6): its programs come from the tool sub-build, which
    // scans it as its own root. The root itself is always scanned.
    std::vector<mcpp::modgraph::PackageRoot> scannedPackages;
    scannedPackages.reserve(state.packages.size());
    for (std::size_t i = 0; i < state.packages.size(); ++i)
        if (state.compilesHere(i))
            scannedPackages.push_back(state.packages[i]);

    // THE CLOSURE OF EACH PACKAGE THIS PLAN COMPILES (mcpp#732): the package
    // and every package it reaches through code and workspace-member edges. A
    // module name is unique within one program, and a program links its root
    // package's closure. An `artifacts` edge ships a separate program and a
    // `[build-dependencies]` edge serves the build, so neither is followed. A
    // workspace plan's virtual root compiles nothing and has no closure, so
    // two members that share no program may each provide a module of one name.
    mcpp::modgraph::Closures closures;
    {
        auto qualified = [](const mcpp::manifest::Manifest& m) {
            return m.package.namespace_.empty() ? m.package.name
                                                : m.package.namespace_ + "." + m.package.name;
        };
        std::map<std::size_t, std::vector<std::size_t>> codeEdges;
        for (auto const& e : state.dependencyEdges) {
            if (!e.requestedArtifacts.empty() || e.buildOnly) continue;
            codeEdges[e.consumerPackageIndex].push_back(e.dependencyPackageIndex);
        }
        for (std::size_t i = 0; i < state.packages.size(); ++i) {
            if (!state.compilesHere(i)) continue;
            if (i == 0 && state.m->package.virtualRoot) continue;
            std::set<std::string> names;
            std::set<std::size_t> seen{i};
            std::vector<std::size_t> work{i};
            while (!work.empty()) {
                const auto k = work.back();
                work.pop_back();
                names.insert(qualified(state.packages[k].manifest));
                if (auto it = codeEdges.find(k); it != codeEdges.end())
                    for (auto j : it->second)
                        if (seen.insert(j).second) work.push_back(j);
            }
            closures[qualified(state.packages[i].manifest)] = std::move(names);
        }
    }

    state.scan = [&] {
        const char* sel = std::getenv("MCPP_SCANNER");
        if (sel && std::string_view(sel) == "p1689") {
            auto tmp = std::filesystem::temp_directory_path()
                     / std::format("mcpp_p1689_{}", std::random_device{}());
            std::filesystem::create_directories(tmp);
            return mcpp::modgraph::scan_packages_p1689(scannedPackages, *state.tc, tmp,
                                                       state.stdFlagAndDialect, closures);
        }
        return mcpp::modgraph::scan_packages(scannedPackages, closures);
    }();
    if (!state.scan.errors.empty()) {
        std::string msg = "scanner errors:\n";
        for (auto& e : state.scan.errors) msg += "  " + e.format() + "\n";
        return std::unexpected(msg);
    }
    for (auto& w : state.scan.warnings) {
        mcpp::diag::warning("modgraph/scan", w.format());
    }

    state.report = mcpp::modgraph::validate(state.scan.graph, *state.m, *state.root);
    for (auto& w : state.report.warnings) {
        if (w.path.empty()) mcpp::diag::warning("modgraph/validate", w.message);
        else mcpp::diag::warning("modgraph/validate",
                                 std::format("{}: {}", w.path.string(), w.message));
    }
    if (!state.report.ok()) {
        std::string msg = "validation errors:\n";
        for (auto& e : state.report.errors) {
            if (e.path.empty()) msg += "  " + e.message + "\n";
            else msg += "  " + e.path.string() + ": " + e.message + "\n";
        }
        return std::unexpected(msg);
    }

    state.needsStdCompat = graph_or_targets_import_std_compat(
        state.scan.graph, *state.m, *state.root, state.packages);
    return graph_or_targets_import_std(state.scan.graph, *state.m, *state.root, state.packages);
}

static std::expected<void, std::string>
step11_dependency_standard_scope_check(PrepareState& state) {
    // A DEPENDENCY THAT DECLARED A HIGHER STANDARD THAN THE GRAPH IS BUILT AT.
    //
    // A C++ module graph has ONE standard — cross-level BMIs are hard
    // incompatible — so the root package's level is imposed graph-wide, and a
    // dependency's `standard` is parsed and then discarded. That is correct and
    // is not the defect. The defect is the silence: a package that declared
    // c++26 because it needs c++26 is compiled at whatever the consumer says,
    // and fails — if it fails at all — with a compiler error inside a
    // translation unit the user does not own, naming neither package nor the
    // mechanism.
    //
    // SCOPED TO MANIFESTS THE PROJECT AUTHOR CONTROLS, and that scope is the
    // whole reason this check is shippable. The cpp20 design doc's §9-Q3
    // declined it because the default and a declaration were indistinguishable;
    // `standardDeclared` fixes that for `mcpp.toml`, and NOT for the index:
    // measured over the local registry, every descriptor with an mcpp segment
    // declares `language` (782 of 782), and 756 of those 774 packages are C
    // libraries with `import_std = false` carrying a boilerplate "c++23". A
    // check that trusted declaredness everywhere would fire against essentially
    // the whole index for any root at c++20 — exactly the outcome §9-Q3
    // refused, reached through a different door.
    //
    // DEGRADED, NOT AN ERROR. The condition is not a proven failure: a package
    // declaring c++26 compiles perfectly well at c++23 whenever it happens not
    // to use a C++26 construct, and that is a working configuration today for
    // anyone who wrote the key aspirationally. `--strict` promotes it.
    const auto graphLevel = state.m->cppStandard.level;
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        auto const& pkg = state.packages[i];
        if (!pkg.manifest.package.standardDeclared) continue;
        // A C++-layer provider's declaration IS applied, to every unit of
        // it that neither provides nor imports a module (`make_plan`), so
        // "is not applied" would be false for exactly the package whose
        // sources need the level. Its module units stay at the graph's
        // level, as every module unit does.
        if (mcpp::manifest::cxx_layer_implementation_standard(pkg.manifest))
            continue;
        // The scope gate. A package whose root is under a store directory
        // arrived from an index and its declaration was written by a
        // descriptor generator, not by the person reading this diagnostic.
        if (mcpp::build::path_is_under_any(pkg.root, state.storeRoots))
            continue;
        auto declared = mcpp::manifest::normalize_cpp_standard(
            pkg.manifest.package.standard);
        if (!declared || declared->level <= graphLevel) continue;
        mcpp::diag::degraded(
            "build/standard",
            std::format("dependency `{}` declares standard = \"{}\", and "
                        "this graph is built at {}",
                        pkg.manifest.package.name,
                        declared->canonical, state.m->cppStandard.canonical),
            "a C++ module graph has one standard, so the dependency's "
            "declaration is not applied and its sources are compiled at the "
            "graph's level",
            std::format(
                "raise the consumer's standard to \"{}\", or declare it "
                "once for every member:\n\n  [workspace.package]\n  "
                "standard = \"{}\"", declared->canonical, declared->canonical));
    }
    return {};
}

static std::expected<void, std::string>
step11_dialect_flag_reaches_std_prebuild(PrepareState& state, bool needsStdModule) {
    // A DIALECT FLAG THAT REACHES EVERY TU AND NOT THE `import std` PREBUILD
    // IS A BUILD THAT CANNOT SUCCEED, AND MCPP KNOWS IT BEFORE COMPILING.
    //
    // `[build] cxxflags = ["-fno-exceptions"]` is applied to each translation
    // unit; the std BMI in `stdFlagAndDialect` is precompiled without it,
    // because only `dialect_flags()` rides that channel. Every importer then
    // fails inside a file mcpp generated:
    //
    //   std: error: language dialect differs 'C++23', expected
    //               'C++23/no-exceptions'
    //   std: error: failed to read compiled module: Bad file data
    //
    // The message names the mechanism and not the key, so the way out
    // (`dialect_cxxflags`, which IS applied to the prebuild, the scan and every
    // TU) is not discoverable from it. Both facts are known here: whether the
    // graph imports `std`, and which flags reached the prebuild.
    //
    // REFUSED RATHER THAN WARNED, and that is the same rule the host-dependence
    // diagnostics follow from the other side: this build provably cannot
    // succeed, so there is no user decision to respect. Contrast
    // `[toolchain] system`, which builds and runs and is therefore warned about.
    //
    // GATED ON `needsStdModule` — without `import std` in the graph there is no
    // prebuilt BMI to disagree with, and `-fno-exceptions` is then an ordinary
    // per-unit flag that works. A check that refused in both cases would have
    // stopped testing the condition it claims to test.
    if (needsStdModule) {
        const auto prebuilt = mcpp::toolchain::cppfly::effective_dialect_flags(
            *state.tc, state.m->cppStandard.experimental,
            mcpp::manifest::dialect_flags(state.m->buildConfig));
        // THE ROOT PACKAGE ONLY, and the narrowing is a correctness bound
        // rather than a shortcut.
        //
        // A dependency carrying the same flag fails identically — but only if
        // ITS OWN translation units import `std`. `needsStdModule` is a
        // property of the whole graph: a C++ wrapper package that uses no std
        // module can carry `-fno-exceptions` in its `[build] cxxflags` and
        // compile perfectly well inside a graph whose ROOT imports std.
        // Refusing there would stop a build that works, which is the one thing
        // a refusal must never do — the rule is "provably cannot build", and
        // for a dependency this evidence does not prove it.
        //
        // Extending it needs a per-package answer to "does this package import
        // std", which the scan graph holds and does not expose in that shape.
        // Recorded here so the next person meets the reason and not the gap.
        for (auto const& pkg : std::span{state.packages}.first(1)) {
            const auto words = mcpp::manifest::flag_words(pkg.manifest.buildConfig.cxxflags);
            auto missing = mcpp::manifest::dialect_flags_missing_from_prebuild(words, prebuilt);
            if (missing.empty()) continue;
            std::string list;
            for (auto const& f : missing) {
                if (!list.empty()) list += ", ";
                list += '`'; list += f; list += '`';
            }
            // NAMES THE FLAG, NOT THE TABLE IT CAME FROM. The same flag
            // reaches the compile line from `[build] cxxflags`, from
            // `[profile.<name>] cxxflags` and from a `[target.…]` / `cfg(...)`
            // block; by the time it is read here they have been merged, and
            // asserting one of them would be wrong two times in three.
            return std::unexpected(std::format(
                "{} changes the language dialect{}, but the `import std` BMI is "
                "precompiled without it, so every importing translation unit "
                "will fail with \"language dialect differs\".\n"
                "       Declare it as a dialect flag instead — that channel is "
                "applied to the std BMI prebuild, the module scan and every TU "
                "in the graph:\n"
                "\n"
                "         [build]\n"
                "         dialect_cxxflags = [{}]\n"
                "\n"
                "       It belongs in `[build]` and not in a profile or a "
                "per-target block: a dialect the standard library was not built "
                "with cannot be held by one package or one profile alone.",
                list, std::string{},
                [&] {
                    std::string q;
                    for (auto const& f : missing) {
                        if (!q.empty()) q += ", ";
                        q += '"'; q += f; q += '"';
                    }
                    return q;
                }()));
        }
    }
    return {};
}

static std::expected<void, std::string>
step11_msvc_crt_word_check(PrepareState& state) {
    // A FREE-FORM CRT WORD IS ALWAYS A SECOND STATEMENT ON THE MSVC ABI (D3,
    // #718). Every MSVC-ABI build now states its own CRT model, so a literal
    // `/MT`/`/MD`(`d`) or `-fms-runtime-lib=*` in `[build] cxxflags` or
    // `dialect_cxxflags` can never be the only voice: agreeing repeats a
    // fact already resolved (warned, naming the key to write instead);
    // disagreeing is refused before compiling, naming the word, the key and
    // the value it corresponds to.
    //
    // NOT gated on `needsStdModule`: the CRT model is a link-time fact for
    // every MSVC-ABI build, with or without `import std`.
    //
    // EVERY PACKAGE'S `cxxflags`, because each reaches its own package's
    // units after the graph's flags and would compile them against another
    // CRT: one image, two CRTs. `dialect_cxxflags` is the root's alone (a
    // dependency's reaches no command). `cxx_runtime` and `linkage` are
    // root-level keys, so a dependency's agreeing word is not warned: the key
    // the warning would name cannot be written there, and the word changes
    // nothing. Its contradicting word is refused like the root's.
    if (mcpp::toolchain::is_msvc_target(*state.tc)) {
        const bool wantsStatic = mcpp::toolchain::msvc_wants_static_crt(
            state.m->buildConfig.linkage, state.m->buildConfig.cxxRuntime);
        for (std::size_t i = 0; i < state.packages.size(); ++i) {
            auto const& pkg = state.packages[i];
            // A selected workspace member is the project being developed and
            // is spoken to as a root (§15).
            const bool isRoot = i == 0 || pkg.selectedMember;
            // A word `[workspace.build]` contributed is named at the table
            // that states it (WS3): the member's `[build]` does not contain
            // it, and a reader sent there finds nothing to remove.
            auto inherited_words = [&](std::string_view tomlKey) {
                auto it = pkg.manifest.buildConfig.inheritedFromWorkspace.find(
                    std::string(tomlKey));
                return it == pkg.manifest.buildConfig.inheritedFromWorkspace.end()
                    ? std::vector<std::string>{}
                    : mcpp::manifest::flag_words(it->second);
            };
            auto check_words = [&](std::span<const std::string> list,
                                   std::string_view key, std::string_view tomlKey)
                    -> std::expected<void, std::string> {
                const auto fromWorkspace = inherited_words(tomlKey);
                const auto workspaceKey = std::format("[workspace.build] {}", tomlKey);
                for (auto const& w : list) {
                    const bool inherited =
                        std::ranges::find(fromWorkspace, w) != fromWorkspace.end();
                    auto verdict = mcpp::toolchain::check_crt_word(
                        w, wantsStatic, inherited ? std::string_view(workspaceKey) : key);
                    if (!verdict) continue;
                    if (verdict->contradicts)
                        return std::unexpected(verdict->message);
                    // Redundant, not degraded: the engine does exactly what
                    // it would have done without the flag.
                    if (isRoot)
                        mcpp::diag::warning("build/msvc-crt-word",
                            verdict->message);
                }
                return {};
            };
            const auto cxxflagsWords =
                mcpp::manifest::flag_words(pkg.manifest.buildConfig.cxxflags);
            const auto cxxflagsKey = isRoot
                ? std::string("[build] cxxflags")
                : std::format("the [build] cxxflags of dependency '{}'",
                              pkg.manifest.package.name);
            if (auto r = check_words(cxxflagsWords, cxxflagsKey, "cxxflags"); !r)
                return std::unexpected(r.error());
            if (isRoot)
                if (auto r = check_words(pkg.manifest.buildConfig.dialectCxxflags,
                                         "[build] dialect_cxxflags", "dialect_cxxflags"); !r)
                    return std::unexpected(r.error());
        }
    }
    return {};
}

static std::expected<void, std::string>
step11_package_std_module_source(PrepareState& state) {
    // A standard library that came from a PACKAGE brings its own module
    // source, because the compiler cannot be asked for one it does not have.
    //
    // `-print-library-module-manifest-path' is the right question when the
    // standard library is the compiler's own. It is the wrong question when
    // the library was configured by a package for a target the compiler
    // knows nothing about: the source exists, and the compiler has never
    // heard of it. So the package says where it is, and what it needs ---
    // its include path and its own __config_site, neither of which the
    // compiler would find.
    //
    // Both are read only from a package that ALSO provides the capability
    // below. A package that named a std module without supplying the
    // library would be describing something it does not have.
    for (auto& pkg : state.packages) {
        if (pkg.manifest.stdModule.empty()) continue;
        // Either spelling of the C++ layer (see `provides_cxx_layer`). The
        // same predicate decides which package's implementation units keep
        // their own standard in `make_plan`, so the two cannot name different
        // packages as the standard library.
        if (!mcpp::manifest::provides_cxx_layer(pkg.manifest)) continue;
        auto src = pkg.root / pkg.manifest.stdModule;
        if (!std::filesystem::exists(src)) {
            return std::unexpected(std::format(
                "package '{}' declares [package].std-module = '{}', and there "
                "is no such file under '{}'",
                pkg.manifest.package.name, pkg.manifest.stdModule,
                pkg.root.string()));
        }
        // AND THE COMPAT MODULE, FROM THE SAME PACKAGE OR NOT AT ALL.
        //
        // `std.compat` is a second module over the SAME library. Leaving it
        // pointing at the toolchain's copy does not fail where it is set — it
        // fails later, in that copy's own headers, against a configuration that
        // was never generated for this target. Measured on a macOS cross:
        //
        //   error: std module precompile failed (rc=1):
        //     …/xim-x-llvm/22.1.8/share/libc++/v1/std.compat.cppm
        //     …/include/c++/v1/__config:13: '__config_site' file not found
        //
        // — which reads as a broken toolchain payload and says nothing about
        // the two libraries having been mixed. A package that supplies one
        // module supplies both, or the pair is not offered.
        if (!pkg.manifest.stdCompatModule.empty()) {
            auto csrc = pkg.root / pkg.manifest.stdCompatModule;
            if (!std::filesystem::exists(csrc)) {
                return std::unexpected(std::format(
                    "package '{}' declares [package].std-compat-module = '{}', "
                    "and there is no such file under '{}'",
                    pkg.manifest.package.name, pkg.manifest.stdCompatModule,
                    pkg.root.string()));
            }
            state.tc->set_std_modules(src, csrc);
        } else {
            state.tc->set_std_modules(src);
        }
        state.tc->targetCxxRuntime  = true;
        state.tc->importStdMinLevel = 20;   // libc++'s own floor; see clang.cppm
        // The target, first. On a freestanding target that means the whole ISA
        // profile --- `--target', `-march', `-mabi', `-mcmodel' --- because a
        // module built without them disagrees with every unit that imports it,
        // and clang reports that as an ABI mismatch naming a .pcm file rather
        // than the flag that split them. On a hosted one it is the triple alone.
        std::string flags;
        if (auto fs = mcpp::toolchain::triple::parse(state.tc->targetTriple);
            fs && fs->is_freestanding()) {
            if (auto spec = mcpp::freestanding::resolve(*fs))
                flags += mcpp::freestanding::compile_prefix(*spec, true);
        } else if (!state.tc->crossTargetFlag.empty()) {
            // `crossTargetFlag` and not `targetTriple`. The triple is mcpp's
            // vocabulary (`aarch64-macos`); the flag carries the spelling a
            // compiler takes (`arm64-apple-macos14.0`). Measured: emitting the
            // first produced `--target=aarch64-macos`, which clang accepts as a
            // triple it has never heard of and then treats as a bare-metal
            // aarch64 — the module and its importers would agree with each
            // other and with nothing else.
            flags += " " + state.tc->crossTargetFlag;
            // AND THE SECOND CHANNEL. `hostflags.cppm` reaches every ordinary
            // translation unit; this command is assembled here instead, so a
            // `std.pcm` built with SEH would be imported by units built with
            // DWARF. Same function, not a second copy of the decision.
            for (auto& f : mcpp::toolchain::graph_runtime_compile_flags(*state.tc))
                flags += " " + f;
        }
        // `__OPENKAL__` AND THE REALISED [c-abi] ENVIRONMENT REACH THE STD
        // MODULE TOO (design §3.4: "环境作用于目标侧的全部编译单元... 以及图中
        // 所有普通包"). The std module's own command is assembled here rather
        // than through `mcpp.toolchain.hostflags`'s shared string (see the
        // comment above), so it needs the same broadcast the ordinary
        // per-package loop gives every other unit — this is that same rule,
        // stated once more at the one site it cannot reach on its own.
        if (state.tc->kernelAbiIsOpenkal) flags += " -D__OPENKAL__";
        if (pkg.manifest.cEnvironment != "platform") {
            for (auto& t : state.tc->cEnvTokens)         flags += " " + t;
            for (auto& t : state.tc->cEnvBuiltinsTokens)  flags += " " + t;
        }
        // Everything up to here says which machine the module is for; what
        // follows says where its headers are. The codegen step needs only the
        // first — see Toolchain::stdModuleTargetFlags.
        state.tc->stdModuleTargetFlags = flags;
        for (auto& f : pkg.manifest.buildConfig.stdModuleFlags) {
            // A flag naming a path is relative to the package that named it,
            // for the same reason the module source is.
            auto candidate = pkg.root / f;
            flags += " " + mcpp::xlings::shq(
                std::filesystem::exists(candidate) ? candidate.string() : f);
        }
        // AND THE HEADERS THIS PACKAGE ITSELF IS BUILT AGAINST.
        //
        // The std module source is one of this package's translation units in
        // every way that matters, and it reaches the C library's headers the
        // same way the rest of them do --- through the requirements the packages
        // BENEATH this one publish. A package cannot name those in its own
        // manifest: they belong to its dependencies, and their paths are known
        // only after resolution.
        //
        // Measured: without them the module compiles until libc++ includes
        // <bits/alltypes.h>, which is the C library's, and stops there.
        // publicUsage rather than privateBuild: the module is compiled once and
        // imported by consumers, so the headers it must see are the ones the
        // package PUBLISHES, not the ones it happens to build itself against.
        // The two differ, and the difference is not cosmetic --- a package's own
        // build path carries directories that exist for its .cpp files and that
        // shadow the library's headers when a module is compiled against them.
        //
        // AND IT IS `targetSideUsage`, NOT THIS PACKAGE'S `publicUsage`.
        //
        // The two are the same set whenever one package supplies every layer,
        // which is the arrangement this block was written for — so reading the
        // package directly was correct and stayed correct until a second
        // provider appeared. `openkal-llvm-runtime` supplies the C++ runtime
        // while `openkal-musl` supplies the C library, and the std module needs
        // both: libc++'s own headers reach `<bits/alltypes.h>`, which is the C
        // library's.
        //
        // Reading the assembled set also makes this site and every compile
        // edge read ONE value. Deriving it here a second time is the shape
        // #233/#240/#242/#344 each cost a release, and the same set has to
        // reach both or the `std` BMI describes a different world than the
        // units importing it — which is mcpp#514 exactly.
        for (auto& d : state.targetSideUsage.includeDirs)
            flags += " -isystem " + mcpp::xlings::shq(d.string());
        for (auto& d : state.targetSideUsage.includeDirsAfter)
            flags += " -idirafter " + mcpp::xlings::shq(d.string());
        // And the definitions, for the same reason as the directories: a C
        // library's headers show a different library depending on which feature
        // macros are set, and the ones this package is built with are the ones
        // its own translation units see. Measured: without them the module
        // reaches musl's <time.h> and stops on `clockid_t', a name that header
        // declares only under the macro the package carries.
        // THE PREBUILT C LIBRARY'S OWN TOKENS, FROM THE PRODUCER EVERY UNIT
        // USES. A package that supplies the C++ layer over a prebuilt C
        // library (`llvm.libcxx` over glibc, or over an Apple SDK) has no
        // way to name that library's headers in its manifest, and the
        // target-side broadcast below carries only graph layers. Without
        // these the precompile reads whatever the driver finds on its own:
        // on Linux the runner's `/usr/include` rather than the payload's
        // glibc, a host dependency no report showed; on macOS nothing, and
        // the precompile stops on `mbstate_t`; on the iOS rows nothing, and
        // it stopped on the same name. Measured on 2026-09-14 across the
        // three. `host_compile_tokens` is what every translation unit of the
        // build gets, asked with the C++ layer marked as the graph's so that
        // it withholds the payload's libc++ and emits the rest: the cfg
        // bypass, the C library's directories, the SDK and the deployment
        // floor. Same function, not a second copy.
        //
        // AND LAST ON THE COMMAND, after the package's own directories:
        // `-isystem` order is search order, and libc++'s headers must precede
        // the C library's, which libc++ states in as many words (`<cctype>`
        // stops the build if it reaches a `<ctype.h>` that is not its own).
        // Emitted ahead of them, glibc's `<math.h>` shadowed libc++'s wrapper
        // and `<complex>` failed on `std::__builtin_isnan` (measured).
        if (state.tc->cAbiPrebuilt) {
            mcpp::toolchain::HostFlagOptions hopt;
            hopt.cfgBypass             = mcpp::toolchain::HostFlagOptions::CfgBypass::Always;
            hopt.cAbiPrebuilt          = true;
            hopt.cxxFromGraph          = true;
            hopt.appleSdkRoot          = state.tc->appleSdkRoot;
            // Target-keyed, not host-keyed (#685) — see `min_platform_version`.
            const bool cAbiTargetIsMacos = [&] {
                auto cAbiTt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
                return cAbiTt && cAbiTt->os == "macos";
            }();
            hopt.macosDeploymentTarget = mcpp::platform::macos::deployment_target(
                cAbiTargetIsMacos, state.m->buildConfig.macosDeploymentTarget);
            for (auto& t : mcpp::toolchain::host_compile_tokens(
                     *state.tc, hopt, mcpp::toolchain::no_escape)) {
                const auto q = " " + mcpp::xlings::shq(t);
                if (flags.find(q) == std::string::npos) flags += q;
            }
            for (auto& t : mcpp::toolchain::apple_float_macro_words(*state.tc)) {
                const auto q = " " + mcpp::xlings::shq(t);
                if (flags.find(q) == std::string::npos) flags += q;
            }
        }
        // The same words the package's own units receive from this list
        // (mcpp.manifest.flag_words), one quoted word each.
        for (auto& w : mcpp::manifest::flag_words(state.targetSideUsage.cxxflags))
            flags += " " + mcpp::xlings::shq(w);
        state.tc->stdModuleFlags = flags;
        break;
    }
    return {};
}

static std::expected<void, std::string>
step11_apple_sdk_cxx_runtime(PrepareState& state, bool needsStdModule) {
    // AN APPLE CROSS TARGET WITHOUT A GRAPH C++ RUNTIME LINKS THE SDK'S
    // libc++ (the Mach-O cell in distribution.cppm), AND THE HEADERS FOLLOW
    // THE RUNTIME. The payload's `std.cppm` and headers describe libc++ 22;
    // the SDK's dylib is libc++ 19 (Xcode 16.4, measured), and Apple's SDKs
    // ship no module sources of their own (no `usr/share/libc++/v1` on the
    // macOS 15.5 and iOS 18.5 SDKs). So:
    //
    //   - a graph that does not import `std` takes the SDK's headers
    //     (hostflags.cppm, `appleSdkCxxHeaders`): one libc++ on every line,
    //     and the payload's module, unused, is withdrawn;
    //   - a graph that imports `std` keeps the payload's module and headers
    //     over the SDK's dylib. That pairing links until an inline path in
    //     the newer headers names an export the older dylib lacks
    //     (`__hash_memory`, `__atomic_notify_all_global_table`, measured),
    //     and it is what every iOS program built before this release got.
    //     It is REPORTED ONCE rather than refused: refusing would break a
    //     program that built yesterday, and the report names the two lines
    //     that make the hazard disappear.
    if (state.tc && !state.tc->appleSdkRoot.empty() && state.targetSideResolved
        && !state.resolvedTargetSide.cxx.fromGraph()) {
        if (!needsStdModule) {
            state.tc->appleSdkCxxHeaders = true;
            state.tc->clear_std_modules();
        } else {
            mcpp::diag::degraded("target/cxx-runtime", std::format(
                "{} links the SDK's libc++ under the toolchain payload's "
                "libc++ headers and std module, which are a different "
                "release of the library", state.tc->targetTriple),
                "the program links while no inline path in the newer headers "
                "names an export the SDK's dylib lacks; `std::unordered_map` "
                "over `std::string` and `std::atomic<T>::notify_all` are two "
                "that do, and they fail at link with `__hash_memory` or "
                "`__atomic_notify_all_global_table` undefined",
                "declare the C++ standard library as a package, which brings "
                "its headers, its module and its objects as one release: "
                "[target.'cfg(os = \"ios\")'.dependencies] "
                "llvm.libcxx = \"22.1.8.1\" (and "
                "llvm.compiler-rt-builtins = \"22.1.8.5\" beside it)");
        }
    }
    return {};
}

static std::expected<void, std::string>
step11_std_module_availability_gate(PrepareState& state, bool needsStdModule) {
    if (needsStdModule && !state.tc->hasImportStd) {
        // A freestanding target reaches here for a reason the generic message
        // gets wrong. Nothing is missing from the toolchain — libc++'s std
        // module is right there — it is that `std` is ONE module over the whole
        // library, threads and filesystem and iostreams included, so there is
        // no subset of it to build without an OS. Saying "provides no std
        // module source" sends the reader to look for a broken payload.
        //
        // The line below is copy-pasteable, and that is a PROMISE: it has
        // to resolve today. It briefly did not — an earlier version of this
        // message named `mcpplibs.std.freestanding` before any such package
        // existed, so following the advice failed at the very next command
        // with "package not found" and sent the reader off to debug their
        // index. The package is published now (103 of libc++'s 110 headers,
        // measured; the 7 that fail fail on a hosted x86_64 too), so the line
        // is back. If it is ever removed from the index, this must change with
        // it.
        //
        // And the VERSION is part of the promise, not decoration — which is
        // how the same defect recurred in a second form. The line said "0.1.0"
        // after 0.2.0 superseded it in the index, and 0.1.0 is not published,
        // so pasting it produced
        //
        //     E_NOT_FOUND: package 'compat.std-freestanding@0.1.0' not found
        //     in the synced index
        //
        // measured 2026-08-20 while documenting this message. A floor would
        // not fix it either: the request has to name a version the index
        // actually carries. Publishing a new std-freestanding means updating
        // this literal in the same change.
        // THE QUESTION IS WHETHER A HOSTED STANDARD LIBRARY IS PRESENT, NOT
        // WHETHER THE TARGET IS FREESTANDING.
        //
        // Those were the same question for as long as no one had built one for
        // such a target, and they stopped being the same when someone did:
        // `mcpplibs/openkal-llvm-runtime' configures libc++, libc++abi and
        // libunwind for a machine with no operating system, and a program above
        // it has the library this refusal says it cannot have.
        //
        // The refusal is kept, because it is right in every case where nothing
        // supplies one --- which is still the ordinary case, and the advice
        // below is still the advice. What changes is that a package can now say
        // otherwise, and it says so the way every other capability is declared:
        //
        //     provides = ["hosted-standard-library"]
        //
        // A capability rather than a triple, because the fact is a property of
        // the graph and not of the target, and because dependency resolution is
        // the earliest time at which it is known.
        const bool hostedStdProvided =
            state.capProviders.find("hosted-standard-library") != state.capProviders.end();
        if (auto ft = mcpp::toolchain::triple::parse(state.tc->targetTriple);
            ft && ft->is_freestanding() && !hostedStdProvided)
        {
            return std::unexpected(std::format(
                "`import std;` is not available on '{}' — a freestanding target "
                "has no hosted standard library.\n"
                "       `std` is one module over the entire library (threads, "
                "filesystem, iostreams\n"
                "       included), so there is no subset of it to build without "
                "an OS underneath.\n"
                "       Use the freestanding subset instead — an ordinary "
                "dependency carrying\n"
                "       the parts of the library that need no OS "
                "(array, span, optional, atomic,\n"
                "       string_view, ranges, expected, charconv, coroutines):\n"
                "\n"
                "           [dependencies]\n"
                "           std-freestanding = \"0.2.0\"\n"
                "\n"
                "       then `import mcpplibs.std.freestanding;` in place of "
                "`import std;`.\n"
                "       The target's C library itself comes from the BOARD "
                "package (riscv-virt-rt\n"
                "       exports `mcpplibs.riscv_virt_rt`).",
                state.tc->targetTriple));
        }
        return std::unexpected(std::format(
            "source imports std but toolchain '{}' provides no std module source",
            state.tc->label()));
    }
    // `std.compat` is a second module of the same library, and not every
    // library has it: a package that supplies `std` alone, or a toolset without
    // `std.compat.ixx`. Said here, before any compile, instead of as the
    // compiler's "module 'std.compat' not found" inside the first unit that
    // imports it, which names neither the library nor the reason.
    if (state.needsStdCompat && state.tc->hasImportStd
        && state.tc->stdCompatSource.empty()) {
        return std::unexpected(std::format(
            "source imports std.compat but the standard library of toolchain '{}' "
            "({}) provides `std` without `std.compat`; import std and the C headers "
            "the code uses instead",
            state.tc->label(), state.tc->stdlibId.empty() ? "unknown" : state.tc->stdlibId));
    }
    // `import std` availability is two-dimensional once C++20 is a legal level:
    // having a std module source is not the same as being able to build it at
    // the project's level. Every toolchain mcpp ships answers 20; only an MSVC
    // STL older than microsoft/STL#3977 answers 23, and those users would
    // otherwise get an error from inside std.ixx.
    if (needsStdModule && state.tc->importStdMinLevel > 0
        && state.m->cppStandard.level < state.tc->importStdMinLevel) {
        return std::unexpected(std::format(
            "source imports std but toolchain '{}' provides the std module only "
            "from {} up, while [package].standard resolves to '{}'; raise the "
            "standard or drop `import std;`",
            state.tc->label(),
            mcpp::manifest::cpp_standard_level_name(state.tc->importStdMinLevel),
            state.m->package.standard));
    }
    return {};
}

static void step11_compute_fingerprint(PrepareState& state) {
    // Compute fingerprint (no lockfile in M1 → empty hash)
    mcpp::toolchain::FingerprintInputs fpi;
    fpi.toolchain            = *state.tc;
    fpi.cppStandard         = state.m->package.standard;
    // Target-keyed, not host-keyed (#685): the fingerprint must fold
    // `macos_deployment_target` whenever THIS BUILD's resolved toolchain
    // targets macOS, whether mcpp itself is running on Linux, Windows or
    // macOS — see the discriminator comment on `min_platform_version` and
    // on `canonical_configuration_flags`.
    const bool fpTargetIsMacos = [&] {
        auto fpTt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
        return fpTt && fpTt->os == "macos";
    }();
    // The configuration alone (workspace design 2026-09-29 §3): a package's own
    // flags are attributes of its node and reach its commands, not this name.
    fpi.compileFlags        = canonical_configuration_flags(*state.m, fpTargetIsMacos);
    // [c-abi] REALISATION AND `__OPENKAL__` PARTICIPATE IN THE FINGERPRINT
    // (design 2026-09-18 §3.4, gap #4 of the design's own self-review). Two
    // builds whose C library declares `data-model = "lp64"` and `"llp64"`
    // compile the SAME source, against the SAME manifest, into objects whose
    // `long` disagrees in width — sharing an output directory between them is
    // exactly the silent ABI mismatch §3.4 exists to rule out. Appended only
    // when non-empty (`tc->cEnvTokens` is empty whenever no `[c-abi]` block
    // resolved), so a graph that declares nothing keeps the directory it
    // already had.
    if (state.tc->kernelAbiIsOpenkal) fpi.compileFlags += " openkal-kernel-abi";
    for (auto& t : state.tc->cEnvTokens)        fpi.compileFlags += " cenv:" + t;
    for (auto& t : state.tc->cEnvBuiltinsTokens) fpi.compileFlags += " cenv:" + t;
    // A package opting OUT via `c-environment = "platform"` (§3.4) is an
    // attribute of that package's node: it changes that package's commands,
    // which ninja rebuilds, and not the configuration (workspace design
    // 2026-09-29 §3).
    // The module-edge schedule changes the SHAPE of build.ninja, and the fast
    // path replays that file without a plan to compare against. Folding the
    // switch into the fingerprint puts a differently-scheduled build in a
    // different directory, which makes replaying the wrong shape structurally
    // impossible instead of merely guarded. Only appended when non-default, so
    // existing build directories keep their identity.
    if (const auto sched = mcpp::build::schedule::requested_switch(*state.m);
        sched != "auto") {
        fpi.compileFlags += " #schedule=";
        fpi.compileFlags += sched;
    }
    // The device axis decides which sources compile and which cfg sections
    // apply, so two builds that differ only in it are two builds. Appended
    // only when set, so a project that asks for no accelerator keeps the
    // build directory it has.
    if (const auto accel = state.resolvedAccel(); !accel.empty()) {
        fpi.compileFlags += " #accel=";
        fpi.compileFlags += accel;
    }
    if (state.m->cppStandard.experimental) {
        // c++fly gate flags are derived (not manifest-declared): fold them in
        // so a cppfly table change across mcpp versions re-fingerprints.
        for (auto& f : mcpp::toolchain::cppfly::resolve(*state.tc).flags) {
            fpi.compileFlags += ' ';
            fpi.compileFlags += f;
        }
    }
    fpi.dependencyLockHash = "";    // M2
    fpi.stdBmiHash         = "";    // updated after stdmod build (chicken/egg ok for M1)
    state.fp = mcpp::toolchain::compute_fingerprint(fpi);
}

static std::expected<void, std::string>
step11_prebuild_std_module(PrepareState& state, bool needsStdModule) {
    // Pre-build std module only when the source graph actually imports it.
    if (needsStdModule) {
        // The std BMI must be compiled with the SAME dialect set its
        // importers use (issue #210: -freflection gates libstdc++'s <meta> —
        // a std BMI built without it structurally lacks std::meta). Both
        // pieces were already in the fingerprint; this fixes the COMMAND
        // construction the fingerprint promised (stdFlagAndDialect above).
        // #422/#718: the CRT model reaches the std module too, on EVERY
        // MSVC-ABI row now (cl and clang++ targeting `*-windows-msvc` alike).
        // `msvc_abi_crt_word` is the SAME helper flags.cppm uses for the
        // project's TUs, so the two cannot drift; it is empty off the MSVC
        // ABI, where the gcc and clang std module builders leave it unread.
        const auto stdCrt = mcpp::toolchain::msvc_abi_crt_word(
            *state.tc, mcpp::toolchain::msvc_wants_static_crt(
                           state.m->buildConfig.linkage, state.m->buildConfig.cxxRuntime));
        // Whether THIS build's resolved toolchain targets macOS — the same
        // target-not-host discriminator `min_platform_version` uses, parsed
        // locally because `tc` (not a `triple::Triple`) is what is in scope
        // here (#685).
        const bool stdTargetIsMacos = [&] {
            auto stdTt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
            return stdTt && stdTt->os == "macos";
        }();
        if (state.overrides.plan_only) {
            // Described, not compiled: the paths and commands are the ones
            // ensure_built would use, from the one derivation in stdmod.cppm.
            auto described = mcpp::toolchain::describe_std_module(
                *state.tc, state.m->package.standard, state.stdFlagAndDialect,
                mcpp::platform::macos::deployment_target(
                    stdTargetIsMacos, state.m->buildConfig.macosDeploymentTarget),
                mcpp::toolchain::default_cache_root(), stdCrt,
                state.needsStdCompat);
            if (!described) {
                refusal::record(refusal::Code::StdModulePrecompile);
                return std::unexpected(described.error().message);
            }
            state.stdBmiPath          = described->bmiPath;
            state.stdObjectPath       = described->objectPath;
            state.stdCompatBmiPath    = described->compatBmiPath;
            state.stdCompatObjectPath = described->compatObjectPath;
            state.describedStdModule  = std::move(*described);
        } else {
            auto sm = mcpp::toolchain::ensure_built(
                *state.tc, state.m->package.standard, state.stdFlagAndDialect,
                mcpp::platform::macos::deployment_target(
                    stdTargetIsMacos, state.m->buildConfig.macosDeploymentTarget),
                mcpp::toolchain::default_cache_root(), stdCrt,
                state.needsStdCompat);
            if (!sm) {
                // THE ONE CODE IN THE TAXONOMY THAT NOTHING WROTE.
                //
                // `Code::StdModulePrecompile` has existed, with a name and a
                // comment, since the taxonomy was written; `grep` for it found the
                // declaration and the `name()` arm and no third site. So every
                // std-module refusal reported `other`, which is the bucket
                // refusal.cppm defines as "a refusal that has not been given a code
                // yet" -- a visible admission, and one nobody had cashed.
                //
                // Measured: `tests/matrix/expected.tsv` carried exactly ONE `other`
                // row out of 176, `x86_64-windows-msvc x llvm@22.1.8` in graph mode,
                // and `scan.sh` printed it under "无名拒绝" on every Windows run.
                // The sentence was right and the classification was missing --
                // the same shape `Code::HostToolToolchain` was added for.
                refusal::record(refusal::Code::StdModulePrecompile);
                return std::unexpected(sm.error().message);
            }
            state.stdBmiPath = sm->bmiPath;
            state.stdObjectPath = sm->objectPath;
            state.stdCompatBmiPath = sm->compatBmiPath;
            state.stdCompatObjectPath = sm->compatObjectPath;
            // C5 / D5a (design 2026-09-26 §3.5): compile_commands.json and the
            // S1 document list the standard-library units too, so the plan
            // needs the commands mcpp ran to build them (§13396 below), not
            // only their output paths. `describe_std_module` is the pure
            // derivation `ensure_built` itself reads before running anything
            // (mcpp.toolchain.stdmod's header); calling it again here starts
            // no process and cannot name a different command or directory.
            // A failure here is not this build's failure -- `ensure_built`
            // above already succeeded with the same inputs -- so it only
            // means the description is unavailable for the plan, silently.
            auto described = mcpp::toolchain::describe_std_module(
                *state.tc, state.m->package.standard, state.stdFlagAndDialect,
                mcpp::platform::macos::deployment_target(
                    stdTargetIsMacos, state.m->buildConfig.macosDeploymentTarget),
                mcpp::toolchain::default_cache_root(), stdCrt,
                state.needsStdCompat);
            if (described) state.describedStdModule = std::move(*described);
        }
    }
    return {};
}

// SPEC-008 W3 (#734 E6): the package being built imports a module of a
// dependency that states an interface root, and that module is not one of the
// dependency's public modules (the root and what it re-exports with
// `export import`, transitively). The build succeeds from source; against the
// dependency's packed form it would not, because a packed library ships its
// public modules' closure only. Warned once per module, and never for a
// dependency without a root, which states no interface.
static void step11_public_module_check(PrepareState& state) {
    if (state.packages.empty()) return;
    // The re-exports are read by the text scanner only; a P1689 scan reports
    // imports without saying which are `export import`, and every re-exported
    // module would then read as private. No reading, no warning.
    if (const char* sel = std::getenv("MCPP_SCANNER"); sel && std::string_view(sel) == "p1689")
        return;
    const auto& g = state.scan.graph;
    auto qualified = [](const mcpp::manifest::Manifest& m) {
        return m.package.namespace_.empty() ? m.package.name
                                            : m.package.namespace_ + "." + m.package.name;
    };
    auto primary = [](std::string_view name) {
        return std::string(name.substr(0, name.find(':')));
    };
    const std::string rootName = qualified(state.packages[0].manifest);

    // The package a root import means, by the resolver every other reader
    // uses (mcpp#732): a name two packages provide means the one in the
    // root's closure.
    auto providerOf = [&](const std::string& prim) -> std::optional<std::string> {
        if (auto p = mcpp::modgraph::resolve_provider(g, rootName, prim))
            return g.units[*p].packageName;
        return std::nullopt;
    };
    std::map<std::string, std::vector<const mcpp::modgraph::SourceUnit*>> unitsOf;
    for (auto const& u : g.units) {
        if (!u.provides) continue;
        unitsOf[primary(u.provides->logicalName)].push_back(&u);
    }

    // A member of the root's own workspace is built from source together with
    // the root, whichever form it is published in, so the packed-form
    // consequence W3 states does not arise for it (SPEC-008 §4).
    std::map<std::string, std::set<std::string>> publicOf;
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        auto const& pr = state.packages[i];
        if (!workspace_member_of(state, pr.root).empty()) continue;
        const auto rootFile = (pr.root / mcpp::manifest::resolve_lib_root_path(pr.manifest, pr.root))
                                  .lexically_normal();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(rootFile, ec)) continue;
        const mcpp::modgraph::SourceUnit* rootUnit = nullptr;
        for (auto const& u : g.units)
            if (u.path.lexically_normal() == rootFile && u.provides) { rootUnit = &u; break; }
        if (!rootUnit) continue;
        std::set<std::string> pub{primary(rootUnit->provides->logicalName)};
        std::vector<std::string> work(pub.begin(), pub.end());
        while (!work.empty()) {
            auto mod = work.back(); work.pop_back();
            for (auto const* u : unitsOf[mod])
                for (auto const& re : u->reexports)
                    if (auto p = primary(re.logicalName); pub.insert(p).second) work.push_back(p);
        }
        publicOf[qualified(pr.manifest)] = std::move(pub);
    }

    std::set<std::string> warned;
    for (auto const& u : g.units) {
        if (u.packageName != rootName) continue;
        for (auto const& req : u.requires_) {
            const auto prim = primary(req.logicalName);
            auto prov = providerOf(prim);
            if (!prov || *prov == rootName) continue;
            auto pub = publicOf.find(*prov);
            if (pub == publicOf.end() || pub->second.contains(prim)) continue;
            if (!warned.insert(prim).second) continue;
            std::string names;
            for (auto const& n : pub->second) { if (!names.empty()) names += ", "; names += n; }
            mcpp::diag::report(
                mcpp::diag::Severity::Warning, "build/interface",
                std::format("'{}' imports '{}' of '{}', which is not one of that "
                            "package's public modules", u.relPath.generic_string(),
                            prim, *prov),
                std::format("the build succeeds from source, and fails against the "
                            "packed form of '{}'", *prov),
                std::format("import a public module of '{}': {}", *prov, names));
        }
    }
}

std::expected<void, std::string> phase11_scan(PrepareState& state) {
    auto needsStdModule = step11_scan_sources(state);
    if (!needsStdModule) return std::unexpected(needsStdModule.error());
    step11_public_module_check(state);

    if (auto r = step11_dependency_standard_scope_check(state); !r)
        return std::unexpected(r.error());

    if (auto r = step11_dialect_flag_reaches_std_prebuild(state, *needsStdModule); !r)
        return std::unexpected(r.error());

    if (auto r = step11_msvc_crt_word_check(state); !r)
        return std::unexpected(r.error());

    if (auto r = step11_package_std_module_source(state); !r)
        return std::unexpected(r.error());

    if (auto r = step11_apple_sdk_cxx_runtime(state, *needsStdModule); !r)
        return std::unexpected(r.error());

    if (auto r = step11_std_module_availability_gate(state, *needsStdModule); !r)
        return std::unexpected(r.error());

    step11_compute_fingerprint(state);

    if (auto r = step11_prebuild_std_module(state, *needsStdModule); !r)
        return std::unexpected(r.error());

    if (state.print_fingerprint) {
        std::println("Toolchain: {}", state.tc->label());
        std::println("Fingerprint: {}", state.fp.hex);
        for (std::size_t i = 0; i < state.fp.parts.size(); ++i) {
            std::println("  [{}] {}", i + 1, state.fp.parts[i]);
        }
    }

    return {};
}

} // namespace mcpp::build
