// The C++ runtime distribution contract table (issue #336).
//
// These tests exist because the thing they cover used to be five independent
// derivations of one decision, and the bug was that a new rule landed in some
// of them and not the others. So the assertions are deliberately about the
// TABLE's totality and about each cell's identity — not about "the flags look
// roughly right".

#include <gtest/gtest.h>

import std;
import mcpp.build.distribution;

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
TEST(Distribution, HostCoupledReachesTestBinariesOnMacos) {
    auto in = macos_input();
    in.requested = dist::Contract::HostCoupled;

    for (auto role : {dist::Role::Distributable, dist::Role::Test}) {
        in.role = role;
        auto m = dist::resolve(in);
        EXPECT_EQ(m.effective, dist::Contract::HostCoupled) << dist::to_string(role);
        EXPECT_EQ(m.unitFlags, " -lc++")                    << dist::to_string(role);
        EXPECT_FALSE(m.degraded)                            << dist::to_string(role);
        // No static libc++ means no Mach-O initializer-ordering problem.
        EXPECT_FALSE(m.streamInitShim)                      << dist::to_string(role);
    }
}

// The other half of #336: by default a test binary keeps the SAME
// self-contained runtime as a shipped one. Flipping this default would
// re-open #202 (system libc++ dylib against toolchain libc++ headers →
// undefined __hash_memory on libc++ 22), so it is asserted, not assumed.
TEST(Distribution, TestsDefaultToSelfContained) {
    for (auto fmt : {dist::Format::Elf, dist::Format::MachO, dist::Format::Pe}) {
        EXPECT_EQ(dist::default_contract(dist::Role::Test, fmt),
                  dist::Contract::SelfContained);
        EXPECT_EQ(dist::default_contract(dist::Role::Distributable, fmt),
                  dist::Contract::SelfContained);
    }

    auto in = macos_input();
    in.role = dist::Role::Test;
    in.requested = dist::default_contract(dist::Role::Test, dist::Format::MachO);
    auto m = dist::resolve(in);
    EXPECT_NE(m.unitFlags.find("-load_hidden"), std::string::npos);
    EXPECT_TRUE(m.streamInitShim);
}

// -load_hidden, not a plain by-path link: the visibility is the load-bearing
// half (PR #117 — default-visibility statics get unified with the system
// libc++ from dyld's shared cache and ostream<<int crosses copies).
TEST(Distribution, MacosSelfContainedUsesHiddenArchives) {
    auto in = macos_input();
    auto m  = dist::resolve(in);
    EXPECT_EQ(m.unitFlags,
              " -nostdlib++ -Wl,-load_hidden,/tc/lib/libc++.a"
              " -Wl,-load_hidden,/tc/lib/libc++abi.a");
    EXPECT_FALSE(m.degraded);
    EXPECT_TRUE(m.diagnostic.empty());
}

// An archive is linked, never run: it embeds no runtime and imposes none.
TEST(Distribution, IntermediateCarriesNoContractFlags) {
    for (auto in : {macos_input(), linux_gcc_input()}) {
        in.role = dist::Role::Intermediate;
        in.requested = dist::Contract::SelfContained;
        EXPECT_TRUE(dist::resolve(in).unitFlags.empty());
    }
}

// ---------------------------------------------------------------------------
// INV-4: a contract that cannot be honored is REPORTED. The pre-#336 code
// degraded silently in exactly these spots — a toolchain with no libc++.a fell
// back to `-lc++` and said nothing, so an artifact documented as portable to
// macOS 14 was quietly pinned to the build machine.
TEST(Distribution, MissingArchivesDegradeLoudly) {
    auto in = macos_input();
    in.libcxxArchive.clear();
    in.libcxxAbiArchive.clear();
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::HostCoupled);
    EXPECT_TRUE(m.degraded);
    EXPECT_FALSE(m.diagnostic.empty());
    EXPECT_EQ(m.unitFlags, " -lc++");
}

