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
    EXPECT_EQ(dist::default_contract(dist::Role::Test),
              dist::Contract::SelfContained);
    EXPECT_EQ(dist::default_contract(dist::Role::Distributable),
              dist::Contract::SelfContained);

    auto in = macos_input();
    in.role = dist::Role::Test;
    in.requested = dist::default_contract(dist::Role::Test);
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

// MSVC's self-contained form would be the /MT runtime, which mcpp does not
// emit. Before the table this cell simply produced nothing and claimed
// success; now it names the gap.
TEST(Distribution, MsvcSelfContainedIsAnHonestGap) {
    dist::MechanismInput in;
    in.format          = dist::Format::Pe;
    in.stdlibId        = "msvc";
    in.explicitRequest = true;
    auto m = dist::resolve(in);
    EXPECT_EQ(m.effective, dist::Contract::HostCoupled);
    EXPECT_TRUE(m.degraded);
    EXPECT_NE(m.diagnostic.find("/MT"), std::string::npos);
    EXPECT_TRUE(m.unitFlags.empty());

    in.requested = dist::Contract::HostCoupled;
    EXPECT_FALSE(dist::resolve(in).degraded);

    // ...but the DEFAULT must be quiet. mcpp never promised a self-contained
    // MSVC artifact — it emits no /MT at all — so warning on every Windows
    // build would be unactionable noise. A diagnostic is for a broken
    // promise, not for a platform limit nobody asked about.
    in.requested       = dist::Contract::SelfContained;
    in.explicitRequest = false;
    auto quiet = dist::resolve(in);
    EXPECT_FALSE(quiet.degraded);
    EXPECT_TRUE(quiet.diagnostic.empty());
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
