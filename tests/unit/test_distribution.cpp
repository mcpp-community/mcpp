// The C++ runtime distribution contract table (issue #336).
//
// These tests exist because the thing they cover used to be five independent
// derivations of one decision, and the bug was that a new rule landed in some
// of them and not the others. So the assertions are deliberately about the
// TABLE's totality and about each cell's identity — not about "the flags look
// roughly right".


import std;
import mcpplibs.mtest;
import mcpp.build.distribution;

using namespace mcpplibs::mtest;

namespace dist = mcpp::build::dist;

namespace {

dist::MechanismInput macos_input() {
    dist::MechanismInput in;
    in.format           = dist::Format::MachO;
    in.stdlibId         = "libc++";
    in.macosFloor       = true;
    in.libcxxArchive    = "/tc/lib/libc++.a";
    in.libcxxAbiArchive = "/tc/lib/libc++abi.a";
    in.streamInitSymbolPresent = true;
    return in;
}

dist::MechanismInput linux_gcc_input() {
    dist::MechanismInput in;
    in.format   = dist::Format::Elf;
    in.stdlibId = "libstdc++";
    return in;
}

}  // namespace

// ---------------------------------------------------------------------------
// The regression this whole change exists for: `static_stdlib = false` (now
// `cxx_runtime = "host-coupled"`) was silently ignored for test binaries from
// 0.0.86 on, while the docs kept promising the opt-out. The table cannot
// reproduce that bug because the role is an INPUT to one function rather than
// a switch that picks between two independently-computed strings.
UTest host_coupled_reaches_test_binaries_on_macos{"Distribution/HostCoupledReachesTestBinariesOnMacos", [] {
    auto in = macos_input();
    in.requested = dist::Contract::HostCoupled;

    for (auto role : {dist::Role::Distributable, dist::Role::Test}) {
        in.role = role;
        auto m = dist::resolve(in);
        expect(eq(m.effective, dist::Contract::HostCoupled)) << dist::to_string(role);
        expect(eq(m.unitFlags, " -lc++"))                    << dist::to_string(role);
        expect(!m.degraded)                            << dist::to_string(role);
        // No static libc++ means no Mach-O initializer-ordering problem.
        expect(!m.streamInitShim)                      << dist::to_string(role);
    }
}};

// The other half of #336: by default a test binary keeps the SAME
// self-contained runtime as a shipped one. Flipping this default would
// re-open #202 (system libc++ dylib against toolchain libc++ headers →
// undefined __hash_memory on libc++ 22), so it is asserted, not assumed.
UTest tests_default_to_self_contained{"Distribution/TestsDefaultToSelfContained", [] {
    for (auto fmt : {dist::Format::Elf, dist::Format::MachO, dist::Format::Pe}) {
        expect(eq(dist::default_contract(dist::Role::Test, fmt), dist::Contract::SelfContained));
        expect(eq(dist::default_contract(dist::Role::Distributable, fmt), dist::Contract::SelfContained));
    }

    auto in = macos_input();
    in.role = dist::Role::Test;
    in.requested = dist::default_contract(dist::Role::Test, dist::Format::MachO);
    auto m = dist::resolve(in);
    expect(ne(m.unitFlags.find("-load_hidden"), std::string::npos));
    expect(bool(m.streamInitShim));
}};

// -load_hidden, not a plain by-path link: the visibility is the load-bearing
// half (PR #117 — default-visibility statics get unified with the system
// libc++ from dyld's shared cache and ostream<<int crosses copies).
UTest macos_self_contained_uses_hidden_archives{"Distribution/MacosSelfContainedUsesHiddenArchives", [] {
    auto in = macos_input();
    auto m  = dist::resolve(in);
    expect(eq(m.unitFlags, " -nostdlib++ -Wl,-load_hidden,/tc/lib/libc++.a"
              " -Wl,-load_hidden,/tc/lib/libc++abi.a"));
    expect(!m.degraded);
    expect(m.diagnostic.empty());
}};

// An archive is linked, never run: it embeds no runtime and imposes none.
UTest intermediate_carries_no_contract_flags{"Distribution/IntermediateCarriesNoContractFlags", [] {
    for (auto in : {macos_input(), linux_gcc_input()}) {
        in.role = dist::Role::Intermediate;
        in.requested = dist::Contract::SelfContained;
        expect(dist::resolve(in).unitFlags.empty());
    }
}};

// ---------------------------------------------------------------------------
// INV-4: a contract that cannot be honored is REPORTED. The pre-#336 code
// degraded silently in exactly these spots — a toolchain with no libc++.a fell
// back to `-lc++` and said nothing, so an artifact documented as portable to
// macOS 14 was quietly pinned to the build machine.
UTest missing_archives_degrade_loudly{"Distribution/MissingArchivesDegradeLoudly", [] {
    auto in = macos_input();
    in.libcxxArchive.clear();
    in.libcxxAbiArchive.clear();
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::HostCoupled));
    expect(bool(m.degraded));
    expect(!m.diagnostic.empty());
    expect(eq(m.unitFlags, " -lc++"));
}};

UTest missing_macos_floor_degrades_loudly{"Distribution/MissingMacosFloorDegradesLoudly", [] {
    auto in = macos_input();
    in.macosFloor = false;
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::HostCoupled));
    expect(bool(m.degraded));
    expect(!m.diagnostic.empty());
}};

// macOS has no toolchain-coupled form: LLVM's libc++abi/libunwind dylibs
// upward-link /usr/lib/libc++, so asking for one loads a SECOND libc++ into
// the process (#202 forensics). Refused with an explanation rather than
// half-honored.
UTest macos_toolchain_coupled_is_refused_not_faked{"Distribution/MacosToolchainCoupledIsRefusedNotFaked", [] {
    auto in = macos_input();
    in.requested = dist::Contract::ToolchainCoupled;
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::SelfContained));
    expect(bool(m.degraded));
    expect(ne(m.diagnostic.find("toolchain-coupled"), std::string::npos));
}};

