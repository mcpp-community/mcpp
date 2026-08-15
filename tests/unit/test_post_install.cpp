#include <gtest/gtest.h>

import std;
import mcpp.toolchain.post_install;
import mcpp.config;
import mcpp.toolchain.registry;
import mcpp.platform;

// detect_baked_loader parses gcc SPECS GRAMMAR, not plain text. The baked
// loader path is embedded inside %-spec conditionals, e.g.
//   %{mmusl:/lib/ld-musl-x86_64.so.1;:/baked/dir/ld-linux-x86-64.so.2}
// A scanner that treats "whitespace or :;" as the boundary swallows the
// closing braces; replacing that string then corrupts the spec grammar and
// EVERY subsequent g++ invocation dies with "braced spec body ... is
// invalid" (observed on CI). These tests pin the exact grammar shape.

namespace {

using mcpp::toolchain::detect_baked_loader;

// Realistic *link_spec fragment as xim bakes it (64-bit branch rewritten to
// the installing user's home; other multilib branches pristine).
const std::string kBakedSpecs =
    "*link:\n"
    "%{m16|m32|mx32:;:-m elf_x86_64} %{shared:-shared} %{!shared: %{!static: "
    "%{m16|m32:-dynamic-linker %{muclibc:/lib/ld-uClibc.so.0;:%{mbionic:/system/bin/linker;:"
    "%{mmusl:/lib/ld-musl-i386.so.1;:/lib/ld-linux.so.2}}}} "
    "%{m16|m32|mx32:;:-dynamic-linker %{muclibc:/lib/ld64-uClibc.so.0;:%{mbionic:/system/bin/linker64;:"
    "%{mmusl:/lib/ld-musl-x86_64.so.1;:/opt/other-home/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2}}}}} "
    "%{static:-static}}\n";

TEST(DetectBakedLoader, ExtractsExactPathWithoutSpecBraces) {
    auto got = detect_baked_loader(kBakedSpecs);
    EXPECT_EQ(got,
        "/opt/other-home/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2");
    // The regression: any brace in the result corrupts the specs on rewrite.
    EXPECT_EQ(got.find('}'), std::string::npos);
    EXPECT_EQ(got.find('{'), std::string::npos);
}

TEST(DetectBakedLoader, IgnoresPristineMultilibDefaults) {
    // An unbaked spec (all-standard /lib*/ paths) must not be rewritten.
    const std::string pristine =
        "%{mmusl:/lib/ld-musl-x86_64.so.1;:/lib64/ld-linux-x86-64.so.2} "
        "%{m16|m32:-dynamic-linker /lib/ld-linux.so.2} "
        "%{mx32:-dynamic-linker /libx32/ld-linux-x32.so.2}";
    EXPECT_EQ(detect_baked_loader(pristine), "");
}

TEST(DetectBakedLoader, Aarch64LoaderNameDetected) {
    const std::string specs =
        "-dynamic-linker %{mmusl:/lib/ld-musl-aarch64.so.1;:"
        "/srv/build/.xlings/xpkgs/xim-x-glibc/2.39/lib/ld-linux-aarch64.so.1}";
    EXPECT_EQ(detect_baked_loader(specs),
        "/srv/build/.xlings/xpkgs/xim-x-glibc/2.39/lib/ld-linux-aarch64.so.1");
}

TEST(DetectBakedLoader, EmptyWhenNoGnuLoaderPresent) {
    EXPECT_EQ(detect_baked_loader("no loaders here"), "");
    EXPECT_EQ(detect_baked_loader("%{mmusl:/lib/ld-musl-x86_64.so.1}"), "");
}

}  // namespace

// ── the post-install fixup gate (mcpp#427) ───────────────────────────────
//
// A unit test rather than an e2e ON PURPOSE, and the reason is the defect
// itself. `ensure_post_install_fixup` returns early — before any of the logic
// under test — for a payload that resolves OUTSIDE the caller's registry
// ("inherited payload, owner is responsible for its fixup"). Every affordable
// e2e inherits its toolchain by symlink to avoid a multi-gigabyte download, so
// no e2e can reach this code. That is precisely the shape of mcpp#221: a test
// that creates the missing thing somewhere the code under test never looks,
// and passes.
//
// Here the payload is a real directory inside the test's own registry, so the
// containment guard passes and the gate actually runs.

namespace fixup_gate {

struct Sandbox {
    std::filesystem::path root;
    mcpp::config::GlobalConfig cfg;

