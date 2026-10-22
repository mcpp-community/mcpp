#include <gtest/gtest.h>

import std;
import mcpp.toolchain.base;
import mcpp.toolchain.base_probe;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpp.platform;
import mcpp.platform.macos;

// The target base (design 2026-10-23, "target base and execution host"): the
// string grammar of `sysroot`, which spellings each row takes, the
// measurement tables, and what the link model renders for a stated base.
// Everything here runs on every host -- including the Apple locator, which
// reads `SDKROOT` on all of them.

namespace {

namespace tb = mcpp::toolchain::base;
namespace tc = mcpp::toolchain;
using mcpp::toolchain::triple::parse;

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_target_base_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void touch(const std::filesystem::path& p, std::string_view text = "x") {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string ident(const std::filesystem::path& p) { return p.string(); }

bool has(const std::vector<std::string>& v, std::string_view s) {
    return std::ranges::find(v, s) != v.end();
}

} // namespace

TEST(TargetBaseGrammar, EveryForm) {
    auto e = tb::parse_text("");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->form, tb::Form::Empty);

    auto p = tb::parse_text("path:/opt/acme//sysroot");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->form, tb::Form::Path);
    EXPECT_EQ(p->text, "path:/opt/acme/sysroot");

    auto k = tb::parse_text("acme:sysroot-rhel8@1.0");
    ASSERT_TRUE(k);
    EXPECT_EQ(k->form, tb::Form::Package);

    auto l = tb::parse_text("glibc@system");
    ASSERT_TRUE(l);
    EXPECT_EQ(l->form, tb::Form::Locator);
    EXPECT_EQ(l->family, "glibc");
    EXPECT_EQ(l->version, "system");

    // A versioned C library without a namespace is the ecosystem's package
    // (SPEC-006 §2.3); a versioned SDK is a locator with a version.
    auto g = tb::parse_text("glibc@2.39");
    ASSERT_TRUE(g);
    EXPECT_EQ(g->form, tb::Form::Package);
    EXPECT_EQ(g->text, "xim:glibc@2.39");
    auto i = tb::parse_text("iphoneos@18.5");
    ASSERT_TRUE(i);
    EXPECT_EQ(i->form, tb::Form::Locator);
    EXPECT_EQ(i->version, "18.5");
}

TEST(TargetBaseGrammar, RefusesWhatNamesNothing) {
    EXPECT_FALSE(tb::parse_text("sysroot"));
    EXPECT_FALSE(tb::parse_text("path:"));
    EXPECT_FALSE(tb::parse_text("bogus@1.0"));
    // An Apple SDK has no package; `@system` takes no namespace.
    EXPECT_FALSE(tb::parse_text("xim:iphoneos@18.5"));
    EXPECT_FALSE(tb::parse_text("xim:glibc@system"));
}

TEST(TargetBaseRows, EachRowTakesItsOwnForms) {
    auto linux_gnu = *parse("x86_64-linux-gnu");
    EXPECT_FALSE(tb::refusal(linux_gnu, *tb::parse_text("glibc@system")));
    EXPECT_FALSE(tb::refusal(linux_gnu, *tb::parse_text("path:/x")));
    EXPECT_FALSE(tb::refusal(linux_gnu, *tb::parse_text("xim:glibc@2.39")));
    // A family names a platform, so it must agree with the row.
    EXPECT_TRUE(tb::refusal(linux_gnu, *tb::parse_text("musl@system")));
    EXPECT_TRUE(tb::refusal(linux_gnu, *tb::parse_text("iphoneos@system")));

    auto sim = *parse("aarch64-ios-sim");
    EXPECT_FALSE(tb::refusal(sim, *tb::parse_text("iphonesimulator@system")));
    EXPECT_FALSE(tb::refusal(sim, *tb::parse_text("path:/sdk")));
    EXPECT_TRUE(tb::refusal(sim, *tb::parse_text("iphoneos@system")));
    EXPECT_TRUE(tb::refusal(sim, *tb::parse_text("")));

    auto msvc = *parse("x86_64-windows-msvc");
    EXPECT_FALSE(tb::refusal(msvc, *tb::parse_text("msvc@system")));
    EXPECT_FALSE(tb::refusal(msvc, *tb::parse_text("xim:msvc@14.44")));
    EXPECT_TRUE(tb::refusal(msvc, *tb::parse_text("acme:vs@1")));

    // The MSVC spelling on a MinGW row names another platform's toolset.
    EXPECT_TRUE(tb::refusal(*parse("x86_64-windows-gnu"), *tb::parse_text("msvc@14.44.35207")));

    // An SDK toolchain's row carries its own system: no new form.
    auto android = *parse("aarch64-linux-android");
    EXPECT_TRUE(tb::refusal(android, *tb::parse_text("path:/ndk")));
    EXPECT_EQ(tb::row_base(android).address, tb::Address::DriverInternal);

    EXPECT_EQ(tb::row_base(*parse("riscv64-none-elf")).address, tb::Address::Multilib);
    EXPECT_EQ(tb::row_base(*parse("aarch64-macos")).address, tb::Address::AppleIsysroot);
}