// The silent no-op this model was built to make impossible: on a Linux/libc++
// toolchain, `static_stdlib = true` produced NO flag, NO warning and a
// toolchain-coupled binary — while the manifest, the docs and `--version` all
// said the artifact was self-contained.
UTest linux_libcxx_self_contained_is_real_or_loud{"Distribution/LinuxLibcxxSelfContainedIsRealOrLoud", [] {
    dist::MechanismInput in;
    in.format    = dist::Format::Elf;
    in.stdlibId  = "libc++";
    in.requested = dist::Contract::SelfContained;

    // No archives on this toolchain → say so, and name what you got instead.
    auto bare = dist::resolve(in);
    expect(eq(bare.effective, dist::Contract::ToolchainCoupled));
    expect(bool(bare.degraded));
    expect(!bare.diagnostic.empty());

    // Archives present → a real self-contained link. libunwind.a is part of
    // the mechanism, not a bonus: without it the binary still pulls
    // libunwind.so.1 and is not self-contained (verified against a real
    // llvm@22.1.8 payload).
    in.libcxxArchive     = "/tc/libc++.a";
    in.libcxxAbiArchive  = "/tc/libc++abi.a";
    in.libunwindArchive  = "/tc/libunwind.a";
    auto full = dist::resolve(in);
    expect(eq(full.effective, dist::Contract::SelfContained));
    expect(!full.degraded);
    expect(eq(full.unitFlags, " -nostdlib++ /tc/libc++.a /tc/libc++abi.a /tc/libunwind.a --unwindlib=none"));
    // Mach-O only — ELF sorts .init_array by priority, so the ordering bug
    // does not exist here (confirmed by running the repro on Linux).
    expect(!full.streamInitShim);

    in.libunwindArchive.clear();
    auto noUnwind = dist::resolve(in);
    expect(bool(noUnwind.degraded));
    expect(ne(noUnwind.diagnostic.find("libunwind"), std::string::npos));
    expect(eq(noUnwind.unitFlags.find("--unwindlib=none"), std::string::npos));

    in.libunwindArchive = "/tc/libunwind.a";
    for (auto contract : {dist::Contract::ToolchainCoupled, dist::Contract::HostCoupled}) {
        in.requested = contract;
        expect(eq(dist::resolve(in).unitFlags.find("--unwindlib=none"), std::string::npos));
    }
}};

// ---------------------------------------------------------------------------
// Byte-level parity with the pre-#336 emission on the platforms that were
// already correct. These strings are the whole reason the change is safe to
// ship: the Linux/Windows link is the same link, only assembled in one place.
UTest linux_gcc_parity{"Distribution/LinuxGccParity", [] {
    auto in = linux_gcc_input();
    expect(eq(dist::resolve(in).unitFlags, " -static-libstdc++"));

    in.requested = dist::Contract::HostCoupled;
    expect(eq(dist::resolve(in).unitFlags, ""));
    expect(!(dist::resolve(in).degraded));

    // ELF has no distinct emission for toolchain-coupled; the difference is
    // the rpath the link already carries, which this contract does not own.
    in.requested = dist::Contract::ToolchainCoupled;
    expect(eq(dist::resolve(in).unitFlags, ""));
    expect(!(dist::resolve(in).degraded));
}};

UTest mingw_parity{"Distribution/MingwParity", [] {
    dist::MechanismInput in;
    in.format   = dist::Format::Pe;
    in.stdlibId = "libstdc++";
    in.mingw    = true;

    // Whole-link -static is what "self-contained" means on MinGW: the
    // piecemeal -static-libstdc++ recipe still leaves libwinpthread-1.dll.
    expect(eq(dist::resolve(in).unitFlags, " -static -static-libstdc++"));

    in.hostIsWindows = true;
    expect(eq(dist::resolve(in).unitFlags, " -static -static-libstdc++ -static-libgcc"));

    // The libc axis is independent: `linkage = "static"` keeps -static even
    // when the C++ runtime is host-coupled.
    in.hostIsWindows   = false;
    in.requested       = dist::Contract::HostCoupled;
    in.fullStaticLibc  = true;
    expect(eq(dist::resolve(in).unitFlags, " -static"));
}};

// #718 §7.2/§7.3: the MSVC-ABI whole-project default. Undeclared resolves to
// toolchain-coupled when the toolset has a redistributable, and to
// host-coupled — silently — when it does not; `linkage = "static"` or an
// explicit `self-contained` both mean the static CRT regardless.
UTest msvc_abi_default_contract_row{"Distribution/MsvcAbiDefaultContractRow", [] {
    expect(eq(dist::msvc_abi_default_contract(/*staticCrt=*/false, /*hasRedist=*/true), dist::Contract::ToolchainCoupled));
    expect(eq(dist::msvc_abi_default_contract(/*staticCrt=*/false, /*hasRedist=*/false), dist::Contract::HostCoupled));
    expect(eq(dist::msvc_abi_default_contract(/*staticCrt=*/true, /*hasRedist=*/true), dist::Contract::SelfContained));
    expect(eq(dist::msvc_abi_default_contract(/*staticCrt=*/true, /*hasRedist=*/false), dist::Contract::SelfContained))
        << "a static CRT needs no redistributable to be self-contained";
}};

// `role_contracts` takes the SAME default for every role once
// `ContractStatement::msvcAbiDefault` is set — a per-role judgement about a
// format's hazard (the way MinGW's PE cell has one) has no place on this ABI.
UTest role_contracts_uses_the_msvc_abi_default_for_every_role{"Distribution/RoleContractsUsesTheMsvcAbiDefaultForEveryRole", [] {
    dist::ContractStatement s;
    s.msvcAbiDefault = dist::Contract::ToolchainCoupled;
    auto c = dist::role_contracts(s, dist::Format::Pe, {});
    expect(eq(c.program, dist::Contract::ToolchainCoupled));
    expect(eq(c.intermediate, dist::Contract::ToolchainCoupled));
    expect(eq(c.tests, dist::Contract::ToolchainCoupled));
    expect(eq(c.shared, dist::Contract::ToolchainCoupled));
    expect(!c.programStated);
    expect(!c.sharedStated);

    // An explicit statement still outranks the default, on this ABI as on
    // every other.
    s.cxxRuntime = "host-coupled";
    auto explicitC = dist::role_contracts(s, dist::Format::Pe, {});
    expect(eq(explicitC.program, dist::Contract::HostCoupled));
    expect(bool(explicitC.programStated));

    // Off the MSVC ABI (`msvcAbiDefault` unset), the format's own per-role
    // defaults are untouched — SharedLibrary still gets the ELF-only
    // toolchain-coupled default, not the MSVC-ABI one.
    dist::ContractStatement elf;
    auto elfC = dist::role_contracts(elf, dist::Format::Elf, {});
    expect(eq(elfC.program, dist::Contract::SelfContained));
    expect(eq(elfC.shared, dist::Contract::ToolchainCoupled));
}};