TEST(Distribution, MissingMacosFloorDegradesLoudly) {
    auto in = macos_input();
    in.macosFloor = false;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::HostCoupled);
    EXPECT_TRUE(m.degraded);
    EXPECT_FALSE(m.diagnostic.empty());
}

// macOS has no toolchain-coupled form: LLVM's libc++abi/libunwind dylibs
// upward-link /usr/lib/libc++, so asking for one loads a SECOND libc++ into
// the process (#202 forensics). Refused with an explanation rather than
// half-honored.
TEST(Distribution, MacosToolchainCoupledIsRefusedNotFaked) {
    auto in = macos_input();
    in.requested = dist::Contract::ToolchainCoupled;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::SelfContained);
    EXPECT_TRUE(m.degraded);
    EXPECT_NE(m.diagnostic.find("toolchain-coupled"), std::string::npos);
}

// The silent no-op this model was built to make impossible: on a Linux/libc++
// toolchain, `static_stdlib = true` produced NO flag, NO warning and a
// toolchain-coupled binary — while the manifest, the docs and `--version` all
// said the artifact was self-contained.
TEST(Distribution, LinuxLibcxxSelfContainedIsRealOrLoud) {
    dist::MechanismInput in;
    in.format    = dist::Format::Elf;
    in.stdlibId  = "libc++";
    in.requested = dist::Contract::SelfContained;

    // No archives on this toolchain → say so, and name what you got instead.
    auto bare = dist::resolve(in);
    EXPECT_EQ(bare.effective, dist::Contract::ToolchainCoupled);
    EXPECT_TRUE(bare.degraded);
    EXPECT_FALSE(bare.diagnostic.empty());

    // Archives present → a real self-contained link. libunwind.a is part of
    // the mechanism, not a bonus: without it the binary still pulls
    // libunwind.so.1 and is not self-contained (verified against a real
    // llvm@22.1.8 payload).
    in.libcxxArchive     = "/tc/libc++.a";
    in.libcxxAbiArchive  = "/tc/libc++abi.a";
    in.libunwindArchive  = "/tc/libunwind.a";
    auto full = dist::resolve(in);
    EXPECT_EQ(full.effective, dist::Contract::SelfContained);
    EXPECT_FALSE(full.degraded);
    EXPECT_EQ(full.unitFlags,
              " -nostdlib++ /tc/libc++.a /tc/libc++abi.a /tc/libunwind.a");
    // Mach-O only — ELF sorts .init_array by priority, so the ordering bug
    // does not exist here (confirmed by running the repro on Linux).
    EXPECT_FALSE(full.streamInitShim);

    in.libunwindArchive.clear();
    auto noUnwind = dist::resolve(in);
    EXPECT_TRUE(noUnwind.degraded);
    EXPECT_NE(noUnwind.diagnostic.find("libunwind"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Byte-level parity with the pre-#336 emission on the platforms that were
// already correct. These strings are the whole reason the change is safe to
// ship: the Linux/Windows link is the same link, only assembled in one place.
TEST(Distribution, LinuxGccParity) {
    auto in = linux_gcc_input();
    EXPECT_EQ(dist::resolve(in).unitFlags, " -static-libstdc++");

    in.requested = dist::Contract::HostCoupled;
    EXPECT_EQ(dist::resolve(in).unitFlags, "");
    EXPECT_FALSE(dist::resolve(in).degraded);

    // ELF has no distinct emission for toolchain-coupled; the difference is
    // the rpath the link already carries, which this contract does not own.
    in.requested = dist::Contract::ToolchainCoupled;
    EXPECT_EQ(dist::resolve(in).unitFlags, "");
    EXPECT_FALSE(dist::resolve(in).degraded);
}

TEST(Distribution, MingwParity) {
    dist::MechanismInput in;
    in.format   = dist::Format::Pe;
    in.stdlibId = "libstdc++";
    in.mingw    = true;

    // Whole-link -static is what "self-contained" means on MinGW: the
    // piecemeal -static-libstdc++ recipe still leaves libwinpthread-1.dll.
    EXPECT_EQ(dist::resolve(in).unitFlags, " -static -static-libstdc++");

    in.hostIsWindows = true;
    EXPECT_EQ(dist::resolve(in).unitFlags,
              " -static -static-libstdc++ -static-libgcc");

    // The libc axis is independent: `linkage = "static"` keeps -static even
    // when the C++ runtime is host-coupled.
    in.hostIsWindows   = false;
    in.requested       = dist::Contract::HostCoupled;
    in.fullStaticLibc  = true;
    EXPECT_EQ(dist::resolve(in).unitFlags, " -static");
}

// MSVC's self-contained form IS the /MT runtime, and mcpp emits it — the
// switch is `msvcStaticCrt`, derived once by `msvc_wants_static_crt` from the
// two manifest keys that mean the same physical thing on this ABI.
//
// What the table must get right is that the switch is whole-PROJECT: cl bakes
// _MSVC_MT/_MSVC_MD into the one std module a project builds, so a per-role
// request that disagrees cannot be honoured and must say so.
TEST(Distribution, MsvcCrtModelIsWholeProjectAndReportedAsSuch) {
    dist::MechanismInput in;
    in.format          = dist::Format::Pe;
    in.stdlibId        = "msvc";
    in.explicitRequest = true;

    // Project compiled /MT: self-contained is DELIVERED, not degraded.
    in.requested      = dist::Contract::SelfContained;
    in.msvcStaticCrt  = true;
    auto served = dist::resolve(in);
    EXPECT_EQ(served.effective, dist::Contract::SelfContained);
    EXPECT_FALSE(served.degraded);
    EXPECT_TRUE(served.diagnostic.empty());
    // The CRT model is a COMPILE flag on every TU, never a link-line addition.
    EXPECT_TRUE(served.unitFlags.empty());

    // Project compiled /MD, one role asking for self-contained: refused, and
    // the message has to name the whole-project constraint rather than claim
    // the feature is missing.
    in.msvcStaticCrt = false;
    auto refused = dist::resolve(in);
    EXPECT_EQ(refused.effective, dist::Contract::HostCoupled);
    EXPECT_TRUE(refused.degraded);
    EXPECT_NE(refused.diagnostic.find("whole-project"), std::string::npos)
        << refused.diagnostic;
    EXPECT_NE(refused.diagnostic.find("cxx_runtime"), std::string::npos)
        << refused.diagnostic;
    EXPECT_TRUE(refused.unitFlags.empty());

    // `linkage = "static"` is the same switch seen from the libc axis.
    in.fullStaticLibc = true;
    in.msvcStaticCrt  = true;    // as msvc_wants_static_crt would report it
    EXPECT_EQ(dist::resolve(in).effective, dist::Contract::SelfContained);
    in.fullStaticLibc = false;
    in.msvcStaticCrt  = false;

    in.requested = dist::Contract::HostCoupled;
    EXPECT_FALSE(dist::resolve(in).degraded);

    // ...and the DEFAULT must be quiet. Most roles default to the
    // self-contained contract, so a project that never mentioned the CRT gets
    // /MD and no complaint: a diagnostic is for a broken promise, not for a
    // default nobody asked about.
    in.requested       = dist::Contract::SelfContained;
    in.explicitRequest = false;
    auto quiet = dist::resolve(in);
    EXPECT_FALSE(quiet.degraded);
    EXPECT_TRUE(quiet.diagnostic.empty());
    EXPECT_EQ(quiet.effective, dist::Contract::HostCoupled);
}

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
TEST(Distribution, MsvcToolchainCoupledStagesTheToolsetCrt) {
    dist::MechanismInput in;
    in.format          = dist::Format::Pe;
    in.stdlibId        = "msvc";
    in.explicitRequest = true;
    in.requested       = dist::Contract::ToolchainCoupled;

    // /MD: the artifact HAS a vcruntime140.dll dependency, so the contract is
    // deliverable — and delivering it means staging files, not adding flags.
    in.msvcStaticCrt = false;
    auto coupled = dist::resolve(in);
    EXPECT_EQ(coupled.effective, dist::Contract::ToolchainCoupled);
    EXPECT_FALSE(coupled.degraded);
    EXPECT_TRUE(coupled.diagnostic.empty());
    EXPECT_TRUE(coupled.deployToolchainRuntime);
    // The CRT model is a compile flag on every TU; nothing goes on the link line.
    EXPECT_TRUE(coupled.unitFlags.empty());

    // /MT is the one case that stays a degradation, and it is a genuine
    // contradiction rather than a missing mechanism: a static CRT leaves no
    // DLL to couple to. The message has to say which one won.
    in.msvcStaticCrt = true;
    auto contradiction = dist::resolve(in);
    EXPECT_EQ(contradiction.effective, dist::Contract::SelfContained);
    EXPECT_TRUE(contradiction.degraded);
    EXPECT_FALSE(contradiction.deployToolchainRuntime)
        << "a /MT build has no CRT DLL dependency; staging one is dead weight";
    EXPECT_NE(contradiction.diagnostic.find("/MT"), std::string::npos)
        << contradiction.diagnostic;
    EXPECT_NE(contradiction.diagnostic.find("self-contained"), std::string::npos)
        << contradiction.diagnostic;
}

// Nothing but PE+MSVC+toolchain-coupled may ask for files to be staged. The
// flag reaches a copy step, so a stray `true` puts DLLs in an output tree on a
// platform that has no such thing.
TEST(Distribution, NothingElseAsksForStagedRuntimeFiles) {
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
                        EXPECT_EQ(fmt, dist::Format::Pe);
                        EXPECT_NE(sl, std::string_view("libstdc++"));
                        EXPECT_EQ(c, dist::Contract::ToolchainCoupled);
                        EXPECT_FALSE(mt);
                        // Staging files is a promise KEPT. A degraded cell did
                        // not deliver the contract, so it must not act as if
                        // it had.
                        EXPECT_FALSE(m.degraded);
                        EXPECT_EQ(m.effective, dist::Contract::ToolchainCoupled);
                    }
}

// ---------------------------------------------------------------------------
// INV-1, stated as a property rather than a list: the table is TOTAL, and
// every cell that does not deliver what was asked explains itself. A future
// stdlib/format/contract combination that forgets one or the other fails here.
TEST(Distribution, TableIsTotalAndEveryDowngradeExplainsItself) {
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
        EXPECT_TRUE(m.effective == dist::Contract::SelfContained
                    || m.effective == dist::Contract::ToolchainCoupled
                    || m.effective == dist::Contract::HostCoupled) << where;
        // Honesty: a downgrade is never silent.
        if (m.degraded) EXPECT_FALSE(m.diagnostic.empty()) << where;
        if (m.effective != c && role != dist::Role::Intermediate)
            EXPECT_TRUE(m.degraded) << where;
        // The ordering shim is a Mach-O static-libc++ concern and nothing else.
        if (m.streamInitShim) {
            EXPECT_EQ(fmt, dist::Format::MachO) << where;
            EXPECT_EQ(m.effective, dist::Contract::SelfContained) << where;
        }
    }
}

// The generated shim is load-bearing in three specific ways; a well-meaning
// edit that drops any of them turns it into a silent no-op (or worse, an
// unconditional link failure on a toolchain that spells the symbol
// differently). See issue #336 for the disassembly this rests on.
TEST(Distribution, StreamInitShimKeepsItsThreeLoadBearingProperties) {
    auto src = std::string(dist::stream_init_shim_source());
    // 1. weak_import — Mach-O's weak-UNDEFINED form. Plain `weak` is not it;
    //    the first CI round proved that by failing every macOS link.
    EXPECT_NE(src.find("__attribute__((weak_import))"), std::string::npos);
    // 2. ios_base::Init::Init, NOT DoIOSInit::DoIOSInit — the former is
    //    guarded by __cxa_guard, so libc++'s own initializer later becomes a
    //    no-op instead of placement-new'ing over live streams.
    //    ...spelled with TWO leading underscores: an __asm__ label is used
    //    verbatim, so Mach-O's global `_` prefix has to be written out.
    EXPECT_NE(src.find("\"__ZNSt3__18ios_base4InitC1Ev\""), std::string::npos);
    EXPECT_EQ(src.find("DoIOSInit"), std::string::npos);
    // 3. a constructor, and it must be guarded by the weak null check.
    EXPECT_NE(src.find("__attribute__((constructor))"), std::string::npos);
    EXPECT_NE(src.find("if (mcpp_libcxx_ios_init)"), std::string::npos);
    // C, not C++: no standard library, no module flags, no ABI of its own.
    EXPECT_EQ(src.find("#include"), std::string::npos);
}

// The shim binds a libc++ INTERNAL symbol. If that symbol is not in the
// archive, mcpp must NOT generate the reference — an undefined symbol fails
// the link outright (ld64.lld does not treat a weak declaration as an
// optional undefined). The absence is reported instead, because the startup
// hazard is still there.
TEST(Distribution, ShimIsNotGeneratedWhenTheSymbolIsAbsent) {
    auto in = macos_input();
    in.streamInitSymbolPresent = false;
    auto m = dist::resolve(in);
    EXPECT_FALSE(m.streamInitShim);
    EXPECT_FALSE(m.diagnostic.empty());
    // The contract itself is still honored — only the ordering aid is gone.
    EXPECT_EQ(m.effective, dist::Contract::SelfContained);
    EXPECT_NE(m.unitFlags.find("-load_hidden"), std::string::npos);
}

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
TEST(Distribution, SharedLibraryDefaultIsFormatSpecific) {
    EXPECT_EQ(dist::default_contract(dist::Role::SharedLibrary, dist::Format::Elf),
              dist::Contract::ToolchainCoupled);
    // Mach-O: self-contained ALREADY means hidden (-load_hidden), so dyld
    // cannot unify the symbols; and toolchain-coupled is a documented dead end
    // there (#202). PE: no global namespace at all, imports resolve per-DLL.
    EXPECT_EQ(dist::default_contract(dist::Role::SharedLibrary, dist::Format::MachO),
              dist::Contract::SelfContained);
    EXPECT_EQ(dist::default_contract(dist::Role::SharedLibrary, dist::Format::Pe),
              dist::Contract::SelfContained);
}

// The ELF mechanism for the new default: nothing. The driver links
// libstdc++.so and the toolchain's lib directory is already an -L and an rpath
// entry on the line. "No flag" has to be asserted or a future edit that adds
// one back would look like an improvement.
TEST(Distribution, SharedLibraryOnElfEmbedsNothing) {
    auto in = linux_gcc_input();
    in.role      = dist::Role::SharedLibrary;
    in.requested = dist::default_contract(dist::Role::SharedLibrary, dist::Format::Elf);
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::ToolchainCoupled);
    EXPECT_EQ(m.unitFlags, "");
    EXPECT_FALSE(m.degraded);
    EXPECT_TRUE(m.diagnostic.empty());
}

