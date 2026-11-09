
import std;
import mcpplibs.mtest;
import mcpp.toolchain.base;
import mcpp.toolchain.base_probe;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpplibs.os.platform;
import mcpp.toolchain.apple_sdk;
import mcpp.invocation;
import mcpp.build.command_line;

using namespace mcpplibs::mtest;

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

UTest every_form{"TargetBaseGrammar/EveryForm", [] {
    auto e = tb::parse_text("");
    expect(fatal(bool(e)));
    expect(eq(e->form, tb::Form::Empty));

    auto p = tb::parse_text("path:/opt/acme//sysroot");
    expect(fatal(bool(p)));
    expect(eq(p->form, tb::Form::Path));
    expect(eq(p->text, "path:/opt/acme/sysroot"));

    auto k = tb::parse_text("acme:sysroot-rhel8@1.0");
    expect(fatal(bool(k)));
    expect(eq(k->form, tb::Form::Package));

    auto l = tb::parse_text("glibc@system");
    expect(fatal(bool(l)));
    expect(eq(l->form, tb::Form::Locator));
    expect(eq(l->family, "glibc"));
    expect(eq(l->version, "system"));

    // A versioned C library without a namespace is the ecosystem's package
    // (SPEC-006 §2.3); a versioned SDK is a locator with a version.
    auto g = tb::parse_text("glibc@2.39");
    expect(fatal(bool(g)));
    expect(eq(g->form, tb::Form::Package));
    expect(eq(g->text, "xim:glibc@2.39"));
    auto i = tb::parse_text("iphoneos@18.5");
    expect(fatal(bool(i)));
    expect(eq(i->form, tb::Form::Locator));
    expect(eq(i->version, "18.5"));
}};

UTest refuses_what_names_nothing{"TargetBaseGrammar/RefusesWhatNamesNothing", [] {
    expect(!tb::parse_text("sysroot"));
    expect(!tb::parse_text("path:"));
    expect(!tb::parse_text("bogus@1.0"));
    // An Apple SDK has no package; `@system` takes no namespace.
    expect(!tb::parse_text("xim:iphoneos@18.5"));
    expect(!tb::parse_text("xim:glibc@system"));
}};

UTest each_row_takes_its_own_forms{"TargetBaseRows/EachRowTakesItsOwnForms", [] {
    auto linux_gnu = *parse("x86_64-linux-gnu");
    expect(!(tb::refusal(linux_gnu, *tb::parse_text("glibc@system"))));
    expect(!(tb::refusal(linux_gnu, *tb::parse_text("path:/x"))));
    expect(!(tb::refusal(linux_gnu, *tb::parse_text("xim:glibc@2.39"))));
    // A family names a platform, so it must agree with the row.
    expect(bool(tb::refusal(linux_gnu, *tb::parse_text("musl@system"))));
    expect(bool(tb::refusal(linux_gnu, *tb::parse_text("iphoneos@system"))));

    auto sim = *parse("aarch64-ios-sim");
    expect(!(tb::refusal(sim, *tb::parse_text("iphonesimulator@system"))));
    expect(!(tb::refusal(sim, *tb::parse_text("path:/sdk"))));
    expect(bool(tb::refusal(sim, *tb::parse_text("iphoneos@system"))));
    // The spellings the row took before it took a locator keep their meaning.
    expect(!(tb::refusal(sim, *tb::parse_text(""))));

    auto msvc = *parse("x86_64-windows-msvc");
    expect(!(tb::refusal(msvc, *tb::parse_text("msvc@system"))));
    expect(!(tb::refusal(msvc, *tb::parse_text("xim:msvc@14.44"))));
    expect(bool(tb::refusal(msvc, *tb::parse_text("acme:vs@1"))));

    // The MSVC spelling on a MinGW row names another platform's toolset.
    expect(bool(tb::refusal(*parse("x86_64-windows-gnu"), *tb::parse_text("msvc@14.44.35207"))));

    // An SDK toolchain's row carries its own system: no new form.
    auto android = *parse("aarch64-linux-android");
    expect(bool(tb::refusal(android, *tb::parse_text("path:/ndk"))));
    expect(eq(tb::row_base(android).address, tb::Address::DriverInternal));

    expect(eq(tb::row_base(*parse("riscv64-none-elf")).address, tb::Address::Multilib));
    expect(eq(tb::row_base(*parse("aarch64-macos")).address, tb::Address::AppleIsysroot));
}};

