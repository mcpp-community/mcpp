// target_side.cpp -- P9 and P10: the target side resolved against the
// graph, and the form each dependency is linked in.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpp.home;
import mcpp.platform.axis;
import mcpp.libs.json;
import mcpp.log;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.graph;
import mcpp.build.progress;   // the members' programs wait in order (design 2026-09-29 §4.2)
import mcpp.build.schedule.policy;   // resolve_jobs — how many programs compile at once (#748, B2)
import mcpp.platform.capacity;       // the host fallback when no job count was stated
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.cenv;        // [c-abi] declaration → compiler configuration (design 2026-09-18)
import mcpp.toolchain.cenv_probe;  // [c-abi] declaration is checked, not trusted (design §3.2)
import mcpp.toolchain.predefines;  // the macros this engine defines: contract and emission in one module
import mcpp.toolchain.cppfly;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
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
import mcpp.build.linkage_form;   // #519 — which form each dependency takes
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.toolchain.post_install;
import mcpp.platform;
import mcpp.ui;
import mcpp.log;
import mcpp.wire;               // Severity, for PlanNote (#699 item 2, E3)

namespace mcpp::build {

// STEP FUNCTIONS (mcpp#722 / T6), one per section phase9's own banners
// already named. Statements moved verbatim; two scoping braces that
// wrapped several sections at once (no matching close inside any one
// of them) are dropped as redundant, the same treatment graph.cpp and
// features.cpp needed for their own such wrappers.

// Hoisted from a local declaration inside phase9_target_side (mcpp#722 /
// T6): `byLayer`'s element type, needed by the struct that now carries
// gather state across step boundaries.
struct TargetSideCandidate { mcpp::targetside::Provider p; bool direct; std::size_t index = 0; };

// The locals phase9_target_side's first section (candidate gathering)
// used to declare and every later section still reads: the PrepareState
// pattern, one level deeper, for one phase's own steps.
struct TargetSideGather {
    mcpp::targetside::Inputs in;
    std::map<int, std::vector<TargetSideCandidate>> byLayer;
    std::vector<mcpp::targetside::Requirement> requirements;
};

static std::expected<TargetSideGather, std::string>
step9_gather_target_side_candidates(PrepareState& state) {
    namespace tsd = mcpp::targetside;
    TargetSideGather gather;
        namespace tsd = mcpp::targetside;

        // Scan the graph once for every layer. A package declares the layer it
        // supplies and, optionally, the interface name it answers to:
        //
        //     provides = ["mcpp:kernel-abi=openkal"]
        //
        // The engine knows the five layer names and nothing about the
        // implementations that fill them. `hosted-standard-library` is accepted
        // for the C++ layer as the spelling that shipped before this one, so an
        // existing package keeps working unchanged.
        //
        // ONE SUPPLIER PER LAYER, AND TWO IS AN ERROR RATHER THAN A PICK.
        // A C library, a kernel interface and a C++ runtime are mutually
        // exclusive choices; the same rule already governs `[build] runner` for
        // the same reason. Until this scan collected candidates instead of
        // keeping the first acceptable one, two suppliers resolved by graph
        // traversal order — an order the author neither writes nor can predict —
        // and the loser's `[build]` section still reached the command line.
        // `index` — WHICH PACKAGE this candidate is, not just its name.
        //
        // Needed once resolution is done: a layer supplied from the graph
        // publishes an include set the WHOLE build must see (see
        // `targetSideUsage` below), and reaching that package by name would be
        // a second lookup of something already in hand.
        using Candidate = TargetSideCandidate;
                
        // The root's own edges, and in a workspace plan the selected members'
        // (`PrepareState::declaredByRoot`), whose dependencies the author
        // wrote as directly as a root's.
        std::vector<const std::map<std::string, mcpp::manifest::DependencySpec>*> rootDeps{
            &state.m->dependencies};
        for (std::size_t i = 1; i < state.packages.size(); ++i)
            if (state.packages[i].selectedMember)
                rootDeps.push_back(&state.packages[i].manifest.dependencies);
        auto is_direct = [&](std::string_view name) {
            for (auto const* deps : rootDeps)
                for (auto const& [k, _] : *deps) {
                    if (k == name) return true;
                    // Selectors are `<namespace>.<name>` or a bare tail; a tail
                    // match is what the author sees in their own manifest.
                    if (k.size() > name.size() && k.ends_with(name)
                        && k[k.size() - name.size() - 1] == '.')
                        return true;
                }
            return false;
        };

        for (std::size_t pkgIndex = 0; pkgIndex < state.packages.size(); ++pkgIndex) {
            auto const& pkg = state.packages[pkgIndex];
            const auto pkgId = pkg.manifest.package.version.empty()
                ? pkg.manifest.package.name
                : std::format("{}@{}", pkg.manifest.package.name,
                              pkg.manifest.package.version);

            // EVERY PACKAGE KIND, NOT ONLY THE ONES WITH AN XPKG
            // DESCRIPTOR. `warn_unknown_xpkg_keys` reaches a dependency
            // resolved through the index; a path or git dependency carries a
            // manifest of its own and reached no warning at all, so a layer
            // this engine does not know went by in silence. This loop sees
            // every package in the graph.
            for (auto const& cap : pkg.manifest.unknownCapabilities) {
                if (&pkg == &state.packages.front()) continue;   // root: already refused
                // The same text the root's refusal carries, including the list
                // of layers that do exist. A warning that says less than the
                // error it replaced would be a worse diagnostic wearing a
                // milder severity.
                auto why = tsd::parse_capability(cap);
                mcpp::ui::warning(std::format(
                    "package '{}': {}\n"
                    "       Ignored, and this build proceeds without that layer. "
                    "A newer mcpp may resolve it.",
                    pkgId,
                    why ? std::format("`{}` names no capability mcpp knows.", cap)
                        : why.error()));
            }

            for (auto const& entry : pkg.manifest.provides) {
                std::optional<tsd::CapDecl> decl;
                if (auto parsed = tsd::parse_capability(entry); parsed && *parsed)
                    decl = **parsed;
                else if (entry == "hosted-standard-library")
                    decl = tsd::CapDecl{ tsd::CapLayer::CxxAbi, {} };
                if (!decl) continue;
                if (!tsd::layer_is_suppliable_by_package(decl->layer)) {
                    return std::unexpected(std::format(
                        "package '{}' declares `provides = [\"{}\"]`, and the "
                        "compiler is not a layer a package can supply.\n"
                        "       A compiler is a payload this engine installs and "
                        "drives; the differences between families are things the "
                        "engine must know rather than data a package can "
                        "describe.\n"
                        "       A package may REQUIRE one: `requires = "
                        "[\"mcpp:compiler=<family>\"]`.",
                        pkgId, entry));
                }

                tsd::Provider p;
                p.name          = pkg.manifest.package.name;
                p.version       = pkg.manifest.package.version;
                p.interfaceName = decl->interfaceName;
                p.hasStdModule  = !pkg.manifest.stdModule.empty();
                p.cAbiDecl      = pkg.manifest.cAbiDecl;

                auto& slot = gather.byLayer[static_cast<int>(decl->layer)];
                // A package may carry both spellings during the transition, and
                // the array order is the author's, not a preference. Two entries
                // from the SAME package are one supplier; the current spelling
                // names the interface and the older one cannot, so the entry
                // that carries an interface name wins.
                auto same = std::find_if(slot.begin(), slot.end(),
                    [&](const Candidate& c){ return c.p.name == p.name; });
                if (same != slot.end()) {
                    // `index` MOVES WITH `p` AND NOT ON ITS OWN. The two
                    // describe one package, and this branch is reached only
                    // from the same `pkgIndex` today — a package carrying both
                    // spellings — so they cannot differ yet. Tying them keeps
                    // it that way if a second package ever reaches here.
                    if (same->p.interfaceName.empty() && !p.interfaceName.empty()) {
                        same->p     = p;
                        same->index = pkgIndex;
                    }
                } else {
                    slot.push_back({ p, is_direct(p.name), pkgIndex });
                }
            }

            // `requires` — the symmetric half. An entry naming a layer this
            // engine does not know is an error for the same reason a `provides`
            // one is: a typo would otherwise disable a check silently.
            for (auto const& entry : pkg.manifest.requires_) {
                auto parsed = tsd::parse_capability(entry);
                // An unknown layer name is reported where the manifest was
                // read — as an error for the root and a warning for a
                // dependency — so it is skipped rather than refused twice.
                if (!parsed || !*parsed) continue;
                gather.requirements.push_back({ pkgId, (*parsed)->layer,
                                         (*parsed)->interfaceName });
            }
        }

        for (auto const& [layerInt, slot] : gather.byLayer) {
            if (slot.size() < 2) continue;
            tsd::Conflict c;
            c.layer     = static_cast<tsd::CapLayer>(layerInt);
            c.first     = slot[0].p.id();
            c.firstVia  = slot[0].direct ? "" : "a transitive dependency";
            c.second    = slot[1].p.id();
            c.secondVia = slot[1].direct ? "" : "a transitive dependency";
            return std::unexpected(tsd::format_conflict(c));
        }

        auto provider_of = [&](tsd::CapLayer want)
            -> std::optional<tsd::Provider> {
            auto it = gather.byLayer.find(static_cast<int>(want));
            if (it == gather.byLayer.end() || it->second.empty()) return std::nullopt;
            return it->second.front().p;
        };

        auto& in = gather.in;
        if (state.tc) {
            if (auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple)) {
                in.llvmTriple         = tt->llvm_triple(
                    min_platform_version(*state.m, *tt, state.tc->binaryPath));
                in.targetOs           = tt->os;
                in.targetEnv          = tt->env;
                in.freestandingTarget = tt->is_freestanding();
                // NOT `tt->envExplicit`. By this line the triple has been
                // canonicalised, and the canonical form of `x86_64-linux` is
                // `x86_64-linux-gnu` — re-parsing it reports a segment the
                // project never wrote. The request was captured upstream, where
                // the distinction still existed.
                in.requestedCAbi = state.requestedCAbi;
                if (!state.requestedCAbi.empty()) {
                    auto bare = *tt; bare.env.clear();
                    in.requestFreeTarget = bare.str();
                }
                // The segment names a different axis on each platform, and
                // saying WHICH lets the report gloss it instead of merely
                // withholding a warning. Only the C-library case can contradict
                // what the graph resolved; the other two are simply a different
                // question, and the report says so.
                in.envAxis =
                    tt->os == "linux"   ? tsd::EnvAxis::CLibrary
                  : tt->os == "windows" ? tsd::EnvAxis::ObjectAbi
                  : tt->is_freestanding() ? tsd::EnvAxis::ObjectFormat
                                          : tsd::EnvAxis::Unknown;

                // `sysroot = ""` and "no sysroot key" are different answers and
                // must not be collapsed: the first says this project wants no
                // prebuilt C library, the second says it did not say.
                // On an MSVC-ABI row the key names the MSVC toolset, which the
                // toolchain binding consumes (`bind_msvc_sysroot`); it is not a
                // C library package for this model to report as one.
                if (tt->is_msvc_env())
                    ;
                else if (auto const* ovr = sysroot_override(*state.m, *tt); ovr && ovr->empty())
                    in.sysrootDeclaredEmpty = true;
                else
                    in.sysrootXpkg = mcpp::toolchain::triple::effective_sysroot(
                        *tt, sysroot_override(*state.m, *tt));
            }
            in.payloadLibcRef      = state.tc->targetSysrootPkg;
            in.payloadCxxInterface = state.tc->stdlibId;
            // The compiler is a layer, and it is the one layer no package can
            // supply. It enters here so that a requirement has something to be
            // checked against and so the report can show the whole stack.
            in.compilerFamily  = std::string(state.tc->compiler_family());
            in.compilerVersion = state.tc->version;
            // WHETHER THE PAYLOAD HAS A COMPILER RUNTIME FOR AN APPLE CROSS
            // TARGET, read from the payload's own resource directory. Clang's
            // Darwin driver adds `libclang_rt.<platform>.a` from there when
            // the file exists and continues silently when it does not, and
            // the official payload builds only the macOS archive (measured,
            // 22.1.8: `lib/clang/22/lib/darwin/` holds `libclang_rt.osx.a` and
            // no `ios` or `iossim`). The consequence without this line is
            // `__isPlatformVersionAtLeast` undefined at link with nothing
            // said earlier (mcpp#630). The engine never looks in Xcode for the
            // archive: a compiler runtime the payload lacks is a graph
            // package, as it is on the bare rows.
            if (!state.tc->appleSdkRoot.empty()) {
                if (auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
                    tt && tt->is_ios()) {
                    const std::string archive = std::format(
                        "libclang_rt.{}.a", tt->is_ios_simulator() ? "iossim" : "ios");
                    const auto payloadRoot =
                        state.tc->binaryPath.parent_path().parent_path();
                    bool found = false;
                    std::error_code ec;
                    for (auto const& ver : std::filesystem::directory_iterator(
                             payloadRoot / "lib" / "clang", ec)) {
                        if (std::filesystem::exists(
                                ver.path() / "lib" / "darwin" / archive, ec)) {
                            found = true;
                            break;
                        }
                    }
                    in.payloadCompilerRuntimeAbsent = !found;
                }
            }
        }
        in.compilerRuntime = provider_of(tsd::CapLayer::CompilerRuntime);
        in.kernelAbi       = provider_of(tsd::CapLayer::KernelAbi);
        in.cAbi            = provider_of(tsd::CapLayer::CAbi);
        in.cxxAbi          = provider_of(tsd::CapLayer::CxxAbi);

        // A ROW THAT LINKS THROUGH lld DIRECTLY, ON A PAYLOAD WITH NO lld.
        //
        // `x86_64-none-elf` is the only row carrying an `lldEmulation`, and its
        // column comment in mcpp.freestanding.target says why the driver is
        // bypassed for it: the driver "would hand the link to a host `g++` that
        // cannot take our linker's path".
        //
        // MEASURED TWICE ON windows-2022, AND THE SECOND TIME WAS MY OWN
        // FALLBACK. First, an empty `resolve_lld` left linker vocabulary on a
        // driver line:
        //
        //     clang++: error: unknown argument: '-m'
        //
        // Then, falling back to the driver line reproduced exactly what the
        // bypass exists to prevent:
        //
        //     clang++: error: linker (via gcc) command failed
        //     collect2.exe: error: ld returned 1 exit status
        //
        // There is no third shape. The row needs lld by name; when the
        // payload has none, the answer is a refusal at the decision, not a
        // different link.
        if (auto fsT = state.tc.has_value()
                     ? mcpp::toolchain::triple::parse(state.tc->targetTriple)
                     : std::nullopt;
            fsT && fsT->is_freestanding()) {
            auto fsSpec = mcpp::freestanding::resolve(*fsT);
            if (fsSpec && !fsSpec->lldEmulation.empty()
                && mcpp::freestanding::resolve_lld(state.tc->binaryPath).empty()) {
                refusal::record(refusal::Code::LldRequiredAbsent);
                return std::unexpected(std::format(
                    "target '{}' links through lld directly, and this toolchain "
                    "payload ships none.\n"
                    "       The row carries an lld emulation ('{}'), which means "
                    "the compiler driver is\n"
                    "       bypassed — for this target it would hand the link to "
                    "a host linker that\n"
                    "       cannot take a freestanding ELF.\n"
                    "       install a toolchain whose payload contains ld.lld, "
                    "or build this target\n"
                    "       from a host that has one.",
                    fsT->str(), fsSpec->lldEmulation));
            }
        }

