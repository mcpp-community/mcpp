// HarmonyOS / OpenHarmony target identity + driver-retargeting model.
//
// Everything here is host-independent by construction: the triple language is
// pure data, and the link/driver models take a Toolchain by value. That is
// deliberate — the target these tests describe cannot be BUILT anywhere
// without a vendor SDK, so if the model were only testable where the SDK is
// installed it would effectively be untested. See
// .agents/docs/2026-08-04-harmonyos-target-design.md.

#include <gtest/gtest.h>

import std;
import mcpp.toolchain.triple;
import mcpp.toolchain.abi;
import mcpp.toolchain.model;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.hostflags;

using namespace mcpp::toolchain;
using mcpp::toolchain::triple::parse;

// ── triple language ─────────────────────────────────────────────────────────

TEST(OhosTriple, OhosIsAnEnvNotAnOs) {
    auto t = parse("aarch64-linux-ohos");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "aarch64");
    // The OS stays linux. A package that cfg-gates on `os = "linux"` (or on
    // `family = "unix"`) must keep matching on HarmonyOS — the kernel really
    // is Linux, and spelling ohos as an OS would silently exclude every such
    // block.
    EXPECT_EQ(t->os, "linux");
    EXPECT_EQ(t->env, "ohos");
    EXPECT_EQ(t->family(), "unix");
    EXPECT_EQ(t->str(), "aarch64-linux-ohos");
    EXPECT_TRUE(t->is_ohos());
}

TEST(OhosTriple, IsNotReportedAsMusl) {
    // OHOS libc IS a musl fork, and that is exactly why this assertion
    // exists: `is_musl()` drives payload selection and the `abi:musl`
    // capability, both of which mean UPSTREAM musl. An OHOS artifact is not
    // interchangeable with one.
    auto t = parse("aarch64-linux-ohos");
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->is_musl());
    EXPECT_FALSE(t->is_pe());
    EXPECT_FALSE(t->is_msvc_env());
}

TEST(OhosTriple, AcceptsTheSpellingsTheNdkAndLlvmUse) {
    // The SDK's own driver wrappers are named `aarch64-unknown-linux-ohos-*`,
    // and upstream LLVM's long environment name is OpenHOS. Both normalize.
    for (auto s : {"aarch64-unknown-linux-ohos", "aarch64-linux-openhos"}) {
        auto t = parse(s);
        ASSERT_TRUE(t.has_value()) << s;
        EXPECT_EQ(t->str(), "aarch64-linux-ohos") << s;
    }
    auto arm = parse("arm-linux-ohos");
    ASSERT_TRUE(arm.has_value());
    EXPECT_EQ(arm->str(), "arm-linux-ohos");
}

TEST(OhosTriple, ArtifactNamingIsPlainElf) {
    auto t = parse("aarch64-linux-ohos");
    ASSERT_TRUE(t.has_value());
    // Host naming deliberately set to the PE convention: if the answer were
    // taken from the host rather than the target, this would come back
    // `.exe`/`.lib` and the assertion would catch it (that is the B3 bug
    // class, 2026-08-03-b3-target-aware-artifact-naming.md).
    triple::ArtifactNaming pe{".exe", "", ".lib", ".dll", true};
    auto n = triple::artifact_naming(*t, pe);
    EXPECT_EQ(n.exeSuffix, "");
    EXPECT_EQ(n.libPrefix, "lib");
    EXPECT_EQ(n.staticLibExt, ".a");
    EXPECT_EQ(n.sharedLibExt, ".so");
    EXPECT_FALSE(n.sharedNeedsImportLib);
}

TEST(OhosTriple, KnownTargetPinsAnLlvmToolchain) {
    auto t = parse("aarch64-linux-ohos");
    ASSERT_TRUE(t.has_value());
    auto* info = triple::find_known_target(*t);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->tier, "verified");
    // Structural, not stylistic: GCC has no `ohos` target, so a gcc@ pin here
    // would be unbuildable by construction.
    EXPECT_EQ(info->pin, triple::pins::kOhosLlvm);
    EXPECT_TRUE(std::string_view(info->pin).starts_with("llvm@"));
    // musl-shaped libc + no system loader to rely on ⇒ static by default,
    // the same call the musl rows make.
    EXPECT_TRUE(info->defaultStatic);

    for (auto s : {"x86_64-linux-ohos", "arm-linux-ohos"}) {
        auto o = parse(s);
        ASSERT_TRUE(o.has_value()) << s;
        auto* oi = triple::find_known_target(*o);
        ASSERT_NE(oi, nullptr) << s;
        EXPECT_EQ(oi->pin, triple::pins::kOhosLlvm) << s;
    }
}