UTest path_slash_is_the_host_locator{"TargetBaseRows/PathSlashIsTheHostLocator", [] {
    auto s = tb::normalise(*parse("x86_64-linux-gnu"), *tb::parse_text("path:/"));
    expect(eq(s.form, tb::Form::Locator));
    expect(eq(s.text, "glibc@system"));
    auto m = tb::normalise(*parse("x86_64-linux-musl"), *tb::parse_text("path:/"));
    expect(eq(m.text, "musl@system"));
}};

UTest implementation_from_the_preprocessor{"TargetBaseMeasure/ImplementationFromThePreprocessor", [] {
    auto glibc = tb::classify_macros(tb::parse_defines(
        "#define __GLIBC__ 2\n#define __GLIBC_MINOR__ 31\n#define FOO(x) x\n"));
    expect(fatal(bool(glibc)));
    expect(eq(glibc->impl, "glibc"));
    expect(eq(glibc->version, "2.31"));

    auto pico = tb::classify_macros(tb::parse_defines(
        "#define __PICOLIBC__ 1\n#define __PICOLIBC_VERSION__ \"1.8.12\"\n"));
    expect(fatal(bool(pico)));
    expect(eq(pico->version, "1.8.12"));

    auto bionic = tb::classify_macros(tb::parse_defines(
        "#define __BIONIC__ 1\n#define __ANDROID_API__ 24\n"));
    expect(fatal(bool(bionic)));
    expect(eq(bionic->factName, "android.api"));

    expect(!(tb::classify_macros(tb::parse_defines("#define __linux__ 1\n"))));
}};

UTest versions_compare_numerically{"TargetBaseMeasure/VersionsCompareNumerically", [] {
    expect(lt(tb::compare_versions("2.9", "2.31"), 0));
    expect(gt(tb::compare_versions("2.38", "2.31"), 0));
    expect(eq(tb::compare_versions("2.31", "2.31.0"), 0));
    expect(eq(tb::highest_glibc({"GLIBC_2.2.5", "GLIBC_2.38", "GLIBC_PRIVATE", "GLIBC_2.34"}), "2.38"));
}};

UTest multiarch_tuples_are_debians_table{"TargetBaseMeasure/MultiarchTuplesAreDebiansTable", [] {
    expect(eq(tb::multiarch_tuple(*parse("x86_64-linux-gnu")), "x86_64-linux-gnu"));
    expect(eq(tb::multiarch_tuple(*parse("aarch64-linux-gnu")), "aarch64-linux-gnu"));
    expect(eq(tb::multiarch_tuple(*parse("x86_64-linux-musl")), "x86_64-linux-musl"));
}};

UTest payload_layout_is_not_a_sysroot{"TargetBaseMeasure/PayloadLayoutIsNotASysroot", [] {
    Tmp t;
    touch(t.path / "pkg" / "include" / "features.h");
    touch(t.path / "pkg" / "lib64" / "libc.so.6");
    expect(tb::is_payload_layout(t.path / "pkg"));
    touch(t.path / "root" / "usr" / "include" / "features.h");
    touch(t.path / "root" / "lib64" / "libc.so.6");
    expect(!tb::is_payload_layout(t.path / "root"));
}};

// The identity moves when a file the measurement rested on is rewritten in
// place, and only then.
UTest follows_the_evidence{"TargetBaseIdentity/FollowsTheEvidence", [] {
    Tmp t;
    const auto lib = t.path / "lib64" / "libc.so.6";
    touch(lib, "one");
    const auto a = tb::identity_of("path:/x", t.path, {lib}, "glibc 2.31");
    expect(eq(a, tb::identity_of("path:/x", t.path, {lib}, "glibc 2.31")));
    touch(lib, "a longer file");
    expect(ne(a, tb::identity_of("path:/x", t.path, {lib}, "glibc 2.31")));
    expect(ne(a, tb::identity_of("path:/y", t.path, {lib}, "glibc 2.31")));
}};