    return gather;
}

static std::expected<void, std::string>
step9_resolve_and_realise_cabi(PrepareState& state, TargetSideGather& gather) {
    namespace tsd = mcpp::targetside;
    auto& in = gather.in;
        state.resolvedTargetSide = tsd::resolve(in);
        state.targetSideResolved = true;

        // `__OPENKAL__` — design 2026-09-18 §2.1, §3.4. Read from the
        // resolved LAYER's interface name, never from a package name, so a
        // second implementation of `mcpp:kernel-abi=openkal` needs no engine
        // change. Applies to every target-side unit unconditionally — even
        // one that declares `c-environment = "platform"`, because the
        // exception in §3.4 is about the C ENVIRONMENT a package sees, not
        // about whether `kal_*` may be called from it.
        if (state.tc) state.tc->kernelAbiIsOpenkal =
            state.resolvedTargetSide.kernelAbi.interfaceName == "openkal";

        // [c-abi] REALISATION — design §3.2-§3.4. Everything below is
        // skipped, and every command line unchanged, for the graph this
        // engine has always built.
        //
        // THE TEST IS `declared`, NOT THE OPTIONAL. `TargetSide::cAbiDecl` is
        // also set by a `[c-abi-absent]` table on a provider that wrote no
        // `[c-abi]` block, and those absences are diagnostic data with
        // nothing in them to realise — `cenv::realise` requires `declared`
        // (cenv.cppm) and would be reading fields nobody wrote.
        if (state.tc && state.resolvedTargetSide.cAbiDecl
            && state.resolvedTargetSide.cAbiDecl->declared) {
            // `cenv::realise` FIRST, THE COMPILER-FAMILY GATE SECOND — not
            // the other way around (coordinator report, openkal-musl 0.15.0
            // regression: GCC on Linux refused for a declaration
            // `presents = "posix", data-model = "arch-default", wchar = 32,
            // builtins = "iso"` that Linux/x86_64's own default ALREADY
            // satisfies, needing no substitution at all — a gratuitous
            // refusal that lost the package for every GCC user on Linux).
            // `cenv::realise` is a pure function of the declaration and the
            // TARGET, not of the compiler (`mcpp.toolchain.cenv`'s own
            // module header) — computing it before asking anything about the
            // compiler is what lets an EMPTY realisation answer "does this
            // compiler need to be Clang" correctly: no. The Clang-specific
            // mechanisms (`--target=` substitution, `-f[no-]short-wchar`,
            // the `builtins = "iso"` flags) are only needed when realisation
            // actually produces tokens; when it produces none, the target's
            // own default already IS the declaration, and any compiler that
            // can run the verification probe below (`-E -dM`, not a
            // Clang-specific flag) can be trusted to have gotten there —
            // which is exactly what that probe then confirms rather than
            // assumes. The Windows/GCC case is UNCHANGED by this: there the
            // realisation is non-empty (the Cygwin-flavoured substitution),
            // and MinGW's `long` is 32-bit regardless of flags (openkal-musl
            // measured), so the gate below still refuses it.
            auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
            auto realised = mcpp::toolchain::cenv::realise(
                *state.resolvedTargetSide.cAbiDecl, tt ? tt->os : std::string{},
                tt ? tt->arch : std::string{}, tt && tt->is_freestanding());
            if (!realised) {
                refusal::record(refusal::Code::CEnvUnrealisable);
                return std::unexpected(realised.error());
            }
            if ((!realised->tokens.empty() || !realised->builtinsTokens.empty())
                && !mcpp::toolchain::is_clang(*state.tc)) {
                refusal::record(refusal::Code::CEnvUnrealisable);
                return std::unexpected(std::format(
                    "the C library ('{}', {}) declares [c-abi] whose "
                    "realisation for this target requires Clang-specific "
                    "substitution, and this build's compiler ('{}') cannot "
                    "carry it out.\n"
                    "       [c-abi] is realised, for targets that need "
                    "anything at all, with Clang-specific mechanisms — a "
                    "`--target=` substitution and `-f[no-]short-wchar` — so "
                    "a Clang toolchain is required for this target while "
                    "this declaration is in the graph.\n"
                    "       Select one: [toolchain] default = \"llvm@<version>\", "
                    "or [target.<triple>] toolchain = \"llvm@<version>\".",
                    state.resolvedTargetSide.cAbi.interfaceName,
                    state.resolvedTargetSide.cAbi.impl, state.tc->compiler_family()));
            }
            state.tc->cEnvTokens          = realised->tokens;
            state.tc->cEnvBuiltinsTokens  = realised->builtinsTokens;
            state.tc->cEnvExpectWcharBits = realised->expectWcharBits;
            state.tc->cEnvExpectLongBytes = realised->expectLongBytes;
            state.tc->cEnvExpectDefined   = realised->expectDefined;
            state.tc->cEnvExpectUndefined = realised->expectUndefined;

            // VERIFICATION, NOT TRUST (design §3.2). The probe's argv is the
            // IDENTITY-AFFECTING SUBSET of the real command line — the
            // `--target=` substitution and the `-U`/`-f[no-]short-wchar`
            // tokens `cenv::realise` just produced — because those are the
            // only tokens that change what a compiler predefines; include
            // paths and library search flags do not, and leaving them out
            // is what makes this probe cheap AND cacheable across every
            // package that shares this build's target side.
            //
            if (state.tc->cEnvExpectWcharBits != 0 || state.tc->cEnvExpectLongBytes != 0
                || !state.tc->cEnvExpectDefined.empty()
                || !state.tc->cEnvExpectUndefined.empty()) {
                // THE FREESTANDING TARGET HAD NEVER REACHED THE PROBE, AND
                // THAT IS WHY 2026.9.18.3 MEASURED THE HOST (mcpp#674
                // review, 2026-09-20). `Toolchain::crossTargetFlag` is set
                // for a HOSTED target only — the assignment above states the
                // reason: a freestanding target carries its own `--target`
                // together with the ISA flags that must accompany it, and a
                // second one there would be the same decision in two places.
                // That other place is `mcpp.freestanding.linkline`, which the
                // real compile goes through and this probe did not.
                // `cenv::realise` adds no `--target` for a freestanding
                // target either, so the probe ran with NO target selection at
                // all and clang answered for the machine it was running on.
                //
                // Measured: the probe's own argv shape on a Linux host
                // (`-D__unix__ -fno-short-wchar -ffreestanding -x c++ -E -dM
                // -`) reports `__linux__`; with `--target=riscv64-none-elf`
                // it reports `__riscv`, no `__linux__`, and
                // `__SIZEOF_WCHAR_T__` 4. A Windows host answered `_WIN32`
                // and 2 for the same reason, and 2026.9.18.3 read that as the
                // `--target=` substitution failing to strip host predefines.
                // Clang's predefines follow the target; there was no
                // substitution to fail.
                //
                // `hostStripMacros` is therefore GONE, and its removal is the
                // point rather than a tidy-up: `-U_WIN32 -U_WIN64
                // -U__MINGW32__ -U__MINGW64__` deleted the one piece of
                // evidence that said the probe was measuring the wrong
                // machine. A future host leak, if one exists, must reach the
                // mismatch report rather than be undefined before it can.
                //
                // THE ASSEMBLY REFUSES THE OMISSION rather than this site
                // remembering not to make it — `cenv_probe::assemble_argv`
                // holds the invariant, and the unit tests reach it without a
                // cross toolchain.
                std::vector<std::string> freestandingFlags;
                if (tt && tt->is_freestanding()) {
                    auto spec = mcpp::freestanding::resolve(*tt);
                    if (spec) {
                        freestandingFlags.push_back(
                            "--target=" + std::string(spec->triple));
                        for (auto const& f : mcpp::freestanding::compile_flags(*spec))
                            freestandingFlags.push_back(f);
                    }
                }
                auto assembled = mcpp::toolchain::cenv_probe::assemble_argv(
                    state.tc->crossTargetFlag, freestandingFlags,
                    state.tc->cEnvTokens, state.tc->cEnvBuiltinsTokens,
                    tt && tt->is_freestanding(), state.tc->targetTriple);
                if (!assembled) {
                    refusal::record(refusal::Code::CEnvUnrealisable);
                    return std::unexpected(assembled.error());
                }
                const auto& probeArgv = *assembled;
                auto probe = mcpp::toolchain::cenv_probe::verify(
                    state.tc->binaryPath, probeArgv,
                    state.tc->cEnvExpectWcharBits, state.tc->cEnvExpectLongBytes,
                    state.tc->cEnvExpectDefined, state.tc->cEnvExpectUndefined,
                    mcpp::home::cache_root());
                if (!probe) {
                    refusal::record(refusal::Code::CEnvUnrealisable);
                    return std::unexpected(probe.error());
                }
                if (!probe->mismatches.empty()) {
                    refusal::record(refusal::Code::CEnvVerificationMismatch);
                    std::string lines;
                    for (auto& mm : probe->mismatches)
                        lines += std::format(
                            "\n         {:<24} declared {:<10} measured {}",
                            mm.fact, mm.declared, mm.measured);
                    return std::unexpected(std::format(
                        "the C library's [c-abi] declaration does not match "
                        "what the compiler actually produced for '{}'.{}\n"
                        "       A declaration is checked, never trusted "
                        "(design 2026-09-18 §3.2) — the mismatch above was "
                        "measured from the compiler's own predefined macros, "
                        "compiled with the exact tokens this build derived "
                        "from the declaration.",
                        state.tc->targetTriple, lines));
                }
            }
        }

        // REPORTED, NOT REFUSED (mcpp#662, D4). `mcpp.toolchain.hostflags`
        // closes the compiler's own C-library search with `-nostdlibinc`
        // when a package supplies the target's C library (M1) — but only on
        // a Clang-family driver; GCC has no one-token equivalent
        // (`hostflags.cppm`'s own note on the shape it would need). Silently
        // building unisolated was ruled out once already: the resolver
        // refusing the combination outright was ALSO tried and reverted —
        // it fired before `format_report` below and broke three existing
        // e2e fixtures (268, 282, 303) that use a synthetic C-library
        // provider on this host's native, GCC-default target to assert
        // something else entirely, none of them about isolation. A
        // degradation is the third option: it changes no command line
        // (this branch decides nothing `hostflags.cppm` does not already
        // decide on its own), and it does not stop a build the previous
        // release would have allowed — it names, once, the gap the previous
        // release left silent.
        if (state.tc && state.resolvedTargetSide.cAbi.fromGraph()
            && !mcpp::toolchain::is_clang(*state.tc)) {
            mcpp::diag::degraded("target/c-abi-isolation", std::format(
                "the target's C library ('{}', {}) comes from the "
                "dependency graph, and the resolved compiler ('{}') has no "
                "way to stop its own driver from also searching the host's "
                "C library headers",
                state.resolvedTargetSide.cAbi.interfaceName,
                state.resolvedTargetSide.cAbi.impl, state.tc->compiler_family()),
                "a host header can still satisfy an #include the graph's "
                "own headers do not, silently — a Clang-family toolchain "
                "closes that search entirely (docs/22 'Adaptation To The "
                "Resolved Target Side')",
                "add [toolchain] default = \"llvm@<version>\", or for one "
                "target only [target.<triple>] toolchain = \"llvm@<version>\"");
        }

        // REPORTED ONCE, NOT REFUSED. A program that never reaches an
        // availability check links and runs without the archive; refusing it
        // would trade a diagnosed hazard for a regression. The degradation
        // names the platform, the file and the package that supplies it.
        if (state.resolvedTargetSide.compilerRuntime.absent() && state.tc) {
            auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
            mcpp::diag::degraded("target/compiler-runtime", std::format(
                "the toolchain payload carries no compiler runtime for {} "
                "(no libclang_rt.{}.a under its lib/clang/*/lib/darwin), and no "
                "package in the graph provides mcpp:compiler-runtime",
                state.tc->targetTriple,
                tt && tt->is_ios_simulator() ? "iossim" : "ios"),
                "a program that reaches an availability check "
                "(`__builtin_available`, or a system header that uses it) fails "
                "at link with `__isPlatformVersionAtLeast` undefined",
                "declare `llvm.compiler-rt-builtins` under the target's "
                "[target.'cfg(os = \"ios\")'.dependencies]");
        }

        // RECORDED ON THE TOOLCHAIN THE MOMENT IT IS KNOWN, because three
        // producers of a compile line need it and only one of them can see
        // `resolvedTargetSide`.
        //
        // `flags.cppm` reads `plan.targetSide` directly; the std module build
        // (`mcpp.toolchain.stdmod`) and the build.mcpp host helper cannot —
        // they are in the toolchain layer and take a `Toolchain`. Giving them a
        // second way to derive the answer is exactly the shape this release
        // exists to remove, so the answer travels on the value they already
        // share.
        //
        // HERE AND NOT LATER: `ensure_built` runs at :7368 and every compile
        // line is assembled after it. A std BMI built against a different C
        // library than its importers is what e2e 181 catches.
        if (state.tc) state.tc->cAbiPrebuilt = state.resolvedTargetSide.cAbi.prebuilt();
    return {};
}