    Sandbox() {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-fixup-{}", std::random_device{}());
        std::filesystem::create_directories(root / "registry" / "data" / "xpkgs");
        cfg.registryDir = root / "registry";
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // A payload physically inside the registry, so the #273 containment guard
    // treats it as ours to patch and the gate is actually reached.
    std::filesystem::path payload(std::string_view name) {
        auto p = root / "registry" / "data" / "xpkgs" / name / "1.0.0";
        std::filesystem::create_directories(p / "bin");
        return p;
    }
};

mcpp::toolchain::XimToolchainPackage gcc_pkg() {
    mcpp::toolchain::XimToolchainPackage pkg;
    pkg.ximName = "gcc";
    pkg.ximVersion = "16.1.0";
    pkg.needsGccPostInstallFixup = true;
    return pkg;
}

}  // namespace fixup_gate

// ⚠️ THE REGRESSION. An empty runtime identity used to be
//     std::unexpected("… default SubOS has no RuntimeBinding identity …")
// and `prepare.cppm` turned that into `error: toolchain post-install fixup: …`,
// so `mcpp build` AND `mcpp toolchain install` both died on any Linux machine
// whose default SubOS predated xlings' `subos_info` block. Absence is not a
// contradiction: it degrades.
TEST(PostInstallFixup, AnAbsentRuntimeIdentityDegradesInsteadOfFailing) {
    if constexpr (!mcpp::platform::is_linux) {
        GTEST_SKIP() << "the runtime binding gate is Linux-only";
    } else {
        fixup_gate::Sandbox sb;
        auto result = mcpp::toolchain::ensure_post_install_fixup(
            sb.cfg, sb.payload("xim-x-gcc"), fixup_gate::gcc_pkg(),
            /*runtimeId=*/"", /*selectedRuntimeLibDir=*/{});

        ASSERT_TRUE(result.has_value())
            << "an undescribed SubOS became a hard error again: "
            << result.error();
        EXPECT_FALSE(result->applied);
        // ...and it must SAY why. A degradation nobody can report is
        // indistinguishable from a fixup that silently stopped working.
        EXPECT_FALSE(result->skippedReason.empty())
            << "degraded silently — the caller has nothing to print";
    }
}

// The other direction, so relaxing absence cannot quietly relax everything. A
// runtime that is DECLARED but cannot be produced is wrong information, not
// missing information: guessing a different glibc would make one mcpp.toml mean
// different ABIs on different machines.
TEST(PostInstallFixup, ADeclaredRuntimeThatCannotBeHonouredStillFails) {
    if constexpr (!mcpp::platform::is_linux) {
        GTEST_SKIP() << "the runtime binding gate is Linux-only";
    } else {
        fixup_gate::Sandbox sb;
        auto result = mcpp::toolchain::ensure_post_install_fixup(
            sb.cfg, sb.payload("xim-x-gcc"), fixup_gate::gcc_pkg(),
            /*runtimeId=*/"glibc@0.0.0-does-not-exist",
            /*selectedRuntimeLibDir=*/{});

        EXPECT_FALSE(result.has_value())
            << "a runtime identity that names nothing installed was accepted";
    }
}

// No marker may be written for a fixup that did not happen. The marker is a
// content fingerprint whose whole job is to answer "were these inputs ever
// applied"; one written for "we did nothing" reads back as "already applied",
// and the fixup would then never run again — including on the day the user runs
// `xlings self update` and the identity finally becomes knowable.
TEST(PostInstallFixup, ASkippedFixupLeavesNoMarkerBehind) {
    if constexpr (!mcpp::platform::is_linux) {
        GTEST_SKIP() << "the runtime binding gate is Linux-only";
    } else {
        fixup_gate::Sandbox sb;
        auto payload = sb.payload("xim-x-gcc");
        auto result = mcpp::toolchain::ensure_post_install_fixup(
            sb.cfg, payload, fixup_gate::gcc_pkg(), /*runtimeId=*/"", {});
        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_FALSE(std::filesystem::exists(payload / ".mcpp-fixup.json"))
            << "'we did nothing' was recorded as 'already applied'";
    }
}

// A package that needs no fixup at all is not a degradation and must not report
// one, or the caller warns about every msvc/system install.
TEST(PostInstallFixup, APackageWithNoFixupReportsNothingToReport) {
    fixup_gate::Sandbox sb;
    mcpp::toolchain::XimToolchainPackage none;
    none.ximName = "msvc";
    none.ximVersion = "system";
    auto result = mcpp::toolchain::ensure_post_install_fixup(
        sb.cfg, sb.payload("xim-x-msvc"), none, "", {});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->skippedReason.empty())
        << "a toolchain with no fixup reported a degradation: "
        << result->skippedReason;
}