TEST(OhosTriple, FullStaticIsAllowedAndDoesNotConsultTheHost) {
    // hostCapability=false models a macOS/Windows host: a cross target's
    // linkage must not be decided by the build machine (the B2 bug class).
    EXPECT_TRUE(target_supports_full_static("aarch64-linux-ohos", false));
}

TEST(OhosTriple, LoaderKeepsMuslNaming) {
    // Measured from an SDK-produced binary's PT_INTERP: HarmonyOS kept musl's
    // loader file name even though its libc is a fork.
    EXPECT_EQ(loader_filename("aarch64-linux-ohos"), "ld-musl-aarch64.so.1");
    EXPECT_EQ(distro_loader_path("aarch64-linux-ohos"), "/lib/ld-musl-aarch64.so.1");
}

// ── ABI dimensions ──────────────────────────────────────────────────────────

TEST(OhosAbi, LibcIsItsOwnDimensionValue) {
    Toolchain tc;
    tc.compiler     = CompilerId::Clang;
    tc.targetTriple = "aarch64-linux-ohos";
    tc.stdlibId     = "libc++";
    auto p = abi_profile(tc);
    EXPECT_EQ(p.libc, "ohos");        // NOT "musl" — see IsNotReportedAsMusl
    EXPECT_EQ(p.os, "linux");
    EXPECT_EQ(p.arch, "aarch64");
    EXPECT_EQ(p.cxxAbi, "itanium");
    EXPECT_EQ(p.cxxStdlib, "libc++");

    // A package declaring plain `abi:musl` must NOT be considered compatible.
    auto c = parse_abi_capability("abi:musl", "some-pkg");
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(abi_check(p, {*c}).empty());
}

// ── driver retargeting ──────────────────────────────────────────────────────

namespace {

Toolchain cross_clang() {
    Toolchain tc;
    tc.compiler     = CompilerId::Clang;
    tc.version      = "20.1.7";
    tc.binaryPath   = "/xpkgs/llvm/20.1.7/bin/clang++";
    tc.targetTriple = "aarch64-linux-ohos";
    tc.stdlibId     = "libc++";
    // A host glibc payload deliberately left reachable: the point of several
    // assertions below is that the cross path must ignore it.
    tc.payloadPaths = PayloadPaths{"/xpkgs/glibc/include", "/xpkgs/glibc/lib64",
                                   "/xpkgs/linux-headers/include"};
    CrossTarget ct;
    ct.triple          = "aarch64-linux-ohos";
    ct.sysroot         = "/sdk/native/sysroot";
    ct.cxxIncludes     = {"/sdk/native/llvm/include/libcxx-ohos/include/c++/v1"};
    ct.libDirs         = {"/sdk/native/llvm/lib/aarch64-linux-ohos"};
    ct.linkResourceDir = "/sdk/native/llvm/lib/clang/15.0.4";
    ct.provider        = "OpenHarmony SDK 6.1.0.31 (API 23)";
    tc.crossTarget     = ct;
    return tc;
}

bool has(const std::vector<std::string>& v, std::string_view s) {
    return std::ranges::find(v, s) != v.end();
}

bool any_contains(const std::vector<std::string>& v, std::string_view needle) {
    return std::ranges::any_of(v, [&](auto const& t) {
        return t.find(needle) != std::string::npos; });
}

} // namespace

TEST(OhosCross, SysrootModeIgnoresTheHostPayload) {
    auto lm = resolve_link_model(cross_clang());
    EXPECT_EQ(lm.mode, CLibMode::Sysroot);
    EXPECT_EQ(lm.sysroot, std::filesystem::path("/sdk/native/sysroot"));
    // PayloadFirst would have put the host glibc's lib dir on -L and its
    // loader on --dynamic-linker. Aiming a cross link at the build machine's
    // libc is the failure this ordering exists to prevent.
    EXPECT_TRUE(lm.libDirs.empty());
    EXPECT_TRUE(lm.crtDir.empty());
    EXPECT_TRUE(lm.loader.empty());
}

TEST(OhosCross, TargetFlagIsFirstOnBothSides) {
    auto dm = resolve_clang_driver(cross_clang());
    ASSERT_TRUE(dm.isCross());
    auto esc = no_escape;

    auto c = dm.compile_tokens(esc);
    ASSERT_FALSE(c.empty());
    // First, because it selects the toolchain object everything later is
    // interpreted against.
    EXPECT_EQ(c.front(), "--target=aarch64-linux-ohos");

    auto l = dm.link_tokens(esc);
    ASSERT_FALSE(l.empty());
    EXPECT_EQ(l.front(), "--target=aarch64-linux-ohos");
}