static std::expected<void, std::string>
step9_target_side_include_broadcast(PrepareState& state, TargetSideGather& gather) {
    namespace tsd = mcpp::targetside;
        // ── The target side's include set is a property of the BUILD ─────────
        //
        // IT WAS ALREADY COMPUTED, AND IT REACHED EXACTLY ONE TRANSLATION
        // UNIT.
        //
        // A package that supplies a target-side layer publishes the headers the
        // whole target is built against — libc++'s, the C library's, the
        // architecture's. Those travel today as an ordinary `publicUsage`,
        // which propagates ALONG DEPENDENCY EDGES. So a workspace member that
        // depends on the provider receives them and a SIBLING DEPENDENCY
        // PACKAGE does not: `nlohmann.json` is not downstream of
        // `openkal-llvm-runtime`, it is beside it.
        //
        // The result is two flavours of BMI in one build — `std` compiled over
        // the target's libc++ (correct: the block at :7232 hands it exactly
        // this set) and the dependency packages compiled over the payload's.
        // Any unit importing both fails at the first template instantiation
        // that touches a declaration present in both header sets:
        //
        //     istream:1245: error: reference to 'space' is ambiguous
        //     note: candidate … xim-x-llvm/…/__locale:321
        //     note: candidate … openkal-llvm-runtime/…/__locale:302
        //
        // mcpp#514. Reproduced in twenty lines with no openkal at all: a path
        // package declaring `provides = ["mcpp:c++-abi=libc++"]` and one
        // `include_dirs` entry reaches the root and its own units, and reaches
        // no sibling dependency package.
        //
        // THE FIX IS THE ONE `mcpp.targetside` OPENS WITH: resolve once,
        // after the graph is known, and have every consumer read that one
        // value. A `publicUsage` describes what a library asks of ITS USERS; a
        // target side is beneath everything. Modelling the second as the first
        // is what made it edge-scoped.
        //
        // ONLY LAYERS THE GRAPH SUPPLIES. `Layer::fromGraph()` is the whole
        // condition. A payload-supplied layer already reaches every unit
        // through `mcpp.toolchain.hostflags`, and emitting it twice would put
        // the ordering of one decision in two places.
        {
            std::set<std::size_t> layerProviderIndices;
            auto note_layer = [&](tsd::CapLayer which, const tsd::Layer& resolved) {
                if (!resolved.fromGraph()) return;
                auto it = gather.byLayer.find(static_cast<int>(which));
                if (it != gather.byLayer.end() && !it->second.empty()) {
                    layerProviderIndices.insert(it->second.front().index);
                    if (which == tsd::CapLayer::CxxAbi)
                        state.cxxLayerProviderIndex = it->second.front().index;
                }
            };
            note_layer(tsd::CapLayer::CompilerRuntime, state.resolvedTargetSide.compilerRuntime);
            note_layer(tsd::CapLayer::KernelAbi,       state.resolvedTargetSide.kernelAbi);
            note_layer(tsd::CapLayer::CAbi,            state.resolvedTargetSide.cAbi);
            note_layer(tsd::CapLayer::CxxAbi,          state.resolvedTargetSide.cxx);

            for (auto idx : layerProviderIndices) {
                if (idx >= state.packages.size()) continue;
                auto const& provider = state.packages[idx];
                state.appendUniquePaths(state.targetSideUsage.includeDirs,
                                  provider.publicUsage.includeDirs);
                state.appendUniquePaths(state.targetSideUsage.includeDirsAfter,
                                  provider.publicUsage.includeDirsAfter);
                state.appendUniqueFlags(state.targetSideUsage.cflags,
                                  provider.publicUsage.cflags);
                state.appendUniqueFlags(state.targetSideUsage.cxxflags,
                                  provider.publicUsage.cxxflags);
            }

            // Into `privateBuild` and NOT into `publicUsage`.
            //
            // It is visible to the whole graph already, so it needs no further
            // propagation; and writing it into `publicUsage` would fold the
            // target side into the usage requirements of any library this
            // build packages — a promise about a different machine.
            //
            // APPENDED, so a package's own directories keep coming first.
            // The target side only has to precede the DRIVER's own defaults,
            // and those are always searched last.
            if (!state.targetSideUsage.includeDirs.empty()
                || !state.targetSideUsage.includeDirsAfter.empty()
                || !state.targetSideUsage.cflags.empty()
                || !state.targetSideUsage.cxxflags.empty()) {
                for (auto& p : state.packages) {
                    state.appendUniquePaths(p.privateBuild.includeDirs,
                                      state.targetSideUsage.includeDirs);
                    state.appendUniquePaths(p.privateBuild.includeDirsAfter,
                                      state.targetSideUsage.includeDirsAfter);
                    state.appendUniqueFlags(p.privateBuild.cflags,
                                      state.targetSideUsage.cflags);
                    state.appendUniqueFlags(p.privateBuild.cxxflags,
                                      state.targetSideUsage.cxxflags);
                }
            }
        }

        // `__OPENKAL__` AND THE REALISED [c-abi] ENVIRONMENT — design
        // 2026-09-18 §2.1, §3.2-§3.4. Broadcast into every package's OWN
        // `privateBuild`, the same channel `targetSideUsage` just used above:
        // it reaches that package's C/C++ compiles AND its dependency scan
        // (`mcpp.modgraph.scanner` reads `privateBuild.cflags`/`cxxflags`),
        // and it is APPENDED, so it follows every flag the package wrote for
        // itself and the driver's own defaults still come last.
        //
        // `c-environment = "platform"` (§3.4) opts a package OUT of the
        // [c-abi] REALISATION ONLY — `__OPENKAL__` still reaches it, because
        // the exception is about the C environment a package's headers see,
        // not about whether its own code may call `kal_*`. The base command
        // line these tokens are appended to is untouched either way, which is
        // what keeps a package that declares neither field byte-identical to
        // a build before this feature existed.
        //
        // ALSO INTO `privateBuild.asmflags` (openkal-musl spike, post-review):
        // the environment is a property of the TARGET, so it has to reach
        // every translation unit built for that target, assembly (.S/.s)
        // included — assembly is preprocessed with the same macros, and real
        // code selects on them (openkal-musl's own `okm_setjmp.S`; upstream
        // libunwind's `assembly.h`). `cflags`/`cxxflags` do not reach a .S
        // file wholesale (`mcpp.build.compile_commands::unit_asm_flags` keeps
        // only their -D/-U/-I words, on purpose — a -std= or -O token meant
        // for the C compiler has no meaning for GAS), so the object-format
        // and wchar-width tokens have to be named again here, into the
        // channel `unit_asm_flags` passes through UNFILTERED. `__OPENKAL__`
        // needs no second copy: it is a -D, and the -D/-U/-I filter already
        // carries it from `cflags` into every assembly unit.
        //
        // Every token in `cEnvTokens`/`cEnvBuiltinsTokens` was checked against
        // clang's GAS (`-x assembler-with-cpp`) front end before this was
        // written (`--target=`, `-f[no-]short-wchar`,
        // `-fno-builtin-memset_pattern16`) and none is rejected — so nothing
        // here is filtered a second time; if a future token IS GAS-hostile,
        // `cenv::realise` is where to split it, not this broadcast.
        // THE MACROS THIS ENGINE DEFINES --- the contract, the rules for
        // reading them and the reason each one exists rather than a manifest
        // key, are `mcpp.toolchain.predefines`. That module is the
        // specification and the implementation of the same thing, so this
        // site decides only WHERE the tokens go, never WHICH they are.
        if (state.tc) {
            std::string targetOs;
            if (auto ttOs = mcpp::toolchain::triple::parse(state.tc->targetTriple))
                targetOs = ttOs->os;
            const auto engineDefines =
                mcpp::toolchain::predefines::define_tokens(
                    targetOs, state.tc->kernelAbiIsOpenkal);
            // Into `cflags`/`cxxflags` only: these are all `-D`, and the
            // channel that builds an assembly unit's flags keeps the -D/-U/-I
            // words of those two (`compile_commands::unit_asm_flags`), so a
            // second copy here would put each one on a `.S` line twice.
            for (auto& p : state.packages) {
                state.appendUniqueFlags(p.privateBuild.cflags,   engineDefines);
                state.appendUniqueFlags(p.privateBuild.cxxflags, engineDefines);
            }
        }

        if (state.tc && (state.tc->kernelAbiIsOpenkal || !state.tc->cEnvTokens.empty()
                   || !state.tc->cEnvBuiltinsTokens.empty())) {
            for (auto& p : state.packages) {
                // `__OPENKAL__` is emitted above, with the rest of the
                // engine's own defines; it is NOT subject to the
                // `c-environment = "platform"` exception below, because that
                // exception is about which C environment a package's headers
                // see, not about whether its code may call `kal_*`.
                if (p.manifest.cEnvironment == "platform") continue;
                state.appendUniqueFlags(p.privateBuild.cflags, state.tc->cEnvTokens);
                state.appendUniqueFlags(p.privateBuild.cxxflags, state.tc->cEnvTokens);
                state.appendUniqueFlags(p.privateBuild.asmflags, state.tc->cEnvTokens);
                state.appendUniqueFlags(p.privateBuild.cflags, state.tc->cEnvBuiltinsTokens);
                state.appendUniqueFlags(p.privateBuild.cxxflags, state.tc->cEnvBuiltinsTokens);
                state.appendUniqueFlags(p.privateBuild.asmflags, state.tc->cEnvBuiltinsTokens);
            }
        }
    return {};
}

static std::expected<void, std::string>
step9_kernel_abi_interface_enumeration(PrepareState& state) {
        // INTERFACE ENUMERATION — THE RESOLUTION-TIME HALF OF THE CAPABILITY
        // MODEL (design 2026-09-20 §5.5; openkal SPEC 0.14 §3.3, §6.2).
        //
        // A package states which interfaces of the `kernel-abi` layer it uses;
        // the package that supplies the layer states which it provides. This
        // engine compares the two sets and knows no member of either: the
        // names belong to the specification that owns the layer, and one may
        // be added to it without a release of this engine.
        //
        // THE QUESTION IS ANSWERED HERE BECAUSE HERE IS WHERE THE ANSWER FIRST
        // EXISTS. §6.2 tabulates three times and states that each is the
        // earliest at which its information exists; "may this program be built
        // against this implementation" is the first of them. Source asking the
        // same question with `#ifdef` asks it during preprocessing, earlier
        // than any answer, which is why each macro-shaped answer to it has had
        // to be replaced by the next one.
        //
        // SILENT WHEN NOTHING DECLARES ANYTHING. A graph in which no package
        // writes `[kernel-abi]` reaches neither loop below, so this addition
        // changes no command line and no diagnostic for every project built
        // before it.
        // THE LIST COMES FROM THE PACKAGE THAT RESOLVED AS THE LAYER, NOT
        // FROM THE FIRST ONE IN THE GRAPH THAT STATED ONE. A graph may
        // carry more than one candidate for a layer — a workspace member
        // beside a dependency, a second implementation reached through a
        // feature that did not activate — and only one of them is the
        // provider this build resolved. Reading whichever came first in
        // `packages` would compare a consumer's requirements against an
        // implementation the build is not using, which is a wrong answer
        // rather than a missing one.
        std::vector<std::string> providedInterfaces;
        std::string providerId;
        for (auto& pkg : state.packages) {
            if (pkg.manifest.kernelAbiProvidesInterfaces.empty()) continue;
            // `impl` is `name@version`; the name is what precedes the
            // separator. A substring test would match `openkal` against
            // `openkal-linux@0.15.0` and read one implementation's list
            // as another's.
            if (!state.resolvedTargetSide.kernelAbi.impl.empty()) {
                auto const& impl = state.resolvedTargetSide.kernelAbi.impl;
                const auto at = impl.find('@');
                const auto implName = at == std::string::npos
                    ? impl : impl.substr(0, at);
                if (implName != pkg.manifest.package.name) continue;
            }
            providedInterfaces = pkg.manifest.kernelAbiProvidesInterfaces;
            providerId = pkg.manifest.package.name;
            break;
        }
        // A REQUIREMENT NOBODY ANSWERED IS SAID SO, because otherwise
        // "yes" and "never asked" are the same reading.
        //
        // Three situations exist and two of them build: the provider
        // states a list and it contains the requirement (build); it
        // states a list and does not (refuse, below); it states nothing
        // at all (build, and until this note, in silence). The third is
        // deliberate --- `provides-interfaces` is younger than the
        // implementations that exist, and a graph that has not adopted it
        // must keep building --- but a consumer reading a green build
        // cannot tell it from the first. One line closes that, and it
        // costs nothing to a graph where the provider does declare.
        std::size_t uncheckedRequirements = 0;
        for (auto& pkg : state.packages) {
            const auto& need = pkg.manifest.kernelAbiRequiresInterfaces;
            if (need.empty()) continue;
            if (providerId.empty()) {
                uncheckedRequirements += need.size();
                continue;
            }
            auto missing = mcpp::targetside::interfaces_not_provided(
                need, providedInterfaces);
            if (missing.empty()) continue;
            refusal::record(refusal::Code::InterfaceNotProvided);
            std::string names;
            for (auto const& mI : missing) {
                names += "\n         ";
                names += mI;
            }
            // THE CODE IS PRINTED, THE WAY E0006 IS, BECAUSE SOMETHING
            // READS THIS. A refusal that only a person can recognise
            // forces every machine consumer to match prose --- and prose
            // that a package's own compile error could coincidentally
            // contain. The mcpp-index compatibility measurement
            // distinguishes "this graph does not supply what the member
            // asked for" from "the member did not build" on exactly this
            // token, and that distinction decides whether a member counts
            // against a compatibility figure.
            // THE LABEL SAYS WHICH IMPLEMENTATION WAS RESOLVED, NOT
            // "provided by". The missing names are listed immediately
            // above it, and `provided by fakekernel` under `openkal.space`
            // reads as the statement that fakekernel provides it --- the
            // exact opposite of what this refusal is about. Read once,
            // rendered, which is the only way that kind of defect is
            // visible: every assertion on this message matches an
            // identifier inside it, and an identifier is in the right
            // place under either wording.
            return std::unexpected(std::format(
                "'{}' requires interfaces the resolved implementation does "
                "not provide. [interface-not-provided]{}\n"
                "       the resolved implementation is {} ({} interface{}), "
                "and none of those listed above is among them.\n"
                "       This is refused before anything is compiled "
                "because dependency resolution is the earliest time the "
                "question can be answered. Select an implementation that "
                "provides them, or remove them from [kernel-abi] "
                "requires-interfaces in '{}'.",
                pkg.manifest.package.name, names, providerId,
                providedInterfaces.size(),
                providedInterfaces.size() == 1 ? "" : "s",
                pkg.manifest.package.name));
        }

        if (uncheckedRequirements > 0) {
            // THE IMPLEMENTATION IS NAMED FROM THE RESOLVED LAYER, not
            // from whichever package happened to be first: the note has
            // to say WHOSE silence this is, or a reader cannot act on it.
            const auto& impl = state.resolvedTargetSide.kernelAbi.impl;
            mcpp::ui::info("note", std::format(
                "kernel-abi interfaces: {} states none, {} requirement{} "
                "unchecked",
                impl.empty() ? std::string("the resolved implementation")
                             : impl,
                uncheckedRequirements,
                uncheckedRequirements == 1 ? "" : "s"));
        }
    return {};
}