TEST(TargetBaseRows, PathSlashIsTheHostLocator) {
    auto s = tb::normalise(*parse("x86_64-linux-gnu"), *tb::parse_text("path:/"));
    EXPECT_EQ(s.form, tb::Form::Locator);
    EXPECT_EQ(s.text, "glibc@system");
    auto m = tb::normalise(*parse("x86_64-linux-musl"), *tb::parse_text("path:/"));
    EXPECT_EQ(m.text, "musl@system");
}

TEST(TargetBaseMeasure, ImplementationFromThePreprocessor) {
    auto glibc = tb::classify_macros(tb::parse_defines(
        "#define __GLIBC__ 2\n#define __GLIBC_MINOR__ 31\n#define FOO(x) x\n"));
    ASSERT_TRUE(glibc);
    EXPECT_EQ(glibc->impl, "glibc");
    EXPECT_EQ(glibc->version, "2.31");

    auto pico = tb::classify_macros(tb::parse_defines(
        "#define __PICOLIBC__ 1\n#define __PICOLIBC_VERSION__ \"1.8.12\"\n"));
    ASSERT_TRUE(pico);
    EXPECT_EQ(pico->version, "1.8.12");

    auto bionic = tb::classify_macros(tb::parse_defines(
        "#define __BIONIC__ 1\n#define __ANDROID_API__ 24\n"));
    ASSERT_TRUE(bionic);
    EXPECT_EQ(bionic->factName, "android.api");

    EXPECT_FALSE(tb::classify_macros(tb::parse_defines("#define __linux__ 1\n")));
}

TEST(TargetBaseMeasure, VersionsCompareNumerically) {
    EXPECT_LT(tb::compare_versions("2.9", "2.31"), 0);
    EXPECT_GT(tb::compare_versions("2.38", "2.31"), 0);
    EXPECT_EQ(tb::compare_versions("2.31", "2.31.0"), 0);
    EXPECT_EQ(tb::highest_glibc({"GLIBC_2.2.5", "GLIBC_2.38", "GLIBC_PRIVATE", "GLIBC_2.34"}),
              "2.38");
}

TEST(TargetBaseMeasure, MultiarchTuplesAreDebiansTable) {
    EXPECT_EQ(tb::multiarch_tuple(*parse("x86_64-linux-gnu")), "x86_64-linux-gnu");
    EXPECT_EQ(tb::multiarch_tuple(*parse("aarch64-linux-gnu")), "aarch64-linux-gnu");
    EXPECT_EQ(tb::multiarch_tuple(*parse("x86_64-linux-musl")), "x86_64-linux-musl");
}

TEST(TargetBaseMeasure, PayloadLayoutIsNotASysroot) {
    Tmp t;
    touch(t.path / "pkg" / "include" / "features.h");
    touch(t.path / "pkg" / "lib64" / "libc.so.6");
    EXPECT_TRUE(tb::is_payload_layout(t.path / "pkg"));
    touch(t.path / "root" / "usr" / "include" / "features.h");
    touch(t.path / "root" / "lib64" / "libc.so.6");
    EXPECT_FALSE(tb::is_payload_layout(t.path / "root"));
}

// The identity moves when a file the measurement rested on is rewritten in
// place, and only then.
TEST(TargetBaseIdentity, FollowsTheEvidence) {
    Tmp t;
    const auto lib = t.path / "lib64" / "libc.so.6";
    touch(lib, "one");
    const auto a = tb::identity_of("path:/x", t.path, {lib}, "glibc 2.31");
    EXPECT_EQ(a, tb::identity_of("path:/x", t.path, {lib}, "glibc 2.31"));
    touch(lib, "a longer file");
    EXPECT_NE(a, tb::identity_of("path:/x", t.path, {lib}, "glibc 2.31"));
    EXPECT_NE(a, tb::identity_of("path:/y", t.path, {lib}, "glibc 2.31"));
}