// MSVC's self-contained form IS the /MT runtime, and mcpp emits it — the
// switch is `msvcStaticCrt`, derived once by `msvc_wants_static_crt` from the
// two manifest keys that mean the same physical thing on this ABI.
//
// What the table must get right is that the switch is whole-PROJECT: cl bakes
// _MSVC_MT/_MSVC_MD into the one std module a project builds, so a per-role
// request that disagrees cannot be honoured and must say so.
UTest msvc_crt_model_is_whole_project_and_reported_as_such{"Distribution/MsvcCrtModelIsWholeProjectAndReportedAsSuch", [] {
    dist::MechanismInput in;
    in.format          = dist::Format::Pe;
    in.stdlibId        = "msvc";
    in.explicitRequest = true;

    // Project compiled /MT: self-contained is DELIVERED, not degraded.
    in.requested      = dist::Contract::SelfContained;
    in.msvcStaticCrt  = true;
    auto served = dist::resolve(in);
    expect(eq(served.effective, dist::Contract::SelfContained));
    expect(!served.degraded);
    expect(served.diagnostic.empty());
    // The CRT model is a COMPILE flag on every TU, never a link-line addition.
    expect(served.unitFlags.empty());

    // Project compiled /MD, one role asking for self-contained: refused, and
    // the message has to name the whole-project constraint rather than claim
    // the feature is missing.
    in.msvcStaticCrt = false;
    auto refused = dist::resolve(in);
    expect(eq(refused.effective, dist::Contract::HostCoupled));
    expect(bool(refused.degraded));
    expect(ne(refused.diagnostic.find("whole-project"), std::string::npos))
        << refused.diagnostic;
    expect(ne(refused.diagnostic.find("cxx_runtime"), std::string::npos))
        << refused.diagnostic;
    expect(refused.unitFlags.empty());

    // `linkage = "static"` is the same switch seen from the libc axis.
    in.fullStaticLibc = true;
    in.msvcStaticCrt  = true;    // as msvc_wants_static_crt would report it
    expect(eq(dist::resolve(in).effective, dist::Contract::SelfContained));
    in.fullStaticLibc = false;
    in.msvcStaticCrt  = false;

    in.requested = dist::Contract::HostCoupled;
    expect(!(dist::resolve(in).degraded));

    // ...and the DEFAULT must be quiet. Most roles default to the
    // self-contained contract, so a project that never mentioned the CRT gets
    // /MD and no complaint: a diagnostic is for a broken promise, not for a
    // default nobody asked about.
    in.requested       = dist::Contract::SelfContained;
    in.explicitRequest = false;
    auto quiet = dist::resolve(in);
    expect(!quiet.degraded);
    expect(quiet.diagnostic.empty());
    expect(eq(quiet.effective, dist::Contract::HostCoupled));
}};

// `toolchain-coupled` on the MSVC runtime used to be a flat refusal, and the
// sentence it refused with conflated two different DLLs:
//
//   ucrtbase.dll     an OS component since Win10 — the refusal was right
//   vcruntime140.dll the TOOLSET's own, sitting in VC\Redist\MSVC\… inside
//                    every toolset mcpp installs — the refusal was wrong
//
// The second one is exactly the relationship gcc has to libstdc++.so, so it
// takes the same contract. What differs is the MECHANISM: PE has no rpath, so
// the DLL travels by being copied beside the artifact.
UTest msvc_toolchain_coupled_stages_the_toolset_crt{"Distribution/MsvcToolchainCoupledStagesTheToolsetCrt", [] {
    dist::MechanismInput in;
    in.format          = dist::Format::Pe;
    in.stdlibId        = "msvc";
    in.explicitRequest = true;
    in.requested       = dist::Contract::ToolchainCoupled;

    // /MD: the artifact HAS a vcruntime140.dll dependency, so the contract is
    // deliverable — and delivering it means staging files, not adding flags.
    in.msvcStaticCrt = false;
    auto coupled = dist::resolve(in);
    expect(eq(coupled.effective, dist::Contract::ToolchainCoupled));
    expect(!coupled.degraded);
    expect(coupled.diagnostic.empty());
    expect(bool(coupled.deployToolchainRuntime));
    // The CRT model is a compile flag on every TU; nothing goes on the link line.
    expect(coupled.unitFlags.empty());

    // /MT is the one case that stays a degradation, and it is a genuine
    // contradiction rather than a missing mechanism: a static CRT leaves no
    // DLL to couple to. The message has to say which one won.
    in.msvcStaticCrt = true;
    auto contradiction = dist::resolve(in);
    expect(eq(contradiction.effective, dist::Contract::SelfContained));
    expect(bool(contradiction.degraded));
    expect(!contradiction.deployToolchainRuntime)
        << "a /MT build has no CRT DLL dependency; staging one is dead weight";
    expect(ne(contradiction.diagnostic.find("/MT"), std::string::npos))
        << contradiction.diagnostic;
    expect(ne(contradiction.diagnostic.find("self-contained"), std::string::npos))
        << contradiction.diagnostic;
}};

// Nothing but PE+MSVC+toolchain-coupled may ask for files to be staged. The
// flag reaches a copy step, so a stray `true` puts DLLs in an output tree on a
// platform that has no such thing.
UTest nothing_else_asks_for_staged_runtime_files{"Distribution/NothingElseAsksForStagedRuntimeFiles", [] {
    const dist::Contract contracts[] = {dist::Contract::SelfContained,
                                        dist::Contract::ToolchainCoupled,
                                        dist::Contract::HostCoupled};
    const dist::Format formats[] = {dist::Format::Elf, dist::Format::MachO,
                                    dist::Format::Pe};
    const std::string_view stdlibs[] = {"libstdc++", "libc++", "msvc", "surprise"};
    for (auto fmt : formats)
        for (auto sl : stdlibs)
            for (auto c : contracts)
                for (bool mt : {false, true})
                    for (bool explicitly : {false, true}) {
                        dist::MechanismInput in;
                        in.format          = fmt;
                        in.stdlibId        = sl;
                        in.requested       = c;
                        in.msvcStaticCrt   = mt;
                        in.explicitRequest = explicitly;
                        in.mingw           = (fmt == dist::Format::Pe
                                              && sl == "libstdc++");
                        auto m = dist::resolve(in);
                        if (!m.deployToolchainRuntime) continue;
                        expect(eq(fmt, dist::Format::Pe));
                        expect(ne(sl, std::string_view("libstdc++")));
                        expect(eq(c, dist::Contract::ToolchainCoupled));
                        expect(!mt);
                        // Staging files is a promise KEPT. A degraded cell did
                        // not deliver the contract, so it must not act as if
                        // it had.
                        expect(!m.degraded);
                        expect(eq(m.effective, dist::Contract::ToolchainCoupled));
                    }
}};