static std::expected<void, std::string>
step9_layering_and_requirement_checks(PrepareState& state, TargetSideGather& gather) {
    namespace tsd = mcpp::targetside;
        if (auto why = tsd::check_layering(state.resolvedTargetSide)) {
            refusal::record(refusal::Code::LayerOrdering);
            return std::unexpected(*why);
        }
        // REQUIREMENTS ARE CHECKED BEFORE ANYTHING IS COMPILED, WHICH IS THE
        // WHOLE POINT OF DECLARING THEM. The combination this rejects — a C++
        // runtime configured for one compiler family being handed to another —
        // otherwise fails inside that runtime's own headers, in a message that
        // names a file the reader has never opened and no decision mcpp made.
        // The origin travels with the check: reaching a compiler-layer refusal
        // now means the project stated its own compiler, and the remedy has to
        // name that statement rather than a global default it is not using.
        if (auto why = tsd::check_requirements(
                state.resolvedTargetSide, gather.requirements,
                tc_origin_is_user_explicit(state.tcOrigin) ? tc_origin_name(state.tcOrigin)
                                                     : std::string_view{})) {
            refusal::record(refusal::Code::LayerRequirement);
            return std::unexpected(*why);
        }
        // A WARNING, NOT A REFUSAL. The graph decides the C library either
        // way, so the segment is ignored rather than violated and the artifact
        // is the same with or without it. Refusing was tried and broke every
        // project spelling the host target `x86_64-linux-gnu` — which is what
        // `mcpp toolchain list` prints, and therefore what people write.
        if (auto why = tsd::check_request(state.resolvedTargetSide))
            mcpp::diag::warning("target", *why);

        // The refusal held since toolchain resolution, released now that the
        // other half of its question has an answer. A payload on this machine
        // does not produce this target; if the graph does not supply the
        // target's system either, then nothing does and the diagnosis stands.
        if (!state.unservedTargetDiagnosis.empty()
            && !state.resolvedTargetSide.system_from_graph()) {
            refusal::record(refusal::Code::HostCannotServe);
            return std::unexpected(state.unservedTargetDiagnosis);
        }
    return {};
}

static void
step9_pin_and_linkage_diagnostics(PrepareState& state) {
        // THE TARGET AND THE COMPILER ARE NOT BOUND TOGETHER, AND THE
        // TARGET ROW'S CONVENTION IS A FALLBACK RATHER THAN A RULE.
        //
        // A row pins a toolchain because the payload that toolchain belongs to
        // is what supplies THAT TARGET'S C library. A project whose target side
        // comes from its dependency graph does not use that payload, so the
        // substitution was unnecessary — and this is the first line at which
        // that is knowable, because it is the first line at which the graph
        // exists.
        //
        // The decision itself is NOT revised here. `tc` has been read and
        // mutated at 39 sites between its resolution and this point — the
        // effective triple, the cross flag, the target sysroot, the MSVC
        // runtime contract — and re-resolving it here would redo all of them
        // out of order. Deferring the CHOICE the way the target side itself was
        // deferred is the structural fix and is its own change; until then the
        // user is told what happened and how to state the preference once.
        //
        // AND NOT FOR A ROW WHOSE PIN IS A CAPABILITY, WHERE BOTH HALVES OF
        // THIS SENTENCE ARE FALSE.
        //
        // The warning says the default "would have served" the target and then
        // tells the reader to declare it. On a capability row neither holds:
        // nothing but the pinned payload can emit the target at all, and the
        // declaration it suggests is REFUSED by the capability gate a few
        // hundred lines above -- so following the advice replaces a warning
        // with an error.
        //
        // Measured on `openkal-linux` built for `x86_64-linux-android`, whose
        // target side does come from the graph:
        //
        //   warning: ... so gcc@16.1.0 would have served x86_64-linux-android.
        //            State the preference: [target.x86_64-linux-android]
        //                                  toolchain = "gcc@16.1.0"
        //   $ (declaring exactly that)
        //   error: target 'x86_64-linux-android' cannot be emitted by
        //          'gcc@16.1.0'.
        //
        // The first claim is false on its own terms too: this gcc payload
        // cannot emit an Android object whatever the graph supplies. `graph`
        // answers "who supplies the SYSTEM", and a capability pin answers "who
        // can emit the FORMAT AND THE SYSTEM" -- two questions, and only the
        // second one decides whether a substitution was avoidable.
        const bool pinIsCapability = [&] {
            auto tt = mcpp::toolchain::triple::parse(state.resolvedTargetCanonical);
            return tt && tt->pin_is_capability();
        }();
        if (!state.pinReplacedDefault.empty()
            && state.resolvedTargetSide.system_from_graph()
            && !pinIsCapability) {
            mcpp::diag::warning("toolchain", std::format(
                "this project's target side comes from its dependency graph, so "
                "{} would have served {}.\n"
                "       mcpp used the target row's convention because the graph "
                "is not known when the\n"
                "       toolchain is chosen. State the preference for this "
                "target to skip the substitution:\n"
                "           [target.{}]\n"
                "           toolchain = \"{}\"",
                state.pinReplacedDefault, state.resolvedTargetCanonical,
                state.resolvedTargetCanonical, state.pinReplacedDefault));
        }

        // A request that cannot be honoured is said so rather than dropped.
        //
        // Measured 2026-08-23: `linkage = "dynamic"` on a project whose system
        // comes from the graph produced a statically linked artifact and
        // printed nothing. The outcome is correct — the graph supplies its
        // libraries as objects compiled into this build, and there is no shared
        // object for a loader to resolve at run time — but a directive that has
        // no effect and no diagnostic is indistinguishable from one that was
        // never read.
        //
        // THE C LIBRARY IS THE LAYER THIS DEPENDS ON, NOT "THE SYSTEM".
        // `system_from_graph()` spans two layers, and the arrangement that
        // separates them is real: a backend running ON a platform takes its
        // kernel interface from the graph while the C library stays the
        // payload's. Measured 2026-08-25 on exactly that project — the
        // predicate was true, this warning printed "The artifact is static",
        // and the artifact had three DT_NEEDED entries including `libc.so.6`.
        // The reason the message gives is a property of the C library alone:
        // a payload libc has a shared object, so `dynamic` is honoured and
        // there is nothing to warn about. Same shape as the three defects this
        // release fixes — see `TargetSide::system_from_graph`'s own note.
        if (state.resolvedTargetSide.cAbi.fromGraph()
            && state.m->buildConfig.linkage == "dynamic")
            mcpp::ui::warning(
                "`linkage = \"dynamic\"` has no effect when the "
                "target's system comes from the dependency graph: those "
                "packages are compiled into this build as objects, and there "
                "is no shared object to link against. The artifact is static.");
}

static std::expected<void, std::string>
step9_same_os_check_and_report(PrepareState& state) {
    namespace tsd = mcpp::targetside;
        // Reported, and reported HERE rather than recorded in a manifest field.
        //
        // A line a project writes states an intention, and it goes stale the
        // moment the packages beneath it change — a program that names its C
        // library by name is naming a transitive dependency it did not choose.
        // This states the outcome, so it cannot be stale, and it answers a
        // question that until now had no answer at all: reading every manifest
        // in the graph did not tell anyone what would end up on the link line,
        // because three places derived it separately and could disagree.
        //
        // AND IT PRINTS ONLY WHAT IS NOT ORDINARY. A zero-configuration build
        // resolves all five layers from one compiler payload, and five lines
        // reading `(payload)` answer a question nobody asked. `MCPP_VERBOSE`
        // prints them all; a diagnostic always does.
        // THE REQUESTED TARGET AND THE RESOLVED ONE MUST NAME THE SAME
        // OPERATING SYSTEM, AND UNTIL THIS LINE NOTHING CHECKED.
        //
        // Measured 2026-08-25 in CI, on a machine that had installed only a
        // native gcc — the report itself said it, and the build carried on:
        //
        //     Target x86_64-windows-gnu → x86_64-unknown-linux-gnu
        //     …
        //     src/stream.cpp:68:9: error: 'GetFileType' was not declared
        //
        // Two operating systems on one line. The cross payload was absent, so
        // resolution fell back to the host compiler, and Windows sources were
        // compiled for Linux; the failure surfaced a hundred lines later as an
        // undeclared identifier, naming a symbol rather than the decision.
        // openkal-uefi hit the same fallback at the linker
        // (`ld: unrecognized option '--subsystem'`).
        //
        // THE REPORT ALREADY HELD THE EVIDENCE — this asserts on it rather
        // than deriving the question again. A refusal here costs one line; the
        // alternative is a message about a Win32 function, in a file the reader
        // did not write, for a decision made in this one.
        //
        // Scope is deliberately the OS and not the whole triple: an ABI or
        // vendor difference between `x86_64-windows-gnu` and
        // `x86_64-w64-windows-gnu` is the normalisation this very line reports,
        // and refusing on it would reject every correct cross build.
        // THE NAME THE REPORT PRINTS, DERIVED ONCE AND USED BY BOTH.
        //
        // The first version of this guard read `resolvedTargetCanonical`
        // directly while the report below chose among three sources. They
        // agreed on the machine it was written on and disagreed in CI, where
        // the canonical string was empty and the report still named the target
        // from `targetDisplayName` — so the report showed the mismatch and the
        // guard, asking a different variable, saw nothing to refuse. One fact,
        // derived twice: the shape this whole release exists to remove.
        const std::string reportedTargetName =
            !state.targetDisplayName.empty()
                ? state.targetDisplayName
                : (state.resolvedTargetCanonical.empty()
                       ? (state.tc ? state.tc->targetTriple : std::string{})
                       : state.resolvedTargetCanonical);
        if (!state.resolvedTargetSide.llvmTriple.empty()
            && !reportedTargetName.empty()) {
            auto want = mcpp::toolchain::triple::parse(reportedTargetName);
            auto got  = mcpp::toolchain::triple::parse(
                            state.resolvedTargetSide.llvmTriple);
            // The inputs, when asked for. A guard that declines to fire and a
            // guard that was never reached read the same from outside.
            if (mcpp::log::is_verbose())
                mcpp::ui::info("Target", std::format(
                    "same-OS check: '{}'(os={}) vs '{}'(os={})",
                    reportedTargetName, want ? want->os : "<unparsed>",
                    state.resolvedTargetSide.llvmTriple, got ? got->os : "<unparsed>"));
            if (want && got && !want->os.empty() && !got->os.empty()
                && want->os != got->os) {
                refusal::record(refusal::Code::OsMismatch);
                return std::unexpected(std::format(
                    "target '{}' resolved to a toolchain for '{}'.\n"
                    "       Those are different operating systems, so nothing "
                    "built here would be for\n"
                    "       the target that was asked for. No payload on this "
                    "host produces '{}',\n"
                    "       and mcpp will not substitute the host's.\n"
                    "       install one with `mcpp toolchain install <family> "
                    "<version>`, or name it\n"
                    "       explicitly with `[target.{}] toolchain = \"…\"`.",
                    reportedTargetName, state.resolvedTargetSide.llvmTriple,
                    reportedTargetName, reportedTargetName));
            }
        }
        mcpp::ui::info("Target", tsd::format_report(
            state.resolvedTargetSide, reportedTargetName, mcpp::log::is_verbose()));
    return {};
}

static std::expected<void, std::string>
step9_platform_sdk_closure_visibility(PrepareState& state) {
        // CLOSURE VISIBILITY — design §6. Distinct from the five-layer
        // report above: a platform dependency is not a LAYER (no engine
        // vocabulary names it, and `mcpp.targetside` — the pure, layer-only
        // module the report above comes from — stays that way), it is an
        // ORDINARY package that happens to declare `provides =
        // ["platform-sdk"]`. That is the precise, machine-checkable
        // definition this build uses: a package brings a platform
        // dependency if and only if it says so, the same way a package
        // states any other capability (docs/22, "provides"). Nothing infers
        // this from header paths or link flags, because inference here would
        // have exactly the silent-typo failure mode the reserved `mcpp:`
        // prefix exists to avoid for the five layers — except this
        // capability is deliberately UNPREFIXED, because it names no layer
        // this engine resolves, only a fact a package states about itself.
        std::vector<std::string> platformDeps;
        for (auto& pkg : state.packages) {
            if (std::ranges::find(pkg.manifest.provides, "platform-sdk")
                == pkg.manifest.provides.end())
                continue;
            platformDeps.push_back(pkg.manifest.package.version.empty()
                ? pkg.manifest.package.name
                : std::format("{}@{}", pkg.manifest.package.name,
                              pkg.manifest.package.version));
        }
        if (!platformDeps.empty() || mcpp::log::is_verbose()) {
            std::string joined;
            for (auto& d : platformDeps) {
                if (!joined.empty()) joined += ", ";
                joined += d;
            }
            mcpp::ui::info("Target", std::format(
                "             {:<17} {}", "platform-deps",
                joined.empty() ? std::string("—") : joined));
        }
        if (!platformDeps.empty()
            && state.m->buildConfig.platformDependencies == "refuse") {
            refusal::record(refusal::Code::PlatformDependency);
            std::string joined;
            for (auto& d : platformDeps) {
                if (!joined.empty()) joined += ", ";
                joined += d;
            }
            return std::unexpected(std::format(
                "[build] platform-dependencies = \"refuse\", and the "
                "dependency graph brings {}: {}.\n"
                "       This build asked to be a closure entirely on its "
                "kernel-abi implementation and nothing else (design "
                "2026-09-18 §6).\n"
                "       Remove the dependency, remove the feature that "
                "pulled it in, or drop the refusal to allow it.",
                platformDeps.size() == 1 ? "a platform dependency"
                                         : "platform dependencies",
                joined));
        }
    return {};
}

static std::expected<void, std::string>
step9_kernel_abi_interfaces_and_requirements(PrepareState& state, TargetSideGather& gather) {
    if (auto r = step9_kernel_abi_interface_enumeration(state); !r)
        return std::unexpected(r.error());

    if (auto r = step9_layering_and_requirement_checks(state, gather); !r)
        return std::unexpected(r.error());

    step9_pin_and_linkage_diagnostics(state);

    if (auto r = step9_same_os_check_and_report(state); !r)
        return std::unexpected(r.error());

    return step9_platform_sdk_closure_visibility(state);
}