// The escape hatch stays usable — and is guarded. Asking for a self-contained
// .so is legitimate; letting it export the embedded runtime is not.
TEST(Distribution, ExplicitSelfContainedSharedLibraryHidesTheEmbeddedRuntime) {
    auto in = linux_gcc_input();
    in.role            = dist::Role::SharedLibrary;
    in.requested       = dist::Contract::SelfContained;
    in.explicitRequest = true;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::SelfContained);
    EXPECT_NE(m.unitFlags.find("-static-libstdc++"), std::string::npos) << m.unitFlags;
    EXPECT_NE(m.unitFlags.find("-Wl,--exclude-libs,libstdc++.a"), std::string::npos)
        << m.unitFlags;
    // No diagnostic: this is honored exactly as asked, not degraded.
    EXPECT_FALSE(m.degraded);
}

// The guard is for shared libraries only. An executable's static libstdc++ is
// already local (ld exports only what a loaded object references and mcpp
// passes no -rdynamic), so hiding it would be noise — and `--exclude-libs` on
// an executable link is a flag whose absence is part of the contract.
TEST(Distribution, ExecutablesDoNotGetTheExcludeLibsGuard) {
    auto in = linux_gcc_input();
    in.role      = dist::Role::Distributable;
    in.requested = dist::Contract::SelfContained;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.unitFlags, " -static-libstdc++");
}