// Apple's own convention, on every host: `SDKROOT` names the SDK, its
// `SDKSettings.json` its version, its `libSystem.tbd` its slices.
UTest sdkroot_on_every_host{"TargetBaseApple/SdkrootOnEveryHost", [] {
    Tmp t;
    const auto sdk = t.path / "iPhoneSimulator18.5.sdk";
    touch(sdk / "SDKSettings.json", R"({"CanonicalName": "iphonesimulator18.5", "Version": "18.5"})");
    touch(sdk / "usr" / "include" / "stdio.h");
    touch(sdk / "usr" / "lib" / "libSystem.tbd",
          "--- !tapi-tbd\ntbd-version: 4\ntargets: [ x86_64-ios-simulator, arm64-ios-simulator ]\n");
    mcpp::invocation::ScopedEnv env("SDKROOT", sdk.string());
    auto found = tb::locate_apple(*tb::parse_text("iphonesimulator@system"), "iphonesimulator");
    if (auto&& failed = expect(fatal(bool(found))); !failed) failed << found.error();
    expect(eq(found->root, sdk));
    expect(eq(found->version, "18.5"));
    expect(!(tb::locate_apple(*tb::parse_text("iphonesimulator@17.0"), "iphonesimulator")));
    // SDKROOT names one SDK and does not answer for another. (On an Apple
    // host `xcrun` answers next, and Xcode's iPhoneOS SDK is found there.)
    if (!mcpplibs::os::is_macos)
        expect(!(tb::locate_apple(*tb::parse_text("iphoneos@system"), "iphoneos")));
    using mcpp::toolchain::apple::Slice;
    expect(eq(mcpp::toolchain::apple::sdk_has_target(sdk, "x86_64-ios-simulator"), Slice::carried));
    expect(eq(mcpp::toolchain::apple::sdk_has_target(sdk, "x86_64-ios"), Slice::absent));
}};

UTest path_must_be_an_sdk{"TargetBaseApple/PathMustBeAnSdk", [] {
    Tmp t;
    auto none = tb::locate_apple(*tb::parse_text("path:" + (t.path / "nothing").string()), "macosx");
    expect(!none);
    touch(t.path / "empty" / "readme");
    expect(!(tb::locate_apple(*tb::parse_text("path:" + (t.path / "empty").string()), "macosx")));
}};

// THE LINKER IS GIVEN THE SYSROOT, AND A BINDING ARTIFACT LOADS THE BASE: the
// two defects #755's sysroot had (scenarios C and D).
UTest stated_binding_base_names_its_loader_and_dirs{"TargetBaseLinkModel/StatedBindingBaseNamesItsLoaderAndDirs", [] {
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
    expect(eq(lm.mode, tc::CLibMode::Sysroot));
    auto link = lm.link_tokens(ident);
    expect(bool(has(link, "--sysroot=/sr")));
    expect(bool(has(link, "-Wl,--sysroot=/sr")));
    expect(bool(has(link, "-Wl,--dynamic-linker=/sr/lib64/ld-linux-x86-64.so.2")));
    expect(bool(has(link, "-Wl,-rpath,/sr/lib64")));

    // A floor (a cross target) names no loader: the pristine spec's standard
    // path is the one the target machine has.
    t.base.runtime = tb::Runtime::Floor;
    auto floor = tc::resolve_link_model(t).link_tokens(ident);
    expect(!(std::ranges::any_of(floor, [](auto const& s) {
        return s.starts_with("-Wl,--dynamic-linker"); })));
}};

UTest multiarch_for_a_driver_that_does_not_know_it{"TargetBaseLinkModel/MultiarchForADriverThatDoesNotKnowIt", [] {
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
    // Spelled as the host spells a path: the renderer joins with `/`
    // operators, so a Windows host writes its own separator.
    namespace fs = std::filesystem;
    const auto root = fs::path("/");
    expect(bool(has(lm.compile_tokens(ident),
                    "-idirafter" + (root / "usr" / "include" / "x86_64-linux-gnu").string())));
    auto link = lm.link_tokens(ident);
    expect(bool(has(link, "-B" + (root / "usr" / "lib" / "x86_64-linux-gnu").string())));
    expect(bool(has(link, "-Wl,-rpath-link," + (root / "lib" / "x86_64-linux-gnu").string())));
}};

UTest quotes_only_what_the_shell_would_split{"PasteArg/QuotesOnlyWhatTheShellWouldSplit", [] {
    expect(eq(mcpp::build::command_line::paste_arg("/tmp/home"), "/tmp/home"));
    expect(ne(mcpp::build::command_line::paste_arg("/tmp/mc probe/home"), "/tmp/mc probe/home"));
    expect(ne(mcpp::build::command_line::paste_arg("/tmp/a'b"), "/tmp/a'b"));
}};