static std::expected<void, std::string> step9_layer_conditional_config(PrepareState& state) {
    // ── L1b: conditional sections whose predicate names a target-side layer ──
    //
    // The second half of the conditional axis, and it runs HERE for the same
    // reason the root build.mcpp below does: the target side is now resolved,
    // and from this point on everything that consumes build inputs — the P1689
    // scan, the `stdModuleFlags` collection, the fingerprint, `compute_flags` —
    // reads `packages[]` and `*m`, both of which are still writable.
    //
    // EVERY PACKAGE, NOT JUST THE ROOT. The build.mcpp mirror below patches
    // `packages[0]`, which is right for build.mcpp because a build program
    // speaks for its own package and the dep loop already handled the others.
    // Here the motivating case IS a dependency — a package supplying one C++
    // runtime over several C libraries — so patching only the root would leave
    // the one package this feature exists for unserved.
    //
    // `*m` as well as the snapshots: the plan reads the root's flags from
    // both, so a contribution reaching only one of them would reach some of
    // the root's commands and not others.
    // MUTATING `pkg.manifest` IS NOT ENOUGH, AND THAT IS THE WHOLE
    // DIFFICULTY OF A LATE PRODUCER. `makePackageRoot` snapshots the manifest's
    // build inputs into `privateBuild` / `linkUsage`, and the compile and link
    // edges read THOSE. The build.mcpp tail below solves the identical problem
    // with `directives::mark` + `fold_private_tail`, so this uses the same two
    // helpers rather than a second mechanism — measured first: writing only
    // `pkg.manifest.buildConfig` produced a build in which every layer
    // predicate matched and no flag reached the compiler.
    if (state.targetSideResolved) {
        auto layerCtx = state.cfgCtx();
        layerCtx.layersKnown     = true;
        layerCtx.compiler        = state.resolvedTargetSide.compiler.interfaceName;
        layerCtx.compilerRuntime = state.resolvedTargetSide.compilerRuntime.interfaceName;
        layerCtx.kernelAbi       = state.resolvedTargetSide.kernelAbi.interfaceName;
        layerCtx.cAbi            = state.resolvedTargetSide.cAbi.interfaceName;
        layerCtx.cxxAbi          = state.resolvedTargetSide.cxx.interfaceName;
        // The root manifest and its snapshots are both read by the plan, so a
        // contribution reaching the snapshots but not `*m` would reach some of
        // the root's commands and not others.
        merge_layer_conditional_config(*state.m, layerCtx);
        for (auto& pkg : state.packages) {
            const auto mark  = state.markDirectiveTail(pkg.manifest);
            const auto ldN   = pkg.manifest.buildConfig.ldflags.size();
            const auto privN = pkg.manifest.buildConfig.privateIncludeDirs.size();
            if (!merge_layer_conditional_config(pkg.manifest, layerCtx)) continue;
            // cflags / cxxflags / include_dirs / include_dirs_after.
            state.foldDirectiveTailIntoPrivateBuild(pkg, pkg.manifest, mark);
            // ldflags: the link reads linkUsage.
            pkg.linkUsage.ldflags.insert(
                pkg.linkUsage.ldflags.end(),
                pkg.manifest.buildConfig.ldflags.begin()
                    + static_cast<std::ptrdiff_t>(ldN),
                pkg.manifest.buildConfig.ldflags.end());
            // private_include_dirs: expanded at makePackageRoot and folded into
            // privateBuild.includeDirs, which is what keeps them OUT of
            // publicUsage. A conditional entry has to take the same route or a
            // vendored header overlay would reach every consumer — the blast
            // radius e2e 304 exists to hold.
            for (auto it = pkg.manifest.buildConfig.privateIncludeDirs.begin()
                            + static_cast<std::ptrdiff_t>(privN);
                 it != pkg.manifest.buildConfig.privateIncludeDirs.end(); ++it) {
                if (it->is_absolute()) {
                    auto n = *it; n.make_preferred();
                    if (std::ranges::find(pkg.privateBuild.includeDirs, n)
                        == pkg.privateBuild.includeDirs.end())
                        pkg.privateBuild.includeDirs.push_back(std::move(n));
                    continue;
                }
                for (auto& dir : mcpp::modgraph::expand_dir_glob(
                         pkg.root, it->generic_string()))
                    if (std::ranges::find(pkg.privateBuild.includeDirs, dir)
                        == pkg.privateBuild.includeDirs.end())
                        pkg.privateBuild.includeDirs.push_back(dir);
            }
        }
    }

    return {};
}

static std::expected<void, std::string> step9_dependency_link_forms(PrepareState& state) {
    // ── #519: which FORM does each dependency take in this build ────────────
    //
    // The decision itself lives in `mcpp.build.linkage_form`, which is a pure,
    // table-driven function with no filesystem and no manifest knowledge. What
    // happens here is only the two halves that need this scope: collecting the
    // facts, and MATERIALISING the answer.
    //
    // COMPUTED HERE, APPLIED AFTER THE SCAN (#642 E2). A build program that
    // generates a loader entry, or `dllimport` definitions, needs the form a
    // dependency takes, and the root's program runs next, before the scan.
    // Every input is final at this point: the requests are the root manifest's,
    // which no directive changes; the target facts are resolved; each
    // dependency's own build program has run, so its `ldflags` are complete;
    // and the layer-conditional sections above have been folded. What the scan
    // used to contribute, whether a package has sources of its own, is read
    // from the scanner's own selection (`package_source_files`), so the two
    // cannot disagree. The answers are stored in `dependencyLinkForms`; the
    // application after the scan and the root program's environment both read
    // them, and nothing resolves a second time.
    {
        namespace lf = mcpp::build::linkage_form;

        lf::Request request;
        if (auto parsed = lf::parse(state.m->buildConfig.dependencyLinkage))
            request.whole = *parsed;
        request.wholeIsExplicit = !state.m->buildConfig.dependencyLinkage.empty();
        // ONLY THE ROOT MANIFEST'S EDGES. See DependencySpec::linkage — a
        // package deep in the graph imposing a whole-image layout on its
        // consumer is a supply-chain property, not a convenience. In a
        // workspace plan the root's edges are the selected members' own
        // (`PrepareState::declaredByRoot`): the virtual root declares nothing
        // but the members, so reading only its edges ignored every request.
        // One configuration builds a package in one form, so two members
        // that ask for two forms of one package are refused, naming both.
        //
        // Compared by the identity each declaration resolved to (its
        // namespace and name, written back after resolution), not by the key
        // spelled: `mcpplibs.foo` and `foo` are one package, and `a.foo` and
        // `b.foo` are two.
        struct Asked { lf::DepLinkage form; std::string by; };
        std::map<std::string, Asked, std::less<>> askedFor;
        auto requestEdges = [&](const mcpp::manifest::Manifest& mf, std::string_view who)
            -> std::expected<void, std::string> {
            for (auto const& [depName, spec] : mf.dependencies) {
                if (spec.linkage.empty()) continue;
                auto parsed = lf::parse(spec.linkage);
                if (!parsed) continue;
                auto shortKey = spec.shortName.empty() ? depName : spec.shortName;
                const auto identity = spec.namespace_.empty()
                    ? depName : spec.namespace_ + "." + shortKey;
                if (auto it = askedFor.find(identity);
                    it != askedFor.end() && it->second.form != *parsed)
                    return std::unexpected(std::format(
                        "dependency '{}' is to be linked as '{}' by '{}' and as "
                        "'{}' by '{}'.\n"
                        "       A configuration builds a package in one form: "
                        "state the same `linkage` in both.",
                        identity, lf::to_string(it->second.form), it->second.by,
                        lf::to_string(*parsed), who));
                askedFor.emplace(identity, Asked{*parsed, std::string(who)});
                request.perPackage[depName] = *parsed;
                request.perPackage.emplace(shortKey, *parsed);
            }
            return {};
        };
        if (auto r = requestEdges(*state.m, state.m->package.name); !r)
            return std::unexpected(r.error());
        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            if (!state.packages[i].selectedMember) continue;
            if (auto r = requestEdges(state.packages[i].manifest,
                                      state.packages[i].manifest.package.name); !r)
                return std::unexpected(r.error());
        }

        lf::TargetFacts targetFacts;
        if (auto t = mcpp::toolchain::triple::parse(state.tc->targetTriple))
            targetFacts.hasLoader = !t->is_freestanding();
        // The libc axis. Spelled exactly as `compute_flags` spells it, because
        // the two must agree about what `-static` means: an image linked that
        // way has no interpreter, so no shared object can ever be loaded into
        // it. Two keys with `linkage` in the name, and they are NOT independent.
        targetFacts.fullStaticLibc =
            state.m->buildConfig.linkage == "static"
            && mcpp::toolchain::target_supports_full_static(
                   state.tc->targetTriple, mcpp::platform::supports_full_static);

        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            auto const& pkg = state.packages[i].manifest;
            const std::string fq = pkg.package.namespace_.empty()
                ? pkg.package.name
                : std::format("{}.{}", pkg.package.namespace_, pkg.package.name);

            lf::PackageFacts facts;
            facts.label = std::format("{}@{}", fq, pkg.package.version);
            facts.hasSources = !mcpp::modgraph::package_source_files(
                state.packages[i].root, pkg).empty();
            facts.carriesForeignLinkInputs =
                lf::carries_foreign_link_inputs(
                    mcpp::manifest::flag_words(pkg.buildConfig.ldflags));
            facts.isDistribution = mcpp::pack::is_distribution_package(pkg);
            for (auto const& artifact : pkg.runtimeConfig.artifacts) {
                if (artifact.role == "static-library") facts.shipsStatic = true;
                if (artifact.role == "shared-library") facts.shipsShared = true;
            }
            bool hasLibraryTarget = false;
            for (auto const& t : pkg.targets) {
                if (t.kind == mcpp::manifest::Target::SharedLibrary
                    && !facts.declaredShared) {
                    facts.declaredShared = true;
                    facts.declaredSharedBy = t.kindDeclaredBy;
                    facts.declaredSharedByRow = t.kindFromRow;
                }
                if (t.kind == mcpp::manifest::Target::Library) {
                    hasLibraryTarget = true;
                    // One package, one form: the first library target that
                    // states a default speaks for the package, as the first
                    // `kind = "shared"` does for the constraint.
                    if (!facts.defaultLinkage && !t.linkageDefault.empty()) {
                        facts.defaultLinkage = lf::parse(t.linkageDefault);
                        facts.defaultDeclaredBy = t.linkageDeclaredBy;
                    }
                }
            }

            // A consumer addresses a dependency by whatever it wrote in
            // `[dependencies]` — the fully-qualified name or the bare one —
            // while every message wants the version too. Rather than swapping
            // the label to whichever spelling matches (which drops the version
            // from every refusal), make the request answer to the descriptive
            // label as well.
            for (auto const& key : { fq, pkg.package.name }) {
                if (auto it = request.perPackage.find(key);
                    it != request.perPackage.end()) {
                    request.perPackage.emplace(facts.label, it->second);
                    break;
                }
            }
            auto allowed = lf::admissible(facts, targetFacts);
            DependencyLinkForm form;
            form.answer   = lf::resolve(facts, allowed, request);
            // Recorded for a package that has a library to link; a package of
            // programs or rules has no form to report.
            form.recorded = facts.isDistribution || facts.declaredShared
                         || hasLibraryTarget;
            form.facts    = std::move(facts);
            state.dependencyLinkForms.emplace(i, std::move(form));
        }
    }

    return {};
}

static void step9_define_graph_package_entry_closure(PrepareState& state) {
    // ── The resolved graph, one derivation for two readers (#634 X, #647 E1) ──
    //
    // `resolution.json`'s `graph` section and the document the root build
    // program reads (`mcpp::graph_file()`) describe the same packages, and they
    // are built by this one function so they cannot disagree. Each entry holds
    // the package's identity, every request that reached it (the key as
    // written and the table that declared it) and, for a library, its link
    // form with the reason. The build program's entries add what a program
    // needs to act on a package: its manifest directory, the features it is
    // built with, its targets, and its `[package.metadata]` verbatim.
    //
    // The link form is read from `dependencyLinkForms`, which is computed once,
    // before this point, for exactly this program (#642 E2).
    state.graph_package_entry = [&](std::size_t i, bool forBuildProgram,
                                    const std::vector<bool>* requesters) {
        auto const& pm = state.packages[i].manifest;
        const auto id = mcpp::manifest::package_id(pm.package);
        nlohmann::json entry = {
            {"package", {
                {"canonical", id.canonical()},
                {"namespace", id.namespace_},
                {"name", id.name},
                {"version", id.version},
                {"source", id.sourceProvenance},
            }},
            {"root", i == 0},
        };
        nlohmann::json requests = nlohmann::json::array();
        for (auto const& r : state.graphRequests) {
            if (r.dependencyPackageIndex != i) continue;
            if (requesters && (r.consumerPackageIndex >= requesters->size()
                               || !(*requesters)[r.consumerPackageIndex])) continue;
            requests.push_back({
                {"requester", mcpp::manifest::package_id(
                    state.packages[r.consumerPackageIndex].manifest.package).canonical()},
                {"key", r.key},
                {"table", r.table},
            });
        }
        entry["requested_by"] = std::move(requests);
        if (auto form = state.dependencyLinkForms.find(i);
            form != state.dependencyLinkForms.end() && form->second.recorded)
            entry["link"] = {
                {"form", std::string(mcpp::build::linkage_form::to_string(
                             form->second.answer.linkage))},
                {"reason", form->second.answer.reason},
            };
        // D7: where this package's build program writes in this
        // configuration (`mcpp::out_dir()`), for a reader outside the build
        // -- a test script -- that needs a generated file and must not guess.
        // resolution.json only: the graph document a build program reads is
        // part of its re-run key, and must not change with when it is asked.
        if (!forBuildProgram) {
            static const std::vector<std::string> none;
            // A member's program keeps its cache in the project (step 9).
            const bool member = state.m->package.virtualRoot && i != 0
                             && state.isWorkspaceMemberPackage(i);
            entry["outDir"] = state.programOutDir(member ? *state.root : state.workRoot, pm,
                                                  i < state.activeFeaturesByPackage.size()
                                                      ? state.activeFeaturesByPackage[i] : none)
                                  .generic_string();
            return entry;
        }

        std::error_code ec;
        auto dir = std::filesystem::absolute(state.packages[i].root, ec).lexically_normal();
        entry["manifest_dir"] = dir.string();
        nlohmann::json feats = nlohmann::json::array();
        if (i < state.activeFeaturesByPackage.size())
            for (auto const& f : state.activeFeaturesByPackage[i]) feats.push_back(f);
        entry["features"] = std::move(feats);
        nlohmann::json targets = nlohmann::json::array();
        for (auto const& t : pm.targets) {
            using K = mcpp::manifest::Target::Kind;
            const std::string_view kind =
                t.kind == K::Library       ? "lib"
              : t.kind == K::Binary        ? "bin"
              : t.kind == K::SharedLibrary ? "shared"
              : t.kind == K::TestBinary    ? "test"
              :                              "app";
            targets.push_back({{"name", t.name}, {"kind", std::string(kind)}});
        }
        entry["targets"] = std::move(targets);
        entry["metadata"] = pm.packageMetadataJson.empty()
            ? nlohmann::json::object()
            : nlohmann::json::parse(pm.packageMetadataJson, nullptr,
                                    /*allow_exceptions=*/false);
        if (entry["metadata"].is_discarded()) entry["metadata"] = nlohmann::json::object();
        return entry;
    };

}