// Same rule on the libc++/ELF path: the archives are linked by PATH, but
// --exclude-libs matches the archive BASENAME, so the names are spelled out.
TEST(Distribution, LibcxxSelfContainedSharedLibraryHidesItsArchives) {
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
        EXPECT_NE(m.unitFlags.find(needle), std::string::npos) << needle
                                                               << " / " << m.unitFlags;
}

// An archive embeds no runtime, so the role returns before any mechanism runs.
// Adding a fourth role must not have perturbed that early exit.
TEST(Distribution, IntermediateStillCarriesNoMechanism) {
    auto in = linux_gcc_input();
    in.role      = dist::Role::Intermediate;
    in.requested = dist::Contract::SelfContained;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.unitFlags, "");
    EXPECT_FALSE(m.degraded);
}

// `ldStdlibByRole` indexes by the enum value. If a role is ever inserted
// rather than appended, every stored contract silently re-maps.
TEST(Distribution, RoleCountCoversEveryRole) {
    EXPECT_EQ(static_cast<std::size_t>(dist::Role::SharedLibrary) + 1,
              dist::kRoleCount);
    for (auto r : {dist::Role::Distributable, dist::Role::Test,
                   dist::Role::Intermediate, dist::Role::SharedLibrary}) {
        EXPECT_LT(static_cast<std::size_t>(r), dist::kRoleCount);
        EXPECT_FALSE(dist::to_string(r).empty());
    }
}