TEST(OhosCross, CompileSideTakesTargetLibcxxAndNotTheHosts) {
    auto dm = resolve_clang_driver(cross_clang());
    auto c = dm.compile_tokens(no_escape);
    EXPECT_TRUE(has(c, "-nostdinc++"));
    EXPECT_TRUE(has(c, "-isystem/sdk/native/llvm/include/libcxx-ohos/include/c++/v1"));
    // The host llvm root is <bin>/../ = /xpkgs/llvm/20.1.7. Its libc++ headers
    // must never appear: they would compile without complaint and produce a
    // BMI configured for the wrong platform.
    EXPECT_FALSE(any_contains(c, "/xpkgs/llvm/20.1.7/include"));
}

TEST(OhosCross, ResourceDirIsLinkSideOnly) {
    auto dm = resolve_clang_driver(cross_clang());
    // On the link line: it is where the target's libclang_rt.builtins.a and
    // clang_rt.crt{begin,end}.o live.
    EXPECT_TRUE(has(dm.link_tokens(no_escape),
                    "-resource-dir=/sdk/native/llvm/lib/clang/15.0.4"));
    // NOT on the compile line: that dir belongs to clang 15 and carries clang
    // 15's intrinsic headers, which the actual (much newer) compiler must not
    // read.
    EXPECT_FALSE(any_contains(dm.compile_tokens(no_escape), "-resource-dir="));
}

TEST(OhosCross, NoRpathIntoBuildHostPaths) {
    auto l = resolve_clang_driver(cross_clang()).link_tokens(no_escape);
    EXPECT_TRUE(has(l, "-L/sdk/native/llvm/lib/aarch64-linux-ohos"));
    // The device will not have the build machine's directory layout, and a
    // static link has nowhere to use an rpath anyway.
    EXPECT_FALSE(any_contains(l, "-Wl,-rpath,"));
}

TEST(OhosCross, HostFlagProducerBypassesTheCfgEvenWithoutOne) {
    // The bundled-LLVM cfg is generated at install time for the HOST triple,
    // so a retargeted driver must bypass it. `cross_clang()`'s binary path
    // does not exist, hence hasCfg == false — and the tokens must still be
    // emitted, which is exactly the case a `dm.hasCfg &&` gate would drop.
    auto tc = cross_clang();
    HostFlagOptions opt;
    auto c = host_compile_tokens(tc, opt, no_escape);
    EXPECT_TRUE(has(c, "--target=aarch64-linux-ohos"));
    EXPECT_TRUE(has(c, "--no-default-config"));
    EXPECT_TRUE(has(c, "--sysroot=/sdk/native/sysroot"));

    auto l = host_link_tokens(tc, opt, no_escape);
    EXPECT_TRUE(has(l, "--target=aarch64-linux-ohos"));
    EXPECT_TRUE(has(l, "--sysroot=/sdk/native/sysroot"));
}

TEST(OhosCross, NoStdModuleUnlessTheProviderSuppliesOne) {
    // The default (stock SDK, libc++ 15) has no std module, and mcpp must say
    // so rather than reach for the driver's own — which is the HOST's, and
    // would compile into a wrong-platform BMI without erroring.
    auto tc = cross_clang();
    EXPECT_TRUE(tc.crossTarget->stdModuleSource.empty());
    EXPECT_FALSE(tc.hasImportStd);
}

TEST(OhosCross, HostToolchainIsUntouchedByTheseChanges) {
    // Regression guard: everything above keys on tc.crossTarget, so an
    // ordinary host toolchain must resolve exactly as before — payload-first,
    // no --target, no -resource-dir.
    Toolchain tc;
    tc.compiler     = CompilerId::Clang;
    tc.binaryPath   = "/xpkgs/llvm/20.1.7/bin/clang++";
    tc.targetTriple = "x86_64-linux-gnu";
    tc.payloadPaths = PayloadPaths{"/xpkgs/glibc/include", "/xpkgs/glibc/lib64",
                                   "/xpkgs/linux-headers/include"};
    auto dm = resolve_clang_driver(tc);
    EXPECT_FALSE(dm.isCross());
    EXPECT_TRUE(dm.target_tokens().empty());
    EXPECT_TRUE(dm.cross_link_tokens(no_escape).empty());

    auto lm = resolve_link_model(tc);
    EXPECT_EQ(lm.mode, CLibMode::PayloadFirst);
    EXPECT_EQ(lm.crtDir, std::filesystem::path("/xpkgs/glibc/lib64"));
}