// ---------------------------------------------------------------------------
// INV-1, stated as a property rather than a list: the table is TOTAL, and
// every cell that does not deliver what was asked explains itself. A future
// stdlib/format/contract combination that forgets one or the other fails here.
UTest table_is_total_and_every_downgrade_explains_itself{"Distribution/TableIsTotalAndEveryDowngradeExplainsItself", [] {
    const dist::Contract contracts[] = {dist::Contract::SelfContained,
                                        dist::Contract::ToolchainCoupled,
                                        dist::Contract::HostCoupled};
    const dist::Format formats[] = {dist::Format::Elf, dist::Format::MachO,
                                    dist::Format::Pe};
    const std::string_view stdlibs[] = {"libstdc++", "libc++", "msvc", "surprise"};
    const dist::Role roles[] = {dist::Role::Distributable, dist::Role::Test,
                                dist::Role::Intermediate};

    for (auto c : contracts)
    for (auto fmt : formats)
    for (auto sl : stdlibs)
    for (auto role : roles)
    for (bool archives : {false, true}) {
        dist::MechanismInput in;
        in.requested       = c;
        in.format          = fmt;
        in.stdlibId        = sl;
        in.role            = role;
        in.macosFloor      = true;
        in.explicitRequest = true;   // the question is "what if you ASK for it"

        if (archives) {
            in.libcxxArchive    = "/a.a";
            in.libcxxAbiArchive = "/b.a";
            in.libunwindArchive = "/c.a";
            in.streamInitSymbolPresent = true;
        }
        auto m = dist::resolve(in);
        auto where = std::format("contract={} format={} stdlib={} role={} archives={}",
                                 dist::to_string(c), static_cast<int>(fmt), sl,
                                 dist::to_string(role), archives);
        // Totality: every cell answers with a contract it actually delivered.
        expect(m.effective == dist::Contract::SelfContained
                    || m.effective == dist::Contract::ToolchainCoupled
                    || m.effective == dist::Contract::HostCoupled) << where;
        // Honesty: a downgrade is never silent.
        if (m.degraded) expect(!m.diagnostic.empty()) << where;
        if (m.effective != c && role != dist::Role::Intermediate)
            expect(bool(m.degraded)) << where;
        // The ordering shim is a Mach-O static-libc++ concern and nothing else.
        if (m.streamInitShim) {
            expect(eq(fmt, dist::Format::MachO)) << where;
            expect(eq(m.effective, dist::Contract::SelfContained)) << where;
        }
    }
}};

// The generated shim is load-bearing in three specific ways; a well-meaning
// edit that drops any of them turns it into a silent no-op (or worse, an
// unconditional link failure on a toolchain that spells the symbol
// differently). See issue #336 for the disassembly this rests on.
UTest stream_init_shim_keeps_its_three_load_bearing_properties{"Distribution/StreamInitShimKeepsItsThreeLoadBearingProperties", [] {
    auto src = std::string(dist::stream_init_shim_source());
    // 1. weak_import — Mach-O's weak-UNDEFINED form. Plain `weak` is not it;
    //    the first CI round proved that by failing every macOS link.
    expect(ne(src.find("__attribute__((weak_import))"), std::string::npos));
    // 2. ios_base::Init::Init, NOT DoIOSInit::DoIOSInit — the former is
    //    guarded by __cxa_guard, so libc++'s own initializer later becomes a
    //    no-op instead of placement-new'ing over live streams.
    //    ...spelled with TWO leading underscores: an __asm__ label is used
    //    verbatim, so Mach-O's global `_` prefix has to be written out.
    expect(ne(src.find("\"__ZNSt3__18ios_base4InitC1Ev\""), std::string::npos));
    expect(eq(src.find("DoIOSInit"), std::string::npos));
    // 3. a constructor, and it must be guarded by the weak null check.
    expect(ne(src.find("__attribute__((constructor))"), std::string::npos));
    expect(ne(src.find("if (mcpp_libcxx_ios_init)"), std::string::npos));
    // C, not C++: no standard library, no module flags, no ABI of its own.
    expect(eq(src.find("#include"), std::string::npos));
}};

// The shim binds a libc++ INTERNAL symbol. If that symbol is not in the
// archive, mcpp must NOT generate the reference — an undefined symbol fails
// the link outright (ld64.lld does not treat a weak declaration as an
// optional undefined). The absence is reported instead, because the startup
// hazard is still there.
UTest shim_is_not_generated_when_the_symbol_is_absent{"Distribution/ShimIsNotGeneratedWhenTheSymbolIsAbsent", [] {
    auto in = macos_input();
    in.streamInitSymbolPresent = false;
    auto m = dist::resolve(in);
    expect(!m.streamInitShim);
    expect(!m.diagnostic.empty());
    // The contract itself is still honored — only the ordering aid is gone.
    expect(eq(m.effective, dist::Contract::SelfContained));
    expect(ne(m.unitFlags.find("-load_hidden"), std::string::npos));
}};

// ---------------------------------------------------------------------------
// Shared libraries.
//
// A .so is not a small executable: it is loaded INTO a process that already
// has a C++ runtime. On ELF that matters because there is ONE global symbol
// namespace and the first definition loaded wins — a .so that statically
// embedded libstdc++ exports ~3000 std symbols unversioned, the linker
// resolves the EXECUTABLE's std references against it (`-lfoo` precedes the
// driver's `-lstdc++`, so the archive member is never pulled), and the
// executable's own `-static-libstdc++` becomes a no-op. Swap that .so for
// another build of the same SONAME and `std::runtime_error::what()` is gone.
//
// The whole table is asserted cell by cell rather than "the ELF case", because
// the reason the other two formats keep the old answer is a real argument
// about each of them and a regression there would be silent.
UTest shared_library_default_is_format_specific{"Distribution/SharedLibraryDefaultIsFormatSpecific", [] {
    expect(eq(dist::default_contract(dist::Role::SharedLibrary, dist::Format::Elf), dist::Contract::ToolchainCoupled));
    // Mach-O: self-contained ALREADY means hidden (-load_hidden), so dyld
    // cannot unify the symbols; and toolchain-coupled is a documented dead end
    // there (#202). PE: no global namespace at all, imports resolve per-DLL.
    expect(eq(dist::default_contract(dist::Role::SharedLibrary, dist::Format::MachO), dist::Contract::SelfContained));
    expect(eq(dist::default_contract(dist::Role::SharedLibrary, dist::Format::Pe), dist::Contract::SelfContained));
}};

// The ELF mechanism for the new default: nothing. The driver links
// libstdc++.so and the toolchain's lib directory is already an -L and an rpath
// entry on the line. "No flag" has to be asserted or a future edit that adds
// one back would look like an improvement.
UTest shared_library_on_elf_embeds_nothing{"Distribution/SharedLibraryOnElfEmbedsNothing", [] {
    auto in = linux_gcc_input();
    in.role      = dist::Role::SharedLibrary;
    in.requested = dist::default_contract(dist::Role::SharedLibrary, dist::Format::Elf);
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::ToolchainCoupled));
    expect(eq(m.unitFlags, ""));
    expect(!m.degraded);
    expect(m.diagnostic.empty());
}};

