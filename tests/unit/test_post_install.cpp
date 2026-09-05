#include <gtest/gtest.h>
#include <fstream>

import std;
import mcpp.toolchain.post_install;
import mcpp.xlings;
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

// THE REGRESSION. An empty runtime identity used to be
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
// and the fixup would then never run again — including on the day the user
// moves to a SubOS that does describe itself and the identity becomes knowable.
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

// THE VERSION THAT WAS ASKED FOR AND THE VERSION THAT WAS INSTALLED ARE
// TWO VOCABULARIES FOR ONE FACT.
//
// A RuntimeBinding carries the DECLARED identity; xlings names the payload
// directory after what the request RESOLVED to. The two come apart the moment
// the index moves a package within a series — `xim:glibc@2.44` resolving to
// `2.44.2`.
//
// Measured 2026-08-27 on every CI machine with a cold cache, on `main` as
// readily as on any branch:
//
//     error: selected RuntimeBinding glibc@2.44 requires payload
//            '…/xim-x-glibc/2.44', but it is not installed
//     $ ls …/xim-x-glibc/   →   2.44.2
namespace {

// A payload is "installed" for this purpose when it has a lib dir with a
// loader in it — that is what select_glibc_payload_lib returns.
std::filesystem::path make_glibc_payload(const std::filesystem::path& root,
                                         std::string_view version) {
    auto lib = root / std::string(version) / "lib64";
    std::filesystem::create_directories(lib);
    std::ofstream(lib / "ld-linux-x86-64.so.2");
    return lib;
}

struct GlibcRootFixture {
    std::filesystem::path root;
    explicit GlibcRootFixture(std::string_view name)
        : root(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
    }
    ~GlibcRootFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

TEST(GlibcPayload, TheExactVersionIsPreferred) {
    GlibcRootFixture fx{"mcpp_glibc_exact"};
    auto want = make_glibc_payload(fx.root, "2.44");
    make_glibc_payload(fx.root, "2.44.2");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(*got, want);
}

TEST(GlibcPayload, ARequestResolvesToItsOneRefinement) {
    GlibcRootFixture fx{"mcpp_glibc_refine"};
    auto only = make_glibc_payload(fx.root, "2.44.2");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(*got, only);
}

// PER COMPONENT, NOT PER CHARACTER. `2.4` is not a request that `2.44`
// answers — a prefix match on the string would say it is, and would then hand
// a build the wrong C library without saying anything.
TEST(GlibcPayload, AStringPrefixIsNotARefinement) {
    GlibcRootFixture fx{"mcpp_glibc_strprefix"};
    make_glibc_payload(fx.root, "2.44");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.4");
    EXPECT_FALSE(got.has_value());
}

// AND THE REFUSAL STILL STANDS WHEN THERE IS NO ONE ANSWER. "The resolution
// of this request" has to be a single payload to be an answer at all; two
// refinements are not a menu to pick from.
TEST(GlibcPayload, TwoRefinementsAreRefusedRatherThanChosenBetween) {
    GlibcRootFixture fx{"mcpp_glibc_ambiguous"};
    make_glibc_payload(fx.root, "2.44.1");
    make_glibc_payload(fx.root, "2.44.2");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44");
    EXPECT_FALSE(got.has_value());
}

// THE RESOLVER ITSELF, because it has TWO callers and they failed
// separately. The first version of this fix lived inside the toolchain fixup;
// `probe`'s compile-side payload discovery spelled the same lookup its own way
// and kept missing — and ITS failure names no version at all:
//
//     bits/os_defines.h:39: fatal error: features.h: No such file
//
// because the glibc include directory is simply never added. Asserting on the
// shared function is what makes both call sites covered by one test.
TEST(PayloadDirForVersion, ExactWinsOverRefinement) {
    GlibcRootFixture fx{"mcpp_pdfv_exact"};
    std::filesystem::create_directories(fx.root / "2.44");
    std::filesystem::create_directories(fx.root / "2.44.2");
    auto got = mcpp::xlings::paths::payload_dir_for_version(fx.root, "2.44");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->filename(), "2.44");
}

TEST(PayloadDirForVersion, OneRefinementIsTheAnswer) {
    GlibcRootFixture fx{"mcpp_pdfv_one"};
    std::filesystem::create_directories(fx.root / "2.44.2");
    auto got = mcpp::xlings::paths::payload_dir_for_version(fx.root, "2.44");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->filename(), "2.44.2");
}

TEST(PayloadDirForVersion, TwoRefinementsAreNotAnAnswer) {
    GlibcRootFixture fx{"mcpp_pdfv_two"};
    std::filesystem::create_directories(fx.root / "2.44.1");
    std::filesystem::create_directories(fx.root / "2.44.2");
    EXPECT_FALSE(mcpp::xlings::paths::payload_dir_for_version(fx.root, "2.44"));
}

// PER COMPONENT, NOT PER CHARACTER — a string prefix would hand a build the
// wrong C library and say nothing.
TEST(PayloadDirForVersion, AStringPrefixIsNotARefinement) {
    GlibcRootFixture fx{"mcpp_pdfv_strprefix"};
    std::filesystem::create_directories(fx.root / "2.44");
    EXPECT_FALSE(mcpp::xlings::paths::payload_dir_for_version(fx.root, "2.4"));
}

TEST(GlibcPayload, NothingInstalledIsStillRefused) {
    GlibcRootFixture fx{"mcpp_glibc_empty"};
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44");
    EXPECT_FALSE(got.has_value());
}

// AN EMPTY REQUEST IS NOT A REQUEST FOR EVERYTHING.
//
// `packageRoot / ""` is `packageRoot`, and that IS a directory — so the
// exact-match branch would hand the CONTAINER back and every caller would
// treat it as a payload. Both callers reject an empty version before arriving,
// which is exactly why nothing would have caught it here.
TEST(PayloadDirForVersion, AnEmptyRequestIsNotTheContainer) {
    auto root = std::filesystem::temp_directory_path()
              / "mcpp_payload_empty_request";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "2.44.2");

    EXPECT_FALSE(mcpp::xlings::paths::payload_dir_for_version(root, ""));
    // The denominator: a real request against the same tree still resolves,
    // so an empty result above is the guard and not an unreadable directory.
    EXPECT_TRUE(mcpp::xlings::paths::payload_dir_for_version(root, "2.44"));

    std::filesystem::remove_all(root);
}