static std::expected<void, std::string> step9_root_build_program(PrepareState& state) {
    // ── L3: ROOT build.mcpp (moved after dependency resolution, design §3.1
    // item 4) ────────────────────────────────────────────────────────────────
    // Runs HERE — after dep resolution + feature activation (so the contract
    // env can expose MCPP_DEP_<NAME>_DIR exactly like the dep loop above does)
    // and BEFORE the modgraph scan / flag canonicalization / fingerprint (so
    // its generated=/source= sources and flag directives are fully visible).
    // Ordering invariants preserved relative to the pre-move call site:
    // materialize_generated_files (may produce build.mcpp itself) and the L1
    // cfg merge still run earlier — ONLY this call moved later.
    //
    // One wrinkle the old ordering hid: back then apply() mutated *m BEFORE
    // `packages[0] = makePackageRoot(*root, *m)` snapshotted buildConfig into
    // privateBuild/manifest — the copies the scan and per-TU flag assembly
    // actually read. Now the snapshot (and root feature activation on it)
    // already happened, so mirror the directive TAILS into packages[0]
    // explicitly, the same way the dep loop does for its package.
    // A package with no `build.mcpp` still runs one when a rule dependency
    // described it: `run_build_program` writes that program into the build
    // directory. This guard therefore asks the same question the function does,
    // and a guard that asked only about the file left the synthesis unreachable
    // -- the shaders went uncompiled and the refusal named the missing program
    // rather than the guard. Measured.
    // A workspace plan's virtual root has no program; its members' programs
    // run in `step9_member_build_programs`, below.
    if (state.m->package.virtualRoot) return {};
    if (std::filesystem::exists(*state.root / "build.mcpp")
        || !state.m->buildConfig.ruleModules.empty()) {
        auto host = state.host_tc_for_build_program();
        if (!host) return std::unexpected(host.error());
        mcpp::build::BuildProgramEnv bpEnv;
        bpEnv.targetTriple = state.resolvedTargetCanonical;
        // Everything the engine already knows and a build program would
        // otherwise hardcode: the payload ROOT (not the driver), the target's
        // C library, which compiler and which C++ standard library resolved,
        // and the three answers that keep a board package from naming a
        // toolchain. One call — see fill_target_build_env.
        fill_target_build_env(bpEnv, *state.m, state.tc ? &*state.tc : nullptr, state.cfg_opt ? &*state.cfg_opt : nullptr);
        bpEnv.toolsBin = state.projectSubosBin;
        bpEnv.profile      = state.effectiveProfile;
        bpEnv.accel        = state.resolvedAccel();
        fill_package_build_env(bpEnv, *state.m);
        state.fillPackEnv(bpEnv, 0);
        bpEnv.languageModules = state.m->language.modules;
        bpEnv.ruleModules  = state.m->buildConfig.ruleModules;
        if (auto dit = state.deviceSourcesByPackage.find(state.root->string()); dit != state.deviceSourcesByPackage.end())
            bpEnv.deviceSources = dit->second;
        // Set explicitly rather than relying on build_dir()'s root-relative
        // default: under BuildOverrides::work_dir the package root is shared
        // and may be read-only, and the default would write the compiled
        // helper straight into it. Same value as the default when work_dir is
        // unset, so an ordinary build is unchanged.
        bpEnv.artifactsDir = state.workRoot / "target" / ".build-mcpp";
        // What the program imports is kept once for the workspace, and in the
        // global cache where it comes from the engine or the index (#748).
        bpEnv.moduleStore  = bpEnv.artifactsDir / "host-modules";
        if (state.cacheMode == CacheMode::Global)
            bpEnv.moduleCacheRoot = mcpp::home::cache_root();
        // Root mode keeps genBase empty: a relative `generated=` from the ROOT
        // package resolves against the package root (the documented contract),
        // not against OUT_DIR.
        // Same expression as the pre-move call site (and same order), so the
        // contract hash — and therefore the build.mcpp cache — is unchanged
        // across the move for feature-identical builds.
        bpEnv.features     = feature_closure(*state.m, parse_feature_request(state.overrides.features));
        bpEnv.outDir       = state.programOutDir(state.workRoot, *state.m, bpEnv.features);
        // mcpp#241 (root): consumer index 0, same owner as the dep loop.
        //
        // AND THE LINK FORM OF EACH DEPENDENCY (#642 E2), to this program only.
        // The root decides every dependency's form, and when this program runs
        // every input of that decision is final: the requests are the root
        // manifest's, and each dependency's own program has already run. A
        // DEPENDENCY's program is not offered the forms. It runs in discovery
        // order, before the programs of packages discovered after it, and those
        // programs supply facts the answer depends on (a `-L` they add makes a
        // package static-only), so the value it could be given would be a guess.
        {
            std::map<std::size_t, std::string> rootLinkForms;
            for (auto const& [idx, form] : state.dependencyLinkForms)
                if (form.recorded)
                    rootLinkForms.emplace(idx, std::string(
                        mcpp::build::linkage_form::to_string(form.answer.linkage)));
            state.fillDepDirs(bpEnv, 0, &rootLinkForms);
        }
        state.fillXpkgDirs(bpEnv, *state.m, 0);
        // #355: the host tools the ROOT package requested (consumer index 0).
        if (auto tit = state.toolEnvByConsumer.find(0u); tit != state.toolEnvByConsumer.end())
            bpEnv.toolPaths = tit->second;
        bpEnv.hostModules = state.hostModulesByConsumer.count(0u)
            ? state.hostModulesByConsumer.at(0u)
            : decltype(bpEnv.hostModules){};
        bpEnv.dormantFeatures = state.dormantFeaturesByConsumer.count(0u)
            ? state.dormantFeaturesByConsumer.at(0u)
            : decltype(bpEnv.dormantFeatures){};
        // #647 E1: THE RESOLVED GRAPH, FOR THE ROOT'S PROGRAM ONLY.
        //
        // Every package, dependencies before the packages that request them
        // (ties in discovery order), so a program that merges what libraries
        // contribute can apply them in override order without a sort of its
        // own. The root decides the graph, and every input of that decision is
        // final here -- the same reason `dep_linkage` is offered to this
        // program alone. A file, not variables: a graph with metadata does not
        // fit an environment block (`MAX_ARG_STRLEN`, the Windows limit).
        //
        // ITS DIGEST JOINS THE RE-RUN KEY. Editing a dependency's
        // `[package.metadata]` changes what this program would answer, so it
        // must run again; editing that dependency's sources does not, and the
        // document does not change.
        {
            std::vector<std::size_t> order;
            std::vector<bool> placed(state.packages.size(), false);
            while (order.size() < state.packages.size()) {
                std::size_t pick = state.packages.size();
                for (std::size_t i = 0; i < state.packages.size() && pick == state.packages.size(); ++i) {
                    if (placed[i]) continue;
                    bool ready = true;
                    for (auto const& r : state.graphRequests)
                        if (r.consumerPackageIndex == i && r.dependencyPackageIndex != i
                            && r.dependencyPackageIndex < state.packages.size()
                            && !placed[r.dependencyPackageIndex]) { ready = false; break; }
                    if (ready) pick = i;
                }
                // A cycle leaves nothing ready; its first member in discovery
                // order is taken so the document is still complete.
                if (pick == state.packages.size())
                    for (std::size_t i = 0; i < state.packages.size(); ++i)
                        if (!placed[i]) { pick = i; break; }
                placed[pick] = true;
                order.push_back(pick);
            }
            nlohmann::json doc;
            doc["kind"] = "mcpp.graph";
            doc["version"] = 1;
            nlohmann::json list = nlohmann::json::array();
            for (auto i : order) list.push_back(state.graph_package_entry(i, true, nullptr));
            doc["packages"] = std::move(list);
            const auto text = doc.dump(2) + "\n";
            const auto graphPath = bpEnv.artifactsDir / "graph.json";
            std::error_code gec;
            std::filesystem::create_directories(graphPath.parent_path(), gec);
            const auto tmp = graphPath.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << text;
            }
            std::filesystem::rename(tmp, graphPath, gec);
            if (gec)
                return std::unexpected(std::format(
                    "cannot write the graph document '{}': {}",
                    graphPath.string(), gec.message()));
            bpEnv.graphFile   = graphPath;
            bpEnv.graphDigest = mcpp::toolchain::hash_string(text);
        }
        auto& bcRoot = state.m->buildConfig;
        const auto mark = state.markDirectiveTail(*state.m);
        const auto rldN = bcRoot.ldflags.size(), rsrcN = bcRoot.sources.size(),
                   rmodN = state.m->modules.sources.size();
        const auto ractN = bcRoot.actions.size();
        // #622 A4: how many `[runtime] deploy` entries existed before this
        // program ran — the manifest-sourced ones, already in `packages[0]`'s
        // snapshot. Anything past this index is a `mcpp::deploy()` residue
        // that needs the same mirror the flag/source tails get below.
        const auto rdeployN = state.m->runtimeConfig.linkIntent.deploy.size();
        // Same reason, one field wide: `mcpp::runtime_search_dir()` residue
        // needs the same mirror `deploy` does, or `resolve_runtime_contract`
        // (which reads `packages[0]`'s snapshot, not `*m`) never sees it.
        const auto rsearchDirN = state.m->runtimeConfig.linkIntent.runtimeSearchDirs.size();
        // What the dependencies supplied as runners, before the root's program
        // speaks. The root's emissions are appended to the same slots, so a
        // name both supply becomes one argv joining the two (#634, §9 item 8,
        // measured: `run-A.sh run-B.sh <artifact>`).
        const auto runnerBeforeRoot = bcRoot.runner;
        const auto namedBeforeRoot  = bcRoot.namedRunners;
        // The root's program is the requested package's.
        bpEnv.requested = !state.m->package.virtualRoot;
        auto bp = run_answering_requests(state, *state.m, bpEnv, 0,
            state.m->package.name, [&] {
                return mcpp::build::run_build_program(
                    *state.m, *state.root, host->first, host->second,
                    state.m->cppStandard, bpEnv);
            });
        if (!bp && !state.overrides.plan_only) {
            return std::unexpected(bp.error());
        }
        // #699 item 2 (E3): under `emit build-database` (`plan_only`), a
        // failing root build program describes the package without its
        // directives rather than costing the whole plan. Every mirror below
        // reads what the program would have added to `*m`, so skipping
        // straight past it (nothing runs on this path) is what "without its
        // directives" means; a later failure that follows from the gap
        // fails the member under the ordinary rule (E1).
        if (!bp) {
            state.planNotes.push_back({"MCPP_BUILD_DATABASE_PROGRAM_FAILED",
                bp.error(), mcpp::wire::Severity::Error,
                (*state.root / "build.mcpp").string()});
            // Named so the device-source check below (and anything else whose
            // premise is this program's directives) can tell a package whose
            // program failed apart from one that simply has no program.
            state.programFailedPackages.insert(state.root->string());
        }
        if (bp) {
            // THE SAME RULE THE DEPENDENCIES ARE HELD TO, WITH THE ROOT AS A PARTY.
            // Two suppliers of one runner are refused naming both, and the
            // manifest is the way to choose: a `[target.<triple>]` runner the
            // project writes outranks every supplied one where the runner is
            // looked up, so a name the manifest declares is not refused here.
            {
                const auto parsedTarget = state.tc
                    ? mcpp::toolchain::triple::parse(state.tc->targetTriple)
                    : std::nullopt;
                const auto rowKey = parsedTarget ? parsedTarget->str()
                                  : state.tc     ? state.tc->targetTriple : std::string{};
                const auto* row = [&]() -> const mcpp::manifest::TargetEntry* {
                    if (parsedTarget) return find_target_entry(*state.m, *parsedTarget);
                    auto it = state.m->targetOverrides.find(rowKey);
                    return it == state.m->targetOverrides.end() ? nullptr : &it->second;
                }();
                const auto manifestNames = [&](std::string_view name) {
                    if (!row) return false;
                    if (name.empty()) return !row->runner.empty();
                    return row->namedRunners.contains(std::string(name));
                };
                if (!state.runnerProvider.empty() && !runnerBeforeRoot.empty()
                    && bcRoot.runner.size() > runnerBeforeRoot.size()
                    && !manifestNames({})) {
                    return std::unexpected(std::format(
                        "the dependency '{}' and this project's build program both "
                        "supply the runner for this target, and the two would be "
                        "joined into one argv.\n"
                        "       Drop one of them, or state the runner in "
                        "[target.{}].runner.",
                        state.runnerProvider, rowKey));
                }
                for (auto const& [name, nr] : bcRoot.namedRunners) {
                    auto before = namedBeforeRoot.find(name);
                    auto who = state.namedRunnerProvider.find(name);
                    if (before == namedBeforeRoot.end() || before->second.argv.empty()
                        || who == state.namedRunnerProvider.end() || who->second.empty())
                        continue;
                    if (nr.argv.size() <= before->second.argv.size()) continue;
                    if (manifestNames(name)) continue;
                    return std::unexpected(std::format(
                        "the dependency '{}' and this project's build program both "
                        "supply a runner named '{}' for this target, and the two "
                        "would be joined into one argv.\n"
                        "       Drop one of them, or state it in "
                        "[target.{}.runners].{}.",
                        who->second, name, rowKey, name));
                }
            }
            auto& pkg0 = state.packages[0];
            // Compile-visible tail → privateBuild: the shared fold (same owner
            // as the dep loop; the root's TUs read privateBuild).
            state.foldDirectiveTailIntoPrivateBuild(pkg0, *state.m, mark);
            // Before the source residues are mirrored below: adopting an action's
            // outputs APPENDS to bcRoot.sources, and those appends must be inside
            // the tail that gets copied into the packages[0] snapshot the scan reads.
            state.adoptActionOutputs(*state.m, *state.root, ractN);
            // The root's build program has spoken; a floor it stated is checked
            // now, with the facts every package (it included) established.
            if (auto err = state.checkVersionFloors(); err) return std::unexpected(*err);
            // Root residues — apply() mutated *m, but packages[0].manifest is a
            // value-copy snapshot taken at makePackageRoot, so everything the
            // scan/fingerprint read from the snapshot needs the tail mirrored:
            // sources → the scan walks packages[0].manifest, not *m.
            pkg0.manifest.buildConfig.sources.insert(
                pkg0.manifest.buildConfig.sources.end(),
                bcRoot.sources.begin() + rsrcN, bcRoot.sources.end());
            pkg0.manifest.modules.sources.insert(
                pkg0.manifest.modules.sources.end(),
                state.m->modules.sources.begin() + rmodN, state.m->modules.sources.end());
            // The units its actions generate, as they declared them (D6).
            for (auto const& [path, unit] : state.m->modules.declaredUnits)
                pkg0.manifest.modules.declaredUnits.insert_or_assign(path, unit);
            // The snapshot's manifest is what later readers of the root's
            // flags see -- mirror the flag/include tails, as the old
            // pre-snapshot ordering implicitly did.
            pkg0.manifest.buildConfig.cflags.insert(
                pkg0.manifest.buildConfig.cflags.end(),
                bcRoot.cflags.begin() + static_cast<std::ptrdiff_t>(mark.cflags),
                bcRoot.cflags.end());
            pkg0.manifest.buildConfig.cxxflags.insert(
                pkg0.manifest.buildConfig.cxxflags.end(),
                bcRoot.cxxflags.begin() + static_cast<std::ptrdiff_t>(mark.cxxflags),
                bcRoot.cxxflags.end());
            pkg0.manifest.buildConfig.includeDirs.insert(
                pkg0.manifest.buildConfig.includeDirs.end(),
                bcRoot.includeDirs.begin() + static_cast<std::ptrdiff_t>(mark.includeDirs),
                bcRoot.includeDirs.end());
            pkg0.manifest.buildConfig.includeDirsAfter.insert(
                pkg0.manifest.buildConfig.includeDirsAfter.end(),
                bcRoot.includeDirsAfter.begin()
                    + static_cast<std::ptrdiff_t>(mark.includeDirsAfter),
                bcRoot.includeDirsAfter.end());
            // Link flags → the final link reads *m (already applied); keep the
            // linkUsage snapshot and fingerprint metadata equivalent too.
            pkg0.linkUsage.ldflags.insert(pkg0.linkUsage.ldflags.end(),
                bcRoot.ldflags.begin() + rldN, bcRoot.ldflags.end());
            pkg0.manifest.buildConfig.ldflags.insert(
                pkg0.manifest.buildConfig.ldflags.end(),
                bcRoot.ldflags.begin() + rldN, bcRoot.ldflags.end());
            // #622 A4: `mcpp::deploy()` residue → `packages[0].manifest`, the
            // object `resolve_runtime_contract` (plan.cppm) actually reads.
            // Without this mirror a directive-sourced deploy entry lands in `*m`
            // and nowhere the planner looks — the same gap this block already
            // closes for sources/flags, one more field wide.
            pkg0.manifest.runtimeConfig.linkIntent.deploy.insert(
                pkg0.manifest.runtimeConfig.linkIntent.deploy.end(),
                state.m->runtimeConfig.linkIntent.deploy.begin() + static_cast<std::ptrdiff_t>(rdeployN),
                state.m->runtimeConfig.linkIntent.deploy.end());
            // `mcpp::runtime_search_dir()` residue → `packages[0].manifest`, the
            // same object and the same reason as the `deploy` mirror above: without
            // it a directive-sourced entry lands in `*m` and `resolve_runtime_contract`
            // never looks there.
            pkg0.manifest.runtimeConfig.linkIntent.runtimeSearchDirs.insert(
                pkg0.manifest.runtimeConfig.linkIntent.runtimeSearchDirs.end(),
                state.m->runtimeConfig.linkIntent.runtimeSearchDirs.begin()
                    + static_cast<std::ptrdiff_t>(rsearchDirN),
                state.m->runtimeConfig.linkIntent.runtimeSearchDirs.end());
        }
    }
    return {};
}