// The escape hatch stays usable — and is guarded. Asking for a self-contained
// .so is legitimate; letting it export the embedded runtime is not.
UTest explicit_self_contained_shared_library_hides_the_embedded_runtime{"Distribution/ExplicitSelfContainedSharedLibraryHidesTheEmbeddedRuntime", [] {
    auto in = linux_gcc_input();
    in.role            = dist::Role::SharedLibrary;
    in.requested       = dist::Contract::SelfContained;
    in.explicitRequest = true;
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::SelfContained));
    expect(ne(m.unitFlags.find("-static-libstdc++"), std::string::npos)) << m.unitFlags;
    expect(ne(m.unitFlags.find("-Wl,--exclude-libs,libstdc++.a"), std::string::npos))
        << m.unitFlags;
    // No diagnostic: this is honored exactly as asked, not degraded.
    expect(!m.degraded);
}};

// The guard is for shared libraries only. An executable's static libstdc++ is
// already local (ld exports only what a loaded object references and mcpp
// passes no -rdynamic), so hiding it would be noise — and `--exclude-libs` on
// an executable link is a flag whose absence is part of the contract.
UTest executables_do_not_get_the_exclude_libs_guard{"Distribution/ExecutablesDoNotGetTheExcludeLibsGuard", [] {
    auto in = linux_gcc_input();
    in.role      = dist::Role::Distributable;
    in.requested = dist::Contract::SelfContained;
    auto m = dist::resolve(in);
    expect(eq(m.unitFlags, " -static-libstdc++"));
}};

// Same rule on the libc++/ELF path: the archives are linked by PATH, but
// --exclude-libs matches the archive BASENAME, so the names are spelled out.
UTest libcxx_self_contained_shared_library_hides_its_archives{"Distribution/LibcxxSelfContainedSharedLibraryHidesItsArchives", [] {
    dist::MechanismInput in;
    in.format            = dist::Format::Elf;
    in.stdlibId          = "libc++";
    in.role              = dist::Role::SharedLibrary;
    in.requested         = dist::Contract::SelfContained;
    in.explicitRequest   = true;
    in.libcxxArchive     = "/tc/lib/libc++.a";
    in.libcxxAbiArchive  = "/tc/lib/libc++abi.a";
    in.libunwindArchive  = "/tc/lib/libunwind.a";
    auto m = dist::resolve(in);
    for (auto needle : {"-Wl,--exclude-libs,libc++.a",
                        "-Wl,--exclude-libs,libc++abi.a",
                        "-Wl,--exclude-libs,libunwind.a"})
        expect(ne(m.unitFlags.find(needle), std::string::npos)) << needle
                                                               << " / " << m.unitFlags;
}};

// An archive embeds no runtime, so the role returns before any mechanism runs.
// Adding a fourth role must not have perturbed that early exit.
UTest intermediate_still_carries_no_mechanism{"Distribution/IntermediateStillCarriesNoMechanism", [] {
    auto in = linux_gcc_input();
    in.role      = dist::Role::Intermediate;
    in.requested = dist::Contract::SelfContained;
    auto m = dist::resolve(in);
    expect(eq(m.unitFlags, ""));
    expect(!m.degraded);
}};

// `ldStdlibByRole` indexes by the enum value. If a role is ever inserted
// rather than appended, every stored contract silently re-maps.
UTest role_count_covers_every_role{"Distribution/RoleCountCoversEveryRole", [] {
    expect(eq(static_cast<std::size_t>(dist::Role::SharedLibrary) + 1, dist::kRoleCount));
    for (auto r : {dist::Role::Distributable, dist::Role::Test,
                   dist::Role::Intermediate, dist::Role::SharedLibrary}) {
        expect(lt(static_cast<std::size_t>(r), dist::kRoleCount));
        expect(!(dist::to_string(r).empty()));
    }
}};

// ── The two short-circuits: when there is no C++ runtime to distribute WITH ──
//
// Every cell of the table above answers "how does this artefact carry its C++
// runtime", and all of the answers name a runtime to LINK — the system's, the
// toolchain's, or a static form of one. Two situations make all of them wrong
// rather than merely unnecessary, and in both the archives the table would
// reach for belong to the HOST.

UTest freestanding_carries_no_runtime_to_distribute{"Distribution/FreestandingCarriesNoRuntimeToDistribute", [] {
    dist::MechanismInput in;
    in.format       = dist::Format::Elf;
    in.stdlibId     = "libc++";
    in.freestanding = true;
    in.requested    = dist::Contract::SelfContained;
    in.role         = dist::Role::Distributable;
    // Deliberately present: the point is that they are NOT reached.
    in.libcxxArchive    = "/tc/lib/libc++.a";
    in.libcxxAbiArchive = "/tc/lib/libc++abi.a";
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::SelfContained));
    expect(eq(m.unitFlags, " -nostdlib++"));
    expect(eq(m.unitFlags.find("libc++.a"), std::string::npos));
    expect(!m.degraded);
}};

// The toolchain's archives built for another target: the payload's host
// libc++ found for an `aarch64-freebsd-musl` link. Not linked, and said: the
// row's C++ runtime is a package's. Measured before this existed:
// `ld.lld: error: ...x86_64-unknown-linux-gnu/libc++.a(algorithm.cpp.o) is incompatible`.
UTest foreign_archives_are_not_linked{"Distribution/ForeignArchivesAreNotLinked", [] {
    dist::MechanismInput in;
    in.format             = dist::Format::Elf;
    in.stdlibId           = "libc++";
    in.requested          = dist::Contract::SelfContained;
    in.role               = dist::Role::Distributable;
    in.foreignCxxArchives = true;
    in.foreignCxxTriple   = "x86_64-unknown-linux-gnu";
    auto m = dist::resolve(in);
    expect(eq(m.unitFlags, " -nostdlib++"));
    expect(m.degraded);
    expect(ne(m.diagnostic.find("x86_64-unknown-linux-gnu"), std::string::npos)) << m.diagnostic;
    expect(ne(m.diagnostic.find("mcpp:c++-abi"), std::string::npos)) << m.diagnostic;
}};

// The hosted form of the same fact. A package in the graph has compiled a
// C++ runtime FOR THIS TARGET and its objects are already on the link line, so
// there is no library to name and nothing to look for. Measured before this
// existed: `ld64.lld: error: library not found for -lc++`.
UTest graph_supplied_runtime_carries_no_library_to_name{"Distribution/GraphSuppliedRuntimeCarriesNoLibraryToName", [] {
    auto in = macos_input();
    in.graphCxxRuntime = true;
    in.requested       = dist::Contract::SelfContained;
    in.role            = dist::Role::Distributable;
    auto m = dist::resolve(in);
    expect(eq(m.effective, dist::Contract::SelfContained));
    expect(eq(m.unitFlags, " -nostdlib++"));
    expect(eq(m.unitFlags.find("-lc++"), std::string::npos));
    expect(eq(m.unitFlags.find("load_hidden"), std::string::npos));
}};

