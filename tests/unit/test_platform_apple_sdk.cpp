#include <cstdlib>

import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpplibs.os.platform;
import mcpp.toolchain.apple_sdk;

using namespace mcpplibs::mtest;

namespace mac = mcpp::toolchain::apple;

namespace {

// Save/restore MACOSX_DEPLOYMENT_TARGET around a test so its own opinion
// cannot leak into a sibling test in this binary (gtest runs them in one
// process) and so a value set in the CI environment cannot leak into this
// one.
class ScopedDeploymentTargetEnv {
public:
    explicit ScopedDeploymentTargetEnv(const char* value)
        : inner_("MACOSX_DEPLOYMENT_TARGET", value ? std::optional<std::string>(value) : std::nullopt) {}
private:
    mcpp::invocation::ScopedEnv inner_;   // stated for this invocation, as mcpp states its own
};

}  // namespace

// ─── The three Apple SDKs are a table, not three code paths (C1) ───────────
//
// `sdk_path()` located exactly one SDK, because macOS was the only Apple
// target. The iOS rows need two more, and the difference between them is
// DIRECTORY NAMES -- so the change is a parameter over a table rather than a
// second locator. These two functions are the whole of what can be wrong in
// it, and they are pure, which is why a Linux runner can check them: the
// probing itself needs the machine.

UTest every_requested_sdk_names_its_own_directories{"AppleSdk/EveryRequestedSdkNamesItsOwnDirectories", [] {
    struct Case { std::string_view sdk, platformDir, sdkDir; bool clt; };
    const Case cases[] = {
        { mac::sdk_macos,     "MacOSX.platform",          "MacOSX.sdk",          true  },
        { mac::sdk_iphoneos,  "iPhoneOS.platform",        "iPhoneOS.sdk",        false },
        { mac::sdk_iphonesim, "iPhoneSimulator.platform", "iPhoneSimulator.sdk", false },
    };
    for (auto const& c : cases) {
        auto l = mac::sdk_layout(c.sdk);
        expect(fatal(l.has_value())) << c.sdk;
        expect(eq(l->platformDir, c.platformDir)) << c.sdk;
        expect(eq(l->sdkDir, c.sdkDir)) << c.sdk;
        // A CLT-ONLY INSTALL SHIPS NO iOS SDK. Probing its SDKs directory for
        // one would be a path that cannot exist, and the refusal that names
        // the SDK is the better answer -- so the table says so.
        expect(eq(l->inCommandLineTools, c.clt)) << c.sdk;
    }
    // A name no Apple SDK answers to resolves to nothing, and `sdk_path`
    // returns nullopt before it runs a single command.
    for (auto bogus : {"", "ios", "iphone", "macos", "watchos"}) {
        expect(!(mac::sdk_layout(bogus).has_value())) << bogus;
        expect(!(mac::sdk_path(bogus).has_value())) << bogus;
    }
}};

// SDKROOT NAMES ONE SDK AND THERE ARE THREE QUESTIONS. Honouring the override
// for every request would answer `sdk_path("iphoneos")` with a macOS SDK on
// any machine whose shell has SDKROOT set -- a wrong sysroot, which surfaces
// much later as missing headers rather than as a bad override. So the override
// is matched against what the path IS, and this is that comparison.
UTest an_override_answers_only_for_the_sdk_it_is{"AppleSdk/AnOverrideAnswersOnlyForTheSdkItIs", [] {
    struct Case { std::string_view path, name; };
    const Case cases[] = {
        { "/x/MacOSX.sdk",                     mac::sdk_macos     },
        { "/x/MacOSX15.4.sdk",                 mac::sdk_macos     },
        { "/x/macosx.sdk",                     mac::sdk_macos     },
        { "/x/iPhoneOS.sdk",                   mac::sdk_iphoneos  },
        { "/x/iPhoneOS18.4.sdk",               mac::sdk_iphoneos  },
        { "/x/iPhoneSimulator18.4.sdk",        mac::sdk_iphonesim },
    };
    for (auto const& c : cases)
        expect(eq(mac::sdk_name_of_root(std::filesystem::path(c.path)), c.name))
            << c.path;

    // AND THE TWO iOS NAMES DO NOT COLLAPSE INTO EACH OTHER. Both begin
    // `iPhone`, and a prefix test would make the simulator SDK satisfy a
    // device request -- an SDK that compiles and produces an artefact for the
    // wrong platform, which no later step refuses.
    expect(ne(mac::sdk_name_of_root("/x/iPhoneSimulator18.4.sdk"), mac::sdk_name_of_root("/x/iPhoneOS18.4.sdk")));

    // AND `.sdk` IS REQUIRED. This is positive identification, and the suffix
    // is the marker Apple's own naming gives an SDK; `iPhoneOS.platform` is
    // the directory that CONTAINS one, and a bare `MacOSX` names nothing.
    for (auto notAnSdk : {"/x/Developer", "/x/MacOSX", "/x/iPhoneOS",
                          "/x/iPhoneOS.platform", "/x/.sdk",
                          "/x/Frameworks.sdk"})
        expect(mac::sdk_name_of_root(notAnSdk).empty()) << notAnSdk;
}};