// ── The two short-circuits: when there is no C++ runtime to distribute WITH ──
//
// Every cell of the table above answers "how does this artefact carry its C++
// runtime", and all of the answers name a runtime to LINK — the system's, the
// toolchain's, or a static form of one. Two situations make all of them wrong
// rather than merely unnecessary, and in both the archives the table would
// reach for belong to the HOST.

TEST(Distribution, FreestandingCarriesNoRuntimeToDistribute) {
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
    EXPECT_EQ(m.effective, dist::Contract::SelfContained);
    EXPECT_EQ(m.unitFlags, " -nostdlib++");
    EXPECT_EQ(m.unitFlags.find("libc++.a"), std::string::npos);
    EXPECT_FALSE(m.degraded);
}

// ⚠️ The hosted form of the same fact. A package in the graph has compiled a
// C++ runtime FOR THIS TARGET and its objects are already on the link line, so
// there is no library to name and nothing to look for. Measured before this
// existed: `ld64.lld: error: library not found for -lc++`.
TEST(Distribution, GraphSuppliedRuntimeCarriesNoLibraryToName) {
    auto in = macos_input();
    in.graphCxxRuntime = true;
    in.requested       = dist::Contract::SelfContained;
    in.role            = dist::Role::Distributable;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::SelfContained);
    EXPECT_EQ(m.unitFlags, " -nostdlib++");
    EXPECT_EQ(m.unitFlags.find("-lc++"), std::string::npos);
    EXPECT_EQ(m.unitFlags.find("load_hidden"), std::string::npos);
}