// The same on every format, because the fact is about the graph and not about
// the object format.
UTest graph_supplied_runtime_is_format_independent{"Distribution/GraphSuppliedRuntimeIsFormatIndependent", [] {
    for (auto fmt : {dist::Format::Elf, dist::Format::MachO, dist::Format::Pe}) {
        dist::MechanismInput in;
        in.format          = fmt;
        in.stdlibId        = "libc++";
        in.graphCxxRuntime = true;
        in.requested       = dist::Contract::SelfContained;
        in.role            = dist::Role::Distributable;
        auto m = dist::resolve(in);
        expect(eq(m.unitFlags, " -nostdlib++"))  ;
        expect(eq(m.effective, dist::Contract::SelfContained));
    }
}};

// And it does not depend on the contract the project asked for: a
// host-coupled request cannot be honoured by naming the system's runtime when
// the graph's is already inside the artefact.
UTest graph_supplied_runtime_ignores_the_requested_contract{"Distribution/GraphSuppliedRuntimeIgnoresTheRequestedContract", [] {
    for (auto c : {dist::Contract::SelfContained,
                   dist::Contract::ToolchainCoupled,
                   dist::Contract::HostCoupled}) {
        auto in = macos_input();
        in.graphCxxRuntime = true;
        in.requested       = c;
        in.role            = dist::Role::Distributable;
        auto m = dist::resolve(in);
        expect(eq(m.unitFlags, " -nostdlib++"));
    }
}};

// ── format_for: which format a target produces ──────────────────────────────
//
// This was a lambda inside a fifteen-hundred-line function and therefore had
// no test, and what it got wrong was found by running three hosts against three
// targets. The assertions below are the ones that would have found it in a
// second: the canonical spellings mcpp itself uses contain neither `apple` nor
// `darwin`, and the answer must not depend on the fallback.

UTest format_is_taken_from_the_target_and_not_the_fallback{"Distribution/FormatIsTakenFromTheTargetAndNotTheFallback", [] {
    // Every fallback, so that a target the vocabulary knows can never be
    // decided by the machine doing the building.
    for (auto fb : {dist::Format::Elf, dist::Format::MachO, dist::Format::Pe}) {
        expect(eq(dist::format_for("aarch64-macos", fb), dist::Format::MachO));
        expect(eq(dist::format_for("x86_64-macos", fb), dist::Format::MachO));
        // iOS shares macOS's Mach-O format. Before `format_for` asked
        // `is_mach_o()` instead of `os == "macos"`, this triple matched none
        // of the branches inside the parsed-triple block and fell through to
        // `fb` itself -- so the assertion below would have failed for two of
        // the three fallbacks in this loop, the exact "decided by the
        // machine doing the building" defect this test exists to catch.
        expect(eq(dist::format_for("aarch64-ios", fb), dist::Format::MachO));
        expect(eq(dist::format_for("x86_64-windows-gnu", fb), dist::Format::Pe));
        expect(eq(dist::format_for("x86_64-linux-gnu", fb), dist::Format::Elf));
        expect(eq(dist::format_for("aarch64-linux-musl", fb), dist::Format::Elf));
        expect(eq(dist::format_for("riscv64-none-elf", fb), dist::Format::Elf));
    }
}};

// The `[target.X]` escape hatch: a spelling the vocabulary cannot parse is all
// there is to go on, so LLVM's words are recognised there.
UTest format_falls_back_to_spelling_for_an_unparseable_triple{"Distribution/FormatFallsBackToSpellingForAnUnparseableTriple", [] {
    expect(eq(dist::format_for("arm64-apple-macos14.0", dist::Format::Elf), dist::Format::MachO));
    expect(eq(dist::format_for("x86_64-w64-mingw32", dist::Format::Elf), dist::Format::Pe));
    expect(eq(dist::format_for("x86_64-unknown-darwin", dist::Format::Elf), dist::Format::MachO));
}};

// And only then the host. A triple that says nothing at all is the one case
// where the machine doing the building is the best available answer.
UTest format_uses_the_fallback_only_when_the_triple_says_nothing{"Distribution/FormatUsesTheFallbackOnlyWhenTheTripleSaysNothing", [] {
    expect(eq(dist::format_for("", dist::Format::MachO), dist::Format::MachO));
    expect(eq(dist::format_for("", dist::Format::Pe), dist::Format::Pe));
    expect(eq(dist::format_for("nonsense", dist::Format::Elf), dist::Format::Elf));
}};

// ---------------------------------------------------------------------------
// A SECOND C++ RUNTIME ON THE LINE (mcpp#596).
//
// A lane whose device compiler is configured against libstdc++ puts libstdc++
// on the link line while the artifact links libc++ statically. Two things then
// have to change, and neither is a preference:
//
//   * one unwinder. Linking libunwind.a pulls in only the archive members
//     something referenced, so the interposition is partial by construction --
//     measured, ten of libgcc's eighteen entry points came from the artifact
//     and eight stayed in libgcc_s, including the accessors libstdc++'s
//     personality routine calls. It read an LLVM libunwind context through
//     libgcc's accessors and terminated past a matching handler.
//   * the static archives' symbols hidden. The linker exports what a loaded
//     object references, and a loaded libstdc++ references them: measured, 89
//     exported symbols of which 68 were also defined by libstdc++ or libgcc_s.
//
// The byte-level assertions distinguish the explicit static unwinder from
// libgcc in the foreign-runtime branch; neither selects a second unwinder.

UTest linux_libcxx_with_a_foreign_runtime_takes_one_unwinder{"Distribution/LinuxLibcxxWithAForeignRuntimeTakesOneUnwinder", [] {
    dist::MechanismInput in;
    in.format            = dist::Format::Elf;
    in.stdlibId          = "libc++";
    in.requested         = dist::Contract::SelfContained;
    in.libcxxArchive     = "/tc/libc++.a";
    in.libcxxAbiArchive  = "/tc/libc++abi.a";
    in.libunwindArchive  = "/tc/libunwind.a";

    // The explicit archive supplies the unwinder without a second driver
    // selection; the foreign-runtime branch still selects libgcc instead.
    auto alone = dist::resolve(in);
    expect(eq(alone.unitFlags, " -nostdlib++ /tc/libc++.a /tc/libc++abi.a /tc/libunwind.a --unwindlib=none"));

    in.foreignCxxRuntime = true;
    auto shared = dist::resolve(in);
    // The contract is NOT degraded: the C++ runtime is still embedded, and
    // libgcc_s is in the process either way because libstdc++ needs it.
    expect(eq(shared.effective, dist::Contract::SelfContained));
    expect(!shared.degraded);
    expect(shared.diagnostic.empty());
    expect(eq(shared.unitFlags, " -nostdlib++ /tc/libc++.a /tc/libc++abi.a"
              " -Wl,--exclude-libs,libc++.a -Wl,--exclude-libs,libc++abi.a"
              " --unwindlib=libgcc"));
    // The payload's unwinder archive is NOT on the line: linking it is what
    // creates the second unwinder.
    expect(eq(shared.unitFlags.find("libunwind.a"), std::string::npos));
}};