// The override arm itself, which on macOS sits behind a filesystem probe and
// is therefore reachable by no other check on a Linux runner.
UTest an_unnamed_override_answers_the_default_question_only{"AppleSdk/AnUnnamedOverrideAnswersTheDefaultQuestionOnly", [] {
    // Positive identification: an SDK answers for itself and for nothing else.
    expect(bool(mac::sdkroot_answers("/x/iPhoneOS18.4.sdk", mac::sdk_iphoneos)));
    expect(!mac::sdkroot_answers("/x/iPhoneOS18.4.sdk", mac::sdk_iphonesim));
    expect(!mac::sdkroot_answers("/x/iPhoneOS18.4.sdk", mac::sdk_macos));
    expect(bool(mac::sdkroot_answers("/x/MacOSX15.4.sdk", mac::sdk_macos)));
    expect(!mac::sdkroot_answers("/x/MacOSX15.4.sdk", mac::sdk_iphoneos));

    // NO FLAG DAY. `SDKROOT=/opt/my-sysroot` is a spelling clang accepts and
    // a spelling `sdk_path()` honoured before it took a parameter, so it
    // still answers the default question -- and still does not answer an iOS
    // one, because a hand-rolled macOS sysroot is not an iOS SDK.
    for (auto unnamed : {"/opt/my-sysroot", "/x/Developer", "/x/MacOSX"}) {
        expect(bool(mac::sdkroot_answers(unnamed, mac::sdk_macos)))  << unnamed;
        expect(!mac::sdkroot_answers(unnamed, mac::sdk_iphoneos)) << unnamed;
        expect(!mac::sdkroot_answers(unnamed, mac::sdk_iphonesim)) << unnamed;
    }

    // A name no Apple SDK answers to is not a question, so no override
    // answers it.
    expect(!mac::sdkroot_answers("/x/MacOSX15.4.sdk", "watchos"));
}};

// NO FLAG DAY FOR THE THREE CALLERS. The parameter has a default, and the
// default request behaves exactly as the unparameterised function did: on a
// non-Apple host every form returns nullopt, and on macOS the generic
// `xcrun --show-sdk-path` probe stays first for it and is not used for any
// other SDK, because it returns the ACTIVE default rather than the one asked
// for.
UTest the_default_request_is_the_macos_sdk{"AppleSdk/TheDefaultRequestIsTheMacosSdk", [] {
    expect(eq(mac::sdk_path(), mac::sdk_path(mac::sdk_macos)));
    if constexpr (!mcpplibs::os::is_macos) {
        expect(!(mac::sdk_path().has_value()))
            << "an Apple SDK on a host that has none is a located path that "
               "does not exist";
        expect(!(mac::sdk_path(mac::sdk_iphoneos).has_value()));
    }
}};

// ─── deployment_target: a TARGET decision, not a HOST one (mcpp#685) ────────
//
// `deployment_target` used to read `#if defined(__APPLE__)` and answer empty
// on every non-Apple host, no matter what `targetIsMacos` would have said —
// so `mcpp build --target aarch64-macos` from Linux ignored
// `macos_deployment_target` entirely. These run on whatever host is building
// mcpp itself (this repository's CI includes non-Apple runners), which is
// exactly the property under test: nothing here branches on
// `mcpplibs::os::is_macos`, and a reintroduced `#if defined(__APPLE__)`
// guard would make `TargetIsMacosEnvBeatsManifestBeatsDefault` fail on any
// non-Apple host.

UTest deployment_target_is_empty_for_a_non_macos_target{"AppleSdk/DeploymentTargetIsEmptyForANonMacosTarget", [] {
    // No env, no manifest value, and no env with a manifest value: a
    // non-macOS target gets no floor regardless of what either input says,
    // because applying one would be an answer to a question this target
    // never asked (see min_platform_version's Apple-only branch).
    ScopedDeploymentTargetEnv noEnv(nullptr);
    expect(eq(mac::deployment_target(false, ""), ""));
    expect(eq(mac::deployment_target(false, "11.0"), ""));
    ScopedDeploymentTargetEnv env("12.3");
    expect(eq(mac::deployment_target(false, "11.0"), ""));
}};

UTest target_is_macos_env_beats_manifest_beats_default{"AppleSdk/TargetIsMacosEnvBeatsManifestBeatsDefault", [] {
    {
        // Neither input set: the built-in floor.
        ScopedDeploymentTargetEnv noEnv(nullptr);
        expect(eq(mac::deployment_target(true, ""), mac::default_deployment_target));
    }
    {
        // Manifest only: the project's stated default.
        ScopedDeploymentTargetEnv noEnv(nullptr);
        expect(eq(mac::deployment_target(true, "11.0"), "11.0"));
    }
    {
        // Env only: the per-invocation override, same convention cargo/cc use.
        ScopedDeploymentTargetEnv env("12.3");
        expect(eq(mac::deployment_target(true, ""), "12.3"));
    }
    {
        // Both set: env wins.
        ScopedDeploymentTargetEnv env("12.3");
        expect(eq(mac::deployment_target(true, "11.0"), "12.3"));
    }
    {
        // An env var present but empty is the same as absent (the convention
        // every other MACOSX_DEPLOYMENT_TARGET reader in this tree follows).
        ScopedDeploymentTargetEnv env("");
        expect(eq(mac::deployment_target(true, "11.0"), "11.0"));
    }
}};