// The same on every format, because the fact is about the graph and not about
// the object format.
TEST(Distribution, GraphSuppliedRuntimeIsFormatIndependent) {
    for (auto fmt : {dist::Format::Elf, dist::Format::MachO, dist::Format::Pe}) {
        dist::MechanismInput in;
        in.format          = fmt;
        in.stdlibId        = "libc++";
        in.graphCxxRuntime = true;
        in.requested       = dist::Contract::SelfContained;
        in.role            = dist::Role::Distributable;
        auto m = dist::resolve(in);
        EXPECT_EQ(m.unitFlags, " -nostdlib++")  ;
        EXPECT_EQ(m.effective, dist::Contract::SelfContained);
    }
}

// ⚠️ And it does not depend on the contract the project asked for: a
// host-coupled request cannot be honoured by naming the system's runtime when
// the graph's is already inside the artefact.
TEST(Distribution, GraphSuppliedRuntimeIgnoresTheRequestedContract) {
    for (auto c : {dist::Contract::SelfContained,
                   dist::Contract::ToolchainCoupled,
                   dist::Contract::HostCoupled}) {
        auto in = macos_input();
        in.graphCxxRuntime = true;
        in.requested       = c;
        in.role            = dist::Role::Distributable;
        auto m = dist::resolve(in);
        EXPECT_EQ(m.unitFlags, " -nostdlib++");
    }
}