// A shared library retains archive privacy and uses the explicit unwinder.
UTest a_shared_library_still_hides_the_archives_without_a_second_runtime{"Distribution/ASharedLibraryStillHidesTheArchivesWithoutASecondRuntime", [] {
    dist::MechanismInput in;
    in.format            = dist::Format::Elf;
    in.stdlibId          = "libc++";
    in.role              = dist::Role::SharedLibrary;
    in.requested         = dist::Contract::SelfContained;
    in.libcxxArchive     = "/tc/libc++.a";
    in.libcxxAbiArchive  = "/tc/libc++abi.a";
    in.libunwindArchive  = "/tc/libunwind.a";
    auto m = dist::resolve(in);
    expect(eq(m.unitFlags, " -nostdlib++ /tc/libc++.a /tc/libc++abi.a"
              " -Wl,--exclude-libs,libc++.a -Wl,--exclude-libs,libc++abi.a"
              " /tc/libunwind.a --unwindlib=none -Wl,--exclude-libs,libunwind.a"));
}};

// THE NAMES `--exclude-libs` MATCHES ARE THE ARCHIVES THE LINKER OPENS
// (#634 A6). The Android NDK's per-API `libc++.a` is a linker script,
// `INPUT(-lc++_static -lc++abi)`: its members come from `libc++_static.a`, and
// hiding `libc++.a` alone left 161 dynamic symbols in a self-contained shared
// library where naming `libc++_static.a` left 4 (measured). The paths on the
// line are unchanged; only the hidden names grow, and only when given.
UTest a_linker_scripts_archives_are_the_names_hidden{"Distribution/ALinkerScriptsArchivesAreTheNamesHidden", [] {
    dist::MechanismInput in;
    in.format            = dist::Format::Elf;
    in.stdlibId          = "libc++";
    in.role              = dist::Role::SharedLibrary;
    in.requested         = dist::Contract::SelfContained;
    in.libcxxArchive     = "/ndk/sysroot/usr/lib/x86_64-linux-android/24/libc++.a";
    in.libcxxAbiArchive  = "/ndk/sysroot/usr/lib/x86_64-linux-android/libc++abi.a";
    in.libunwindArchive  = "/ndk/lib/clang/21/lib/linux/x86_64/libunwind.a";
    in.libcxxLinkedArchiveNames = {"libc++.a", "libc++abi.a", "libc++_static.a"};
    auto m = dist::resolve(in);
    expect(eq(m.unitFlags, " -nostdlib++ /ndk/sysroot/usr/lib/x86_64-linux-android/24/libc++.a"
              " /ndk/sysroot/usr/lib/x86_64-linux-android/libc++abi.a"
              " -Wl,--exclude-libs,libc++.a -Wl,--exclude-libs,libc++abi.a"
              " -Wl,--exclude-libs,libc++_static.a"
              " /ndk/lib/clang/21/lib/linux/x86_64/libunwind.a"
              " --unwindlib=none -Wl,--exclude-libs,libunwind.a"));

    // An executable still carries no guard, whatever the names are.
    in.role = dist::Role::Test;
    expect(eq(dist::resolve(in).unitFlags.find("exclude-libs"), std::string::npos));
}};

// ─── Format::Wasm, the member the module predicted and deferred ────────────
//
// `format_for`'s own comment said a wasm triple "falls out of every branch"
// and reaches `hostFallback`, answering the MACHINE's format -- "the same
// defect class its own header measured for macOS" -- and deferred the fourth
// member "to whoever gives this module a mechanism for it".
UTest format_answers_wasm_for_a_wasm_target_and_not_the_hosts{"Distribution/FormatAnswersWasmForAWasmTargetAndNotTheHosts", [] {
    using mcpp::build::dist::Format;
    using mcpp::build::dist::format_for;
    // Every host fallback, so this cannot pass by agreeing with the machine.
    for (auto fallback : {Format::Elf, Format::MachO, Format::Pe})
        expect(eq(format_for("wasm32-emscripten", fallback), Format::Wasm));
    // The three it already answered stay unchanged.
    expect(eq(format_for("x86_64-linux-gnu", Format::Pe), Format::Elf));
    expect(eq(format_for("aarch64-macos",    Format::Elf), Format::MachO));
    expect(eq(format_for("x86_64-windows-gnu", Format::Elf), Format::Pe));
}};

// THERE IS NOTHING TO BE COUPLED TO, so the contract is satisfied with
// nothing added and there is no degradation to report.
//
// While the member was missing, every wasm build printed a warning about a
// `libc++.so` that cannot exist for the target, on an artefact that has no
// run-time dependency of any kind. A diagnostic is for a BROKEN PROMISE; that
// one was a promise about a mechanism the format does not have.
UTest wasm_is_self_contained_by_construction_and_says_nothing{"Distribution/WasmIsSelfContainedByConstructionAndSaysNothing", [] {
    mcpp::build::dist::MechanismInput in;
    in.format    = mcpp::build::dist::Format::Wasm;
    in.stdlibId  = "libc++";
    in.requested = mcpp::build::dist::Contract::SelfContained;
    in.role      = mcpp::build::dist::Role::Distributable;
    auto m = mcpp::build::dist::resolve(in);
    expect(eq(m.effective, mcpp::build::dist::Contract::SelfContained));
    expect(!m.degraded);
    expect(m.diagnostic.empty()) << m.diagnostic;
    // And no flags: libc++ reaches a wasm link through `em++`'s own link line,
    // so naming archives from a sysroot this module did not resolve would be a
    // second answer to a question the driver has already answered.
    expect(m.unitFlags.empty()) << m.unitFlags;
}};