// The graph document a build program reads (#647 E1): every package the
// program's own package reaches, itself included, dependencies before the
// packages that request them (ties in discovery order), with `root` marking
// the package whose program reads it. For a root that is the whole graph.
static std::expected<std::pair<std::filesystem::path, std::string>, std::string>
write_graph_document(PrepareState& state, std::size_t subject,
                     const std::filesystem::path& artifactsDir) {
    std::vector<bool> reached(state.packages.size(), subject == 0);
    reached[subject] = true;
    for (bool grew = true; grew;) {
        grew = false;
        for (auto const& r : state.graphRequests)
            if (r.consumerPackageIndex < reached.size() && reached[r.consumerPackageIndex]
                && r.dependencyPackageIndex < reached.size()
                && !reached[r.dependencyPackageIndex]) {
                reached[r.dependencyPackageIndex] = true;
                grew = true;
            }
    }
    std::vector<std::size_t> order;
    std::vector<bool> placed(state.packages.size(), false);
    std::size_t wanted = 0;
    for (bool b : reached) wanted += b ? 1 : 0;
    while (order.size() < wanted) {
        std::size_t pick = state.packages.size();
        for (std::size_t i = 0; i < state.packages.size() && pick == state.packages.size(); ++i) {
            if (placed[i] || !reached[i]) continue;
            bool ready = true;
            for (auto const& r : state.graphRequests)
                if (r.consumerPackageIndex == i && r.dependencyPackageIndex != i
                    && r.dependencyPackageIndex < state.packages.size()
                    && reached[r.dependencyPackageIndex]
                    && !placed[r.dependencyPackageIndex]) { ready = false; break; }
            if (ready) pick = i;
        }
        // A cycle leaves nothing ready; its first member in discovery
        // order is taken so the document is still complete.
        if (pick == state.packages.size())
            for (std::size_t i = 0; i < state.packages.size(); ++i)
                if (!placed[i] && reached[i]) { pick = i; break; }
        placed[pick] = true;
        order.push_back(pick);
    }
    nlohmann::json doc;
    doc["kind"] = "mcpp.graph";
    doc["version"] = 1;
    // The requests a document lists are those made by its own packages. A
    // request from outside the program's closure (another workspace member, a
    // workspace plan's virtual root) is a fact about the plan, not about the
    // package, and would make the document, and with it the program's cache
    // key, depend on which members the command selected.
    nlohmann::json list = nlohmann::json::array();
    for (auto i : order) {
        auto entry = state.graph_package_entry(i, true, &reached);
        entry["root"] = i == subject;
        list.push_back(std::move(entry));
    }
    doc["packages"] = std::move(list);
    const auto text = doc.dump(2) + "\n";
    const auto graphPath = artifactsDir / "graph.json";
    std::error_code gec;
    std::filesystem::create_directories(graphPath.parent_path(), gec);
    const auto tmp = graphPath.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    std::filesystem::rename(tmp, graphPath, gec);
    if (gec)
        return std::unexpected(std::format(
            "cannot write the graph document '{}': {}",
            graphPath.string(), gec.message()));
    return std::pair{graphPath, mcpp::toolchain::hash_string(text)};
}

// The build programs of a workspace plan's members (workspace design
// 2026-09-29 §15). A member is the project being developed, so its program
// runs where a root's does: after every dependency's program, with the link
// form of each dependency and the graph document of what it reaches, and with
// its own artifacts directory, `<member>/target/.build-mcpp`, where it was
// when the member was the root of its own build. A workspace member reached as
// a dependency of another member runs here too, so its program sees the same
// environment in every selection.
static std::expected<void, std::string> step9_member_build_programs(PrepareState& state) {
    if (!state.m->package.virtualRoot) return {};
    // Dependencies first: a member's program runs after the programs of the
    // members it depends on, whose directives are then in place in the graph
    // it reads. Discovery order, which the packages are numbered in, puts a
    // member before its dependency when its key sorts first.
    mcpp::graph::AdjacencyList deps(state.packages.size());
    for (auto const& r : state.graphRequests)
        if (r.consumerPackageIndex < deps.size() && r.dependencyPackageIndex < deps.size()
            && r.consumerPackageIndex != r.dependencyPackageIndex)
            deps[r.consumerPackageIndex].push_back(r.dependencyPackageIndex);
    // Mutual dev-dependencies between members are legal, so a cycle skips its
    // closing edge and every other dependency still comes first.
    std::vector<std::size_t> roots(state.packages.size());
    std::iota(roots.begin(), roots.end(), std::size_t{0});
    const auto order = *mcpp::graph::depth_first_order(deps, roots, mcpp::graph::Cycles::Skip);
    auto hasProgram = [&](std::size_t i) {
        if (i == 0 || !state.isWorkspaceMemberPackage(i) || !state.compilesHere(i)) return false;
        std::error_code bpEc;
        return std::filesystem::exists(state.packages[i].root / "build.mcpp", bpEc)
            || !state.packages[i].manifest.buildConfig.ruleModules.empty();
    };
    // Their compiles may overlap (below), and their runs follow this order, so
    // every program not yet run is waiting for its turn; its line says so
    // (build progress design 2026-09-29, §3.2), named as the program names
    // itself.
    for (auto const i : order) {
        if (!hasProgram(i)) continue;
        const auto& m = state.packages[i].manifest;
        mcpp::build::progress::program_scheduled(
            m.package.namespace_.empty() ? m.package.name
                                         : m.package.namespace_ + "." + m.package.name,
            state.packages[i].selectedMember);
    }
    // THE ENVIRONMENT A MEMBER'S PROGRAM IS GIVEN. Computed from the plan as it
    // stands when it is asked, so the compile phase and the run each ask for it.
    auto program_env = [&](std::size_t i)
        -> std::expected<mcpp::build::BuildProgramEnv, std::string> {
        auto& pkg = state.packages[i];
        mcpp::build::BuildProgramEnv bpEnv;
        bpEnv.targetTriple = state.resolvedTargetCanonical;
        fill_target_build_env(bpEnv, *state.m, state.tc ? &*state.tc : nullptr,
                              state.cfg_opt ? &*state.cfg_opt : nullptr);
        bpEnv.toolsBin = state.projectSubosBin;
        bpEnv.profile  = state.effectiveProfile;
        bpEnv.accel    = state.resolvedAccel();
        fill_package_build_env(bpEnv, pkg.manifest);
        // The stage of the member this program acts for: its own, or the one
        // packed member that reaches it (`PrepareState::fillPackEnv`).
        state.fillPackEnv(bpEnv, i);
        bpEnv.requested       = pkg.selectedMember;
        bpEnv.languageModules = pkg.manifest.language.modules;
        bpEnv.ruleModules  = pkg.manifest.buildConfig.ruleModules;
        if (auto dit = state.deviceSourcesByPackage.find(pkg.root.string());
            dit != state.deviceSourcesByPackage.end())
            bpEnv.deviceSources = dit->second;
        bpEnv.artifactsDir = pkg.root / "target" / ".build-mcpp";
        if (i < state.activeFeaturesByPackage.size())
            bpEnv.features = state.activeFeaturesByPackage[i];
        // Beside the cache above, which is in the project under a plan's
        // work directory too: `emit build-database` reuses the build's run.
        bpEnv.outDir = state.programOutDir(*state.root, pkg.manifest, bpEnv.features);
        {
            std::map<std::size_t, std::string> linkForms;
            for (auto const& [idx, form] : state.dependencyLinkForms)
                if (form.recorded)
                    linkForms.emplace(idx, std::string(
                        mcpp::build::linkage_form::to_string(form.answer.linkage)));
            state.fillDepDirs(bpEnv, i, &linkForms);
        }
        state.fillXpkgDirs(bpEnv, pkg.manifest, i);
        if (auto tit = state.toolEnvByConsumer.find(i); tit != state.toolEnvByConsumer.end())
            bpEnv.toolPaths = tit->second;
        bpEnv.hostModules = state.hostModulesByConsumer.count(i)
            ? state.hostModulesByConsumer.at(i) : decltype(bpEnv.hostModules){};
        bpEnv.dormantFeatures = state.dormantFeaturesByConsumer.count(i)
            ? state.dormantFeaturesByConsumer.at(i) : decltype(bpEnv.dormantFeatures){};
        auto graph = write_graph_document(state, i, bpEnv.artifactsDir);
        if (!graph) return std::unexpected(graph.error());
        bpEnv.graphFile   = graph->first;
        bpEnv.graphDigest = graph->second;
        // What the program imports is kept once for the workspace, so that a host
        // module several members import is compiled once for all of them, and in
        // the global cache where it comes from the engine or the index (#748).
        bpEnv.moduleStore  = state.workRoot / "target" / ".build-mcpp" / "host-modules";
        if (state.cacheMode == CacheMode::Global)
            bpEnv.moduleCacheRoot = mcpp::home::cache_root();
        return bpEnv;
    };

    // THE COMPILES COME FIRST, AT THE SAME TIME (#748, B2). What a program needs
    // before it runs is a compile, and a compile depends on no other program's
    // run: only the runs have an order. So every program that needs compiling is
    // compiled now, up to the build's job count at once, and the programs are then
    // run in the order above, each taking the compile made for it.
    //
    // What the compile phase leaves out is what keeps the plan the serial build's
    // plan: it applies no directive, reports no outcome, states no warning and
    // writes no program cache. (It writes what the compile reads: the graph
    // document and a program synthesised from rules.) The runs below do the
    // rest, in the same order as before, so `build.ninja` and the directives
    // applied are byte for byte the serial build's. A program's environment is computed again at its turn, from
    // the plan as the programs before it left it; the compile is used only when
    // it was made for what that computes, and is made again otherwise.
    std::vector<std::size_t> turns;   // the programs, in the order they run
    for (auto const i : order)
        if (hasProgram(i)) turns.push_back(i);
    std::vector<mcpp::build::PrecompiledProgram> precompiled(turns.size());
    if (turns.size() >= 2) {
        if (auto host = state.host_tc_for_build_program()) {
            std::vector<std::optional<mcpp::build::BuildProgramEnv>> envs(turns.size());
            for (std::size_t k = 0; k < turns.size(); ++k)
                if (auto e = program_env(turns[k])) envs[k] = std::move(*e);

            int globalDefaultJobs = 0;
            if (auto c = state.get_cfg(/*requireBootstrap=*/false))
                globalDefaultJobs = static_cast<int>((*c)->defaultJobs);
            int jobs = mcpp::build::schedule::resolve_jobs(*state.m, {}, globalDefaultJobs);
            if (jobs <= 0)
                jobs = mcpp::platform::capacity::recommended_jobs(
                    mcpp::platform::capacity::host_capacity());
            const std::size_t workers = std::min<std::size_t>(
                turns.size(), static_cast<std::size_t>(std::max(jobs, 1)));

            std::atomic<std::size_t> next{0};
            // The first program, in the order they run, whose compile failed. A
            // program after it is never reached by a build that stops at the
            // first failure, so its compile is not started.
            std::atomic<std::size_t> firstFailure{turns.size()};
            auto work = [&] {
                for (;;) {
                    const auto k = next.fetch_add(1);
                    if (k >= turns.size()) return;
                    if (k > firstFailure.load() || !envs[k]) continue;
                    auto& pkg = state.packages[turns[k]];
                    try {
                        precompiled[k] = mcpp::build::precompile_build_program(
                            pkg.manifest, pkg.root, host->first, host->second,
                            pkg.manifest.cppStandard, *envs[k]);
                    } catch (const std::exception& ex) {
                        // A compile that threw is a compile that failed. Its
                        // empty stamp matches nothing, so the program's turn
                        // compiles it again, alone, and reports what that
                        // compile says.
                        precompiled[k] = {};
                        precompiled[k].compiled = true;
                        precompiled[k].stamp    = {};
                        precompiled[k].error    = std::format(
                            "build.mcpp failed to compile: {}", ex.what());
                    }
                    if (precompiled[k].compiled && !precompiled[k].error.empty()) {
                        auto seen = firstFailure.load();
                        while (k < seen && !firstFailure.compare_exchange_weak(seen, k)) {}
                    }
                }
            };
            const auto phaseBegan = std::chrono::steady_clock::now();
            std::vector<std::thread> pool;
            for (std::size_t w = 1; w < workers; ++w) {
                // A thread the system cannot start is one fewer worker, and the
                // compiles are shared by the ones that did.
                try { pool.emplace_back(work); } catch (const std::system_error&) { break; }
            }
            work();
            for (auto& t : pool) t.join();
            // The compiles overlapped, so the time they took is the phase's wall
            // time, stated once; each program's line still shows its own.
            mcpp::build::progress::programs_compiled(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - phaseBegan));
        }
    }

    std::size_t turn = 0;
    for (auto const i : order) {
        if (!hasProgram(i)) continue;
        const auto& made = precompiled[turn++];
        auto& pkg = state.packages[i];
        auto host = state.host_tc_for_build_program();
        if (!host) return std::unexpected(host.error());
        auto envOf = program_env(i);
        if (!envOf) return std::unexpected(envOf.error());
        auto& bpEnv = *envOf;

        auto& bc = pkg.manifest.buildConfig;
        const auto mark = state.markDirectiveTail(pkg.manifest);
        const auto actN = bc.actions.size();
        const auto runnerN = bc.runner.size();
        auto namedBefore = bc.namedRunners;
        const bool exclusiveBefore = bc.runExclusive;
        auto bp = run_answering_requests(state, pkg.manifest, bpEnv, i,
            pkg.manifest.package.name, [&] {
                return mcpp::build::run_build_program(
                    pkg.manifest, pkg.root, host->first, host->second,
                    pkg.manifest.cppStandard, bpEnv, made.compiled ? &made : nullptr);
            });
        if (!bp) {
            if (!state.overrides.plan_only)
                return std::unexpected(std::format(
                    "workspace member '{}': {}", pkg.manifest.package.name, bp.error()));
            state.planNotes.push_back({"MCPP_BUILD_DATABASE_PROGRAM_FAILED",
                std::format("workspace member '{}': {}", pkg.manifest.package.name, bp.error()),
                mcpp::wire::Severity::Error, (pkg.root / "build.mcpp").string()});
            state.programFailedPackages.insert(pkg.root.string());
            continue;
        }
        state.foldDirectiveTailIntoPrivateBuild(pkg, pkg.manifest, mark);
        state.adoptActionOutputs(pkg.manifest, pkg.root, actN);
        // How the artifact is executed reaches the plan's root, where `mcpp
        // run` reads it, by the rule the dependencies' runners follow: one
        // supplier per runner.
        auto& rootBc = state.m->buildConfig;
        if (bc.runner.size() > runnerN) {
            if (!rootBc.runner.empty() && !state.runnerProvider.empty())
                return std::unexpected(std::format(
                    "'{}' and '{}' both supply a runner for this target.\n"
                    "       Drop one of them, or state the runner in "
                    "[target.<triple>].runner.",
                    state.runnerProvider, pkg.manifest.package.name));
            rootBc.runner.assign(bc.runner.begin() + static_cast<std::ptrdiff_t>(runnerN),
                                 bc.runner.end());
            state.runnerProvider = pkg.manifest.package.name;
        }
        for (auto const& [name, nr] : bc.namedRunners) {
            auto before = namedBefore.find(name);
            const bool grew = before == namedBefore.end()
                           || nr.argv.size() > before->second.argv.size()
                           || (nr.longLived && !before->second.longLived);
            if (!grew) continue;
            auto& slot = rootBc.namedRunners[name];
            auto& who  = state.namedRunnerProvider[name];
            if (!slot.argv.empty() && !who.empty())
                return std::unexpected(std::format(
                    "'{}' and '{}' both supply a runner named '{}' for this target.",
                    who, pkg.manifest.package.name, name));
            slot = nr;
            who  = pkg.manifest.package.name;
        }
        if (bc.runExclusive && !exclusiveBefore) rootBc.runExclusive = true;
    }
    if (auto err = state.checkVersionFloors(); err) return std::unexpected(*err);
    // What the programs published (an include directory, an interface
    // define) reaches the members' consumers, as after the dependencies'
    // programs.
    state.computeUsageRequirements();
    return {};
}

