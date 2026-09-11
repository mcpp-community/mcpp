#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.platform.macos;

namespace mac = mcpp::platform::macos;

// ─── The three Apple SDKs are a table, not three code paths (C1) ───────────
//
// `sdk_path()` located exactly one SDK, because macOS was the only Apple
// target. The iOS rows need two more, and the difference between them is
// DIRECTORY NAMES -- so the change is a parameter over a table rather than a
// second locator. These two functions are the whole of what can be wrong in
// it, and they are pure, which is why a Linux runner can check them: the
// probing itself needs the machine.

TEST(AppleSdk, EveryRequestedSdkNamesItsOwnDirectories) {
    struct Case { std::string_view sdk, platformDir, sdkDir; bool clt; };
    const Case cases[] = {
        { mac::sdk_macos,     "MacOSX.platform",          "MacOSX.sdk",          true  },
        { mac::sdk_iphoneos,  "iPhoneOS.platform",        "iPhoneOS.sdk",        false },
        { mac::sdk_iphonesim, "iPhoneSimulator.platform", "iPhoneSimulator.sdk", false },
    };
    for (auto const& c : cases) {
        auto l = mac::sdk_layout(c.sdk);
        ASSERT_TRUE(l.has_value()) << c.sdk;
        EXPECT_EQ(l->platformDir, c.platformDir) << c.sdk;
        EXPECT_EQ(l->sdkDir, c.sdkDir) << c.sdk;
        // A CLT-ONLY INSTALL SHIPS NO iOS SDK. Probing its SDKs directory for
        // one would be a path that cannot exist, and the refusal that names
        // the SDK is the better answer -- so the table says so.
        EXPECT_EQ(l->inCommandLineTools, c.clt) << c.sdk;
    }
    // A name no Apple SDK answers to resolves to nothing, and `sdk_path`
    // returns nullopt before it runs a single command.
    for (auto bogus : {"", "ios", "iphone", "macos", "watchos"}) {
        EXPECT_FALSE(mac::sdk_layout(bogus).has_value()) << bogus;
        EXPECT_FALSE(mac::sdk_path(bogus).has_value()) << bogus;
    }
}

// SDKROOT NAMES ONE SDK AND THERE ARE THREE QUESTIONS. Honouring the override
// for every request would answer `sdk_path("iphoneos")` with a macOS SDK on
// any machine whose shell has SDKROOT set -- a wrong sysroot, which surfaces
// much later as missing headers rather than as a bad override. So the override
// is matched against what the path IS, and this is that comparison.
TEST(AppleSdk, AnOverrideAnswersOnlyForTheSdkItIs) {
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
        EXPECT_EQ(mac::sdk_name_of_root(std::filesystem::path(c.path)), c.name)
            << c.path;

    // AND THE TWO iOS NAMES DO NOT COLLAPSE INTO EACH OTHER. Both begin
    // `iPhone`, and a prefix test would make the simulator SDK satisfy a
    // device request -- an SDK that compiles and produces an artefact for the
    // wrong platform, which no later step refuses.
    EXPECT_NE(mac::sdk_name_of_root("/x/iPhoneSimulator18.4.sdk"),
              mac::sdk_name_of_root("/x/iPhoneOS18.4.sdk"));

    // AND `.sdk` IS REQUIRED. This is positive identification, and the suffix
    // is the marker Apple's own naming gives an SDK; `iPhoneOS.platform` is
    // the directory that CONTAINS one, and a bare `MacOSX` names nothing.
    for (auto notAnSdk : {"/x/Developer", "/x/MacOSX", "/x/iPhoneOS",
                          "/x/iPhoneOS.platform", "/x/.sdk",
                          "/x/Frameworks.sdk"})
        EXPECT_TRUE(mac::sdk_name_of_root(notAnSdk).empty()) << notAnSdk;
}

// The override arm itself, which on macOS sits behind a filesystem probe and
// is therefore reachable by no other check on a Linux runner.
TEST(AppleSdk, AnUnnamedOverrideAnswersTheDefaultQuestionOnly) {
    // Positive identification: an SDK answers for itself and for nothing else.
    EXPECT_TRUE (mac::sdkroot_answers("/x/iPhoneOS18.4.sdk", mac::sdk_iphoneos));
    EXPECT_FALSE(mac::sdkroot_answers("/x/iPhoneOS18.4.sdk", mac::sdk_iphonesim));
    EXPECT_FALSE(mac::sdkroot_answers("/x/iPhoneOS18.4.sdk", mac::sdk_macos));
    EXPECT_TRUE (mac::sdkroot_answers("/x/MacOSX15.4.sdk", mac::sdk_macos));
    EXPECT_FALSE(mac::sdkroot_answers("/x/MacOSX15.4.sdk", mac::sdk_iphoneos));

    // NO FLAG DAY. `SDKROOT=/opt/my-sysroot` is a spelling clang accepts and
    // a spelling `sdk_path()` honoured before it took a parameter, so it
    // still answers the default question -- and still does not answer an iOS
    // one, because a hand-rolled macOS sysroot is not an iOS SDK.
    for (auto unnamed : {"/opt/my-sysroot", "/x/Developer", "/x/MacOSX"}) {
        EXPECT_TRUE (mac::sdkroot_answers(unnamed, mac::sdk_macos))  << unnamed;
        EXPECT_FALSE(mac::sdkroot_answers(unnamed, mac::sdk_iphoneos)) << unnamed;
        EXPECT_FALSE(mac::sdkroot_answers(unnamed, mac::sdk_iphonesim)) << unnamed;
    }

    // A name no Apple SDK answers to is not a question, so no override
    // answers it.
    EXPECT_FALSE(mac::sdkroot_answers("/x/MacOSX15.4.sdk", "watchos"));
}

// NO FLAG DAY FOR THE THREE CALLERS. The parameter has a default, and the
// default request behaves exactly as the unparameterised function did: on a
// non-Apple host every form returns nullopt, and on macOS the generic
// `xcrun --show-sdk-path` probe stays first for it and is not used for any
// other SDK, because it returns the ACTIVE default rather than the one asked
// for.
TEST(AppleSdk, TheDefaultRequestIsTheMacosSdk) {
    EXPECT_EQ(mac::sdk_path(), mac::sdk_path(mac::sdk_macos));
    if constexpr (!mcpp::platform::is_macos) {
        EXPECT_FALSE(mac::sdk_path().has_value())
            << "an Apple SDK on a host that has none is a located path that "
               "does not exist";
        EXPECT_FALSE(mac::sdk_path(mac::sdk_iphoneos).has_value());
    }
}