// ---------------------------------------------------------------------------
// #646 F3a -- one process, one C++ runtime.
//
// The ELF defaults give a program `self-contained` and a shared library
// `toolchain-coupled`. Each is right alone; a program that LOADS such a library
// held two C++ runtimes, and with llvm@22.1.8 it aborted with std::bad_cast.
// An unstated program or test contract takes the shared library's when its
// image loads a C++ shared library of the build.
UTest a_program_that_loads_a_cxx_shared_library_takes_its_contract_on_elf{"Distribution/AProgramThatLoadsACxxSharedLibraryTakesItsContractOnElf", [] {
    const dist::ContractStatement nothing{};
    auto c = dist::role_contracts(nothing, dist::Format::Elf,
                                  {.program = true, .tests = true});
    expect(eq(c.shared, dist::Contract::ToolchainCoupled));
    expect(eq(c.program, dist::Contract::ToolchainCoupled));
    expect(eq(c.tests, dist::Contract::ToolchainCoupled));
    // The intermediate role links no runtime and keeps the program default.
    expect(eq(c.intermediate, dist::Contract::SelfContained));
    expect(!c.programStated);
    expect(!(dist::runtime_split(c, dist::Format::Elf,
                                     {.program = true, .tests = true}).has_value()));
}};

// Nothing moves for a program that loads no C++ shared library: the default
// that makes a shipped binary portable is untouched.
UTest a_program_that_loads_no_cxx_shared_library_keeps_its_default{"Distribution/AProgramThatLoadsNoCxxSharedLibraryKeepsItsDefault", [] {
    auto c = dist::role_contracts({}, dist::Format::Elf, {});
    expect(eq(c.program, dist::Contract::SelfContained));
    expect(eq(c.tests, dist::Contract::SelfContained));
    auto onlyTests = dist::role_contracts({}, dist::Format::Elf, {.tests = true});
    expect(eq(onlyTests.program, dist::Contract::SelfContained));
    expect(eq(onlyTests.tests, dist::Contract::ToolchainCoupled));
}};

// Mach-O and PE keep their table: Mach-O's shared default is already
// self-contained (the F2 question is measured separately), and PE resolves
// imports per DLL.
UTest the_following_rule_is_elf_only{"Distribution/TheFollowingRuleIsElfOnly", [] {
    for (auto f : {dist::Format::MachO, dist::Format::Pe, dist::Format::Wasm}) {
        auto c = dist::role_contracts({}, f, {.program = true, .tests = true});
        expect(eq(c.program, dist::Contract::SelfContained));
        expect(!(dist::runtime_split(c, f, {.program = true}).has_value()));
    }
}};

// A STATED contract is never moved. A stated self-contained program over a
// coupled C++ shared library is the split, and is named for the caller to
// refuse; a stated self-contained project (the library self-contained too) is
// the documented private-copy arrangement and is not.
UTest a_stated_self_contained_program_over_a_coupled_library_is_a_split{"Distribution/AStatedSelfContainedProgramOverACoupledLibraryIsASplit", [] {
    const dist::ContractStatement split{
        .cxxRuntime = "self-contained", .cxxRuntimeShared = "toolchain-coupled"};
    auto c = dist::role_contracts(split, dist::Format::Elf, {.program = true});
    expect(eq(c.program, dist::Contract::SelfContained));
    expect(bool(c.programStated));
    auto role = dist::runtime_split(c, dist::Format::Elf, {.program = true});
    expect(fatal(role.has_value()));
    expect(eq(*role, dist::Role::Distributable));

    const dist::ContractStatement whole{.cxxRuntime = "self-contained"};
    auto w = dist::role_contracts(whole, dist::Format::Elf, {.program = true});
    expect(eq(w.shared, dist::Contract::SelfContained));
    expect(!(dist::runtime_split(w, dist::Format::Elf, {.program = true}).has_value()));

    const dist::ContractStatement tests{.cxxRuntimeTests = "self-contained"};
    auto t = dist::role_contracts(tests, dist::Format::Elf, {.tests = true});
    auto testRole = dist::runtime_split(t, dist::Format::Elf, {.tests = true});
    expect(fatal(testRole.has_value()));
    expect(eq(*testRole, dist::Role::Test));
}};

// `static_stdlib = false` is a statement for every role: all of them are
// host-coupled and nothing splits.
UTest the_older_spelling_states_every_role{"Distribution/TheOlderSpellingStatesEveryRole", [] {
    const dist::ContractStatement host{.staticStdlib = false};
    auto c = dist::role_contracts(host, dist::Format::Elf, {.program = true, .tests = true});
    expect(eq(c.program, dist::Contract::HostCoupled));
    expect(eq(c.shared, dist::Contract::HostCoupled));
    expect(!(dist::runtime_split(c, dist::Format::Elf, {.program = true}).has_value()));
}};

// ---------------------------------------------------------------------------
// #649 E10, closed by #718. Clang on the MSVC ABI used to be given no CRT
// model at all (`MechanismInput::msvcCrtModelEmitted`), so the table recorded
// `host-coupled` beside an artifact that imported no vcruntime DLL. Every
// MSVC-ABI row now receives the SAME model — one helper, `msvc_abi_crt_word`,
// spells it for cl and for clang++ alike — so the mechanism table has no cell
// left that distinguishes a driver: it reads only `msvcStaticCrt`,
// `requested` and `explicitRequest`, none of which name a compiler. That is
// the property that made the field removable rather than merely unused; a
// row without a redistributable directory is a planning-time refusal for an
// explicit `toolchain-coupled` (prepare/plan.cpp), not a table cell, and an
// undeclared one is resolved to `host-coupled` before this table ever runs
// (`dist::msvc_abi_default_contract`).
UTest no_mechanism_cell_distinguishes_clang_from_cl_on_the_msvc_abi{"Distribution/NoMechanismCellDistinguishesClangFromClOnTheMsvcAbi", [] {
    dist::MechanismInput in;
    in.format   = dist::Format::Pe;
    in.stdlibId = "msvc";

    // /MD, requesting (and delivering) the toolchain's own copy.
    in.msvcStaticCrt   = false;
    in.explicitRequest = true;
    in.requested       = dist::Contract::ToolchainCoupled;
    auto coupled = dist::resolve(in);
    expect(eq(coupled.effective, dist::Contract::ToolchainCoupled));
    expect(bool(coupled.deployToolchainRuntime));
    expect(!coupled.degraded);
    expect(coupled.diagnostic.empty());

    // /MT, requesting (and delivering) the static CRT.
    in.msvcStaticCrt = true;
    in.requested     = dist::Contract::SelfContained;
    auto selfContained = dist::resolve(in);
    expect(eq(selfContained.effective, dist::Contract::SelfContained));
    expect(!selfContained.degraded);
    expect(selfContained.diagnostic.empty());

    // The undeclared default: quiet, whichever way `msvcStaticCrt` reads.
    in.explicitRequest = false;
    in.msvcStaticCrt   = false;
    in.requested       = dist::Contract::HostCoupled;
    auto quiet = dist::resolve(in);
    expect(eq(quiet.effective, dist::Contract::HostCoupled));
    expect(!quiet.degraded);
    expect(quiet.diagnostic.empty());
}};