// ── format_for: which format a target produces ──────────────────────────────
//
// ⚠️ This was a lambda inside a fifteen-hundred-line function and therefore had
// no test, and what it got wrong was found by running three hosts against three
// targets. The assertions below are the ones that would have found it in a
// second: the canonical spellings mcpp itself uses contain neither `apple` nor
// `darwin`, and the answer must not depend on the fallback.

TEST(Distribution, FormatIsTakenFromTheTargetAndNotTheFallback) {
    // Every fallback, so that a target the vocabulary knows can never be
    // decided by the machine doing the building.
    for (auto fb : {dist::Format::Elf, dist::Format::MachO, dist::Format::Pe}) {
        EXPECT_EQ(dist::format_for("aarch64-macos", fb),      dist::Format::MachO);
        EXPECT_EQ(dist::format_for("x86_64-macos", fb),       dist::Format::MachO);
        EXPECT_EQ(dist::format_for("x86_64-windows-gnu", fb), dist::Format::Pe);
        EXPECT_EQ(dist::format_for("x86_64-linux-gnu", fb),   dist::Format::Elf);
        EXPECT_EQ(dist::format_for("aarch64-linux-musl", fb), dist::Format::Elf);
        EXPECT_EQ(dist::format_for("riscv64-none-elf", fb),   dist::Format::Elf);
    }
}

// The `[target.X]` escape hatch: a spelling the vocabulary cannot parse is all
// there is to go on, so LLVM's words are recognised there.
TEST(Distribution, FormatFallsBackToSpellingForAnUnparseableTriple) {
    EXPECT_EQ(dist::format_for("arm64-apple-macos14.0", dist::Format::Elf),
              dist::Format::MachO);
    EXPECT_EQ(dist::format_for("x86_64-w64-mingw32", dist::Format::Elf),
              dist::Format::Pe);
    EXPECT_EQ(dist::format_for("x86_64-unknown-darwin", dist::Format::Elf),
              dist::Format::MachO);
}

// ⚠️ And only then the host. A triple that says nothing at all is the one case
// where the machine doing the building is the best available answer.
TEST(Distribution, FormatUsesTheFallbackOnlyWhenTheTripleSaysNothing) {
    EXPECT_EQ(dist::format_for("", dist::Format::MachO),   dist::Format::MachO);
    EXPECT_EQ(dist::format_for("", dist::Format::Pe),      dist::Format::Pe);
    EXPECT_EQ(dist::format_for("nonsense", dist::Format::Elf), dist::Format::Elf);
}