static std::expected<void, std::string> step9_device_sources_reach_an_action(PrepareState& state) {
    // ── Every device source must reach some action ─────────────────────────
    //
    // A device-kind file is the one source the engine has no compile rule for.
    // It is handed to the package's build program (MCPP_DEVICE_SOURCES) and
    // comes back as an action, or it is not compiled at all. Nothing checked
    // that it came back. Two ways it does not, both silent until now:
    //
    //   - the package has no `build.mcpp`. The engine computed the list and
    //     dropped it. Both run sites above are guarded on that file existing,
    //     so there was not even a program to ignore it.
    //   - a program runs but no imported rule claims the extension. A project
    //     with a `.cu` and a `.comp` that imports only `mcpp.rules.spirv` is
    //     this case, and it is the ordinary case for a project with two
    //     backends: a rule takes the extensions it knows and leaves the rest.
    //
    // What they produce today is an undefined reference at the link, naming a
    // symbol and never the file that would have defined it -- and for a
    // `kind = "lib"` target not even that, because an archive is not resolved.
    // A device source that compiles nothing is never what was meant, so it is
    // refused here, where both halves of the fact are still in hand.
    //
    // THE CRITERION IS THE ACTION INPUTS, not "a build program ran": a program
    // that ran and consumed nothing is exactly the second case. It is also the
    // condition an action needs anyway -- one that compiles a file it does not
    // declare as an input does not rerun when that file changes -- so a rule
    // that satisfies it is a rule that rebuilds correctly.
    //
    // THE PREMISE OF THIS CHECK IS THE BUILD PROGRAM'S DIRECTIVES: an action
    // consuming a device source is one such directive. A package whose program
    // failed in this pass (`plan_only`, above) applied none of them, so every
    // device source would read as an orphan -- not a second defect, only the
    // shape the first one takes here. Such a package already carries its one
    // diagnostic, `MCPP_BUILD_DATABASE_PROGRAM_FAILED`; this check does not run
    // for it, exactly as SPEC-005 R5.2 now states (design 2026-09-27 §4.2,
    // mcpp#724 side finding A).
    for (std::size_t i = 0; i < state.packages.size(); ++i) {
        auto const& pkg = state.packages[i];
        if (state.programFailedPackages.contains(pkg.root.string())) continue;
        auto dit = state.deviceSourcesByPackage.find(pkg.root.string());
        if (dit == state.deviceSourcesByPackage.end() || dit->second.empty()) continue;
        auto const& mm = (i == 0) ? *state.m : pkg.manifest;
        std::set<std::filesystem::path> consumed;
        for (auto const& a : mm.buildConfig.actions)
            for (auto const& in : a.inputs) {
                std::filesystem::path ip(in);
                consumed.insert((ip.is_absolute() ? ip : pkg.root / ip).lexically_normal());
            }
        std::string orphans;
        for (auto const& rel : dit->second)
            if (!consumed.contains((pkg.root / rel).lexically_normal()))
                orphans += "         " + rel + "\n";
        if (orphans.empty()) continue;
        std::error_code hasEc;
        // The `programFailedPackages` skip above means this package's program,
        // if it has one, ran and succeeded — `exists(build.mcpp)` here can no
        // longer be true of a program that merely started and failed.
        const bool hasProgram = std::filesystem::exists(pkg.root / "build.mcpp", hasEc)
                              || !pkg.manifest.buildConfig.ruleModules.empty();
        refusal::record(refusal::Code::DeviceSourceUnconsumed);
        return std::unexpected(std::format(
            "`{}`: device sources that no action compiles:\n{}"
            "       A device-kind source is compiled by this package's build program\n"
            "       and by nothing else -- the engine has no rule for these extensions\n"
            "       and never will.\n"
            "{}",
            mm.package.name, orphans,
            hasProgram
                ? "       `build.mcpp` ran but declared no action taking them as inputs.\n"
                  "       fix: import the rule package that claims these extensions and\n"
                  "       call it, or drop them from `[build] sources`. A rule that\n"
                  "       compiles a file must also declare it as an action input, or the\n"
                  "       action will not rerun when the file changes."
                : "       This package has no `build.mcpp`, so nothing was ever offered\n"
                  "       them.\n"
                  "       fix: add a `build.mcpp` importing the rule for these files (e.g.\n"
                  "       `mcpp.rules.cuda` for `.cu`, `mcpp.rules.spirv` for shaders), or\n"
                  "       drop them from `[build] sources`."));
    }

    return {};
}

static std::expected<void, std::string> step9_rerun_input_prepare_dir(PrepareState& state) {
    // ── R1.3: a re-run input inside a `prepare` directory (SPEC-007 §3) ─────
    //
    // A build program's re-run set is declared BEFORE anything is built
    // (`rerun_if_changed`/`rerun_if_changed_glob`), and a `prepare` action's
    // directory is filled AFTER a build program has already run once for
    // this build — it is a ninja edge, scheduled after `mcpp build`'s
    // configure step ends. A program that also names a file or a glob inside
    // such a directory as its own re-run input reads a CONSTRUCTION RESULT
    // while it configures: correct on the SECOND build, once a previous
    // build's `prepare` action has populated the directory, and wrong on the
    // first — the exact pattern of a plugin placing a first installation's
    // libraries on the NEXT plan (design §5.2's route on 2026.9.26.1, which
    // this directive and role exist to remove).
    //
    // WARNED, NOT REFUSED: the program still configures correctly today (its
    // FIRST run sees what the tree already held), and R1.2 already asks a
    // program to say what it could not find with `mcpp::warning`. This is the
    // engine naming an author obligation SPEC-007 states (R1.3), not a build
    // it can complete no differently.
    //
    // `declared_program_inputs` reads back what every package's build.mcpp
    // just declared (or, on a cache hit, declared on its last run) from the
    // caches under `<workRoot>/target/.build-mcpp`, so no extra plumbing is
    // needed to carry the re-run set out of `run_build_program`.
    {
        std::map<std::filesystem::path, std::string> ownerName;
        std::vector<std::pair<std::string, std::filesystem::path>> prepareDirs;
        for (std::size_t i = 0; i < state.packages.size(); ++i) {
            auto const& mm = (i == 0) ? *state.m : state.packages[i].manifest;
            ownerName.emplace(state.packages[i].root.lexically_normal(), mm.package.name);
            for (auto const& a : mm.buildConfig.actions) {
                if (a.role != mcpp::manifest::BuildAction::Role::Prepare) continue;
                if (a.outputDir.empty()) continue;
                prepareDirs.emplace_back(mm.package.name,
                    std::filesystem::path(a.outputDir).lexically_normal());
            }
        }
        if (!prepareDirs.empty()) {
            // `p` reaches strictly inside `dir`: equal paths and a sibling
            // that merely shares a prefix (`lexically_relative` starting with
            // `..`) both do not count.
            auto isUnder = [](const std::filesystem::path& p,
                              const std::filesystem::path& dir) {
                auto rel = p.lexically_relative(dir);
                if (rel.empty()) return false;
                auto s = rel.generic_string();
                return s != "." && s.compare(0, 2, "..") != 0;
            };
            for (auto const& decl : mcpp::build::declared_program_inputs(state.workRoot)) {
                std::vector<std::filesystem::path> watched(decl.files);
                for (auto const& pattern : decl.globs) {
                    // The glob's fixed prefix — everything before its first
                    // wildcard character — is enough to answer whether the
                    // PATTERN reaches into a `prepare` directory; resolving it
                    // into the file set it matches is not needed for that.
                    auto wildcard = pattern.find_first_of("*?[");
                    auto fixed = wildcard == std::string::npos
                               ? pattern : pattern.substr(0, wildcard);
                    watched.push_back((decl.root / fixed).lexically_normal());
                }
                auto ownerIt = ownerName.find(decl.root.lexically_normal());
                const std::string declName =
                    ownerIt != ownerName.end() ? ownerIt->second : decl.root.string();
                for (auto const& w : watched) {
                    for (auto const& [pkgName, dir] : prepareDirs) {
                        if (!isUnder(w, dir)) continue;
                        mcpp::ui::warning(std::format(
                            "{}'s build.mcpp re-runs on '{}', which is inside "
                            "'{}', the directory package '{}' declared with a "
                            "`prepare` action's output_dir. That directory is "
                            "populated at BUILD time, after build.mcpp has "
                            "already configured, so this program sees the "
                            "PREVIOUS build's contents, never the current "
                            "one's (SPEC-007 R1.3).",
                            declName, w.string(), dir.string(), pkgName));
                    }
                }
            }
        }
    }

    // [targets.*] required_features gate: a target is emitted only when ALL its
    // required features are active in this build; otherwise it is silently
    // skipped. A pure build-selection knob — it runs before the modgraph/plan
    // so gated-out targets cost nothing.
    std::erase_if(state.m->targets, [&](const mcpp::manifest::Target& t) {
        for (auto const& rf : t.requiredFeatures)
            if (!state.activeRootFeatures.contains(rf)) return true;
        return false;
    });

    // The dialect-complete standard flag: spelled per-dialect and carrying
    // the module-graph-global dialect flags (issue #210). ONE string shared
    // by the p1689 scan and the std BMI prebuild so scan-time, prebuild-time
    // and compile-time dialect provably agree. Both this and make_plan go
    // through the same cppfly merge, so the c++fly gates (and the
    // c++latest/c++fly per-toolchain std spelling) stay graph-consistent.
    state.stdFlagAndDialect = mcpp::toolchain::cppfly::std_flag(
        *state.tc, state.m->cppStandard.canonical, state.m->cppStandard.level);
    if (state.m->cppStandard.experimental) {
        // c++fly is best-effort by design: say exactly what this toolchain
        // got and what it lacks (the value's contract, design §5.4).
        auto fly = mcpp::toolchain::cppfly::resolve(*state.tc);
        std::string enabled, skipped;
        for (auto& f : fly.features) {
            auto& dst = f.enabled ? enabled : skipped;
            if (!dst.empty()) dst += ", ";
            dst += f.name;
            if (f.enabled && !f.flags.empty()) dst += std::format(" ({})", f.flags);
            if (!f.enabled) dst += std::format(" ({})", f.reason);
        }
        // Narration, like every line that says what the build is doing.
        mcpp::ui::line(std::format("c++fly on {}: {}; enabled: {}; skipped: {}",
                                   state.tc->label(), state.stdFlagAndDialect,
                                   enabled.empty() ? "(none)" : enabled,
                                   skipped.empty() ? "(none)" : skipped));
    }
    for (auto& f : mcpp::toolchain::cppfly::effective_dialect_flags(
             *state.tc, state.m->cppStandard.experimental,
             mcpp::manifest::dialect_flags(state.m->buildConfig))) {
        state.stdFlagAndDialect += ' ';
        state.stdFlagAndDialect += f;
    }
    return {};
}

std::expected<void, std::string> phase9_target_side(PrepareState& state) {
    auto gather = step9_gather_target_side_candidates(state);
    if (!gather) return std::unexpected(gather.error());
    if (auto r = step9_resolve_and_realise_cabi(state, *gather); !r)
        return std::unexpected(r.error());
    if (auto r = step9_target_side_include_broadcast(state, *gather); !r)
        return std::unexpected(r.error());
    if (auto r = step9_kernel_abi_interfaces_and_requirements(state, *gather); !r)
        return std::unexpected(r.error());

    if (auto r = step9_layer_conditional_config(state); !r) return std::unexpected(r.error());
    if (auto r = step9_dependency_link_forms(state); !r) return std::unexpected(r.error());
    step9_define_graph_package_entry_closure(state);
    if (auto r = step9_root_build_program(state); !r) return std::unexpected(r.error());
    if (auto r = step9_member_build_programs(state); !r) return std::unexpected(r.error());
    if (auto r = step9_device_sources_reach_an_action(state); !r) return std::unexpected(r.error());
    if (auto r = step9_rerun_input_prepare_dir(state); !r) return std::unexpected(r.error());

    return {};
}

} // namespace mcpp::build