// Apple's own convention, on every host: `SDKROOT` names the SDK, its
// `SDKSettings.json` its version, its `libSystem.tbd` its slices.
TEST(TargetBaseApple, SdkrootOnEveryHost) {
    Tmp t;
    const auto sdk = t.path / "iPhoneSimulator18.5.sdk";
    touch(sdk / "SDKSettings.json", R"({"CanonicalName": "iphonesimulator18.5", "Version": "18.5"})");
    touch(sdk / "usr" / "include" / "stdio.h");
    touch(sdk / "usr" / "lib" / "libSystem.tbd",
          "--- !tapi-tbd\ntbd-version: 4\ntargets: [ x86_64-ios-simulator, arm64-ios-simulator ]\n");
    mcpp::platform::env::ScopedEnv env("SDKROOT", sdk.string());
    auto found = tb::locate_apple(*tb::parse_text("iphonesimulator@system"), "iphonesimulator");
    ASSERT_TRUE(found) << found.error();
    EXPECT_EQ(found->root, sdk);
    EXPECT_EQ(found->version, "18.5");
    EXPECT_FALSE(tb::locate_apple(*tb::parse_text("iphonesimulator@17.0"), "iphonesimulator"));
    // SDKROOT names one SDK and does not answer for another.
    EXPECT_FALSE(tb::locate_apple(*tb::parse_text("iphoneos@system"), "iphoneos"));
    EXPECT_EQ(mcpp::platform::macos::sdk_has_target(sdk, "x86_64-ios-simulator"), true);
    EXPECT_EQ(mcpp::platform::macos::sdk_has_target(sdk, "x86_64-ios"), false);
}

TEST(TargetBaseApple, PathMustBeAnSdk) {
    Tmp t;
    auto none = tb::locate_apple(*tb::parse_text("path:" + (t.path / "nothing").string()), "macosx");
    EXPECT_FALSE(none);
    touch(t.path / "empty" / "readme");
    EXPECT_FALSE(tb::locate_apple(*tb::parse_text("path:" + (t.path / "empty").string()), "macosx"));
}

// THE LINKER IS GIVEN THE SYSROOT, AND A BINDING ARTIFACT LOADS THE BASE: the
// two defects #755's sysroot had (scenarios C and D).
TEST(TargetBaseLinkModel, StatedBindingBaseNamesItsLoaderAndDirs) {
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.targetTriple = "x86_64-linux-gnu";
    t.binaryPath = "/tc/bin/g++";
    t.base.form = tb::Form::Path;
    t.base.address = tb::Address::GnuSysroot;
    t.base.runtime = tb::Runtime::Binding;
    t.base.root = "/sr";
    t.base.loader = "/sr/lib64/ld-linux-x86-64.so.2";
    t.base.runtimeDirs = {"/sr/lib64"};
    auto lm = tc::resolve_link_model(t);
    EXPECT_EQ(lm.mode, tc::CLibMode::Sysroot);
    auto link = lm.link_tokens(ident);
    EXPECT_TRUE(has(link, "--sysroot=/sr"));
    EXPECT_TRUE(has(link, "-Wl,--dynamic-linker=/sr/lib64/ld-linux-x86-64.so.2"));
    EXPECT_TRUE(has(link, "-Wl,-rpath,/sr/lib64"));

    // A floor (a cross target) names no loader: the pristine spec's standard
    // path is the one the target machine has.
    t.base.runtime = tb::Runtime::Floor;
    auto floor = tc::resolve_link_model(t).link_tokens(ident);
    EXPECT_FALSE(std::ranges::any_of(floor, [](auto const& s) {
        return s.starts_with("-Wl,--dynamic-linker"); }));
}

TEST(TargetBaseLinkModel, MultiarchForADriverThatDoesNotKnowIt) {
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.targetTriple = "x86_64-linux-gnu";
    t.binaryPath = "/tc/bin/g++";
    t.base.form = tb::Form::Locator;
    t.base.address = tb::Address::GnuSysroot;
    t.base.runtime = tb::Runtime::Binding;
    t.base.root = "/";
    t.base.multiarch = true;
    t.base.multiarchTuple = "x86_64-linux-gnu";
    auto lm = tc::resolve_link_model(t);
    EXPECT_TRUE(has(lm.compile_tokens(ident), "-idirafter/usr/include/x86_64-linux-gnu"));
    auto link = lm.link_tokens(ident);
    EXPECT_TRUE(has(link, "-B/usr/lib/x86_64-linux-gnu"));
    EXPECT_TRUE(has(link, "-Wl,-rpath-link,/lib/x86_64-linux-gnu"));
}

TEST(PasteArg, QuotesOnlyWhatTheShellWouldSplit) {
    EXPECT_EQ(mcpp::platform::shell::paste_arg("/tmp/home"), "/tmp/home");
    EXPECT_NE(mcpp::platform::shell::paste_arg("/tmp/mc probe/home"), "/tmp/mc probe/home");
    EXPECT_NE(mcpp::platform::shell::paste_arg("/tmp/a'b"), "/tmp/a'b");
}
