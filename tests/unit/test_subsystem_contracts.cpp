#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

import std;
import mcpp.version;
import mcpp.build.program_protocol;
import mcpp.build.hostprogram;

// GLOBAL CROSS-CHECK.
//
// The packages under `modules/` carry their own tests, which see only the
// package and therefore state its contract in isolation. Those tests cannot
// fail when a CONSUMER stops agreeing with the contract — from inside the
// package there is no consumer.
//
// This file is the other half. It imports a subsystem and the engine code that
// consumes it, and asserts they still say the same thing. The pair gives two
// distinguishable failures:
//
//   the subsystem test goes red   the contract changed
//   this file goes red            the contract held and a consumer drifted
//   both go red                   the change was made in one place and
//                                 propagated, which is the normal case
//
// A fact worth pinning here is one that TWO layers have to agree on. A fact
// only one layer can see belongs in that layer's own tests.

namespace pp = mcpp::build::program_protocol;

TEST(SubsystemContracts, TheAnnouncedProtocolIsSubstitutedAndNotHardcoded) {
    // The bundled `mcpp` module announces `mcpp:protocol=N` so the engine can
    // reject a program written for a newer protocol. N is injected by
    // substituting `@PROTOCOL@` at compile time, PRECISELY so the announced
    // value cannot drift from the checked one.
    //
    // If someone spells the number instead, both this project's build and the
    // subsystem's own tests stay green while a bumped protocol silently keeps
    // announcing the old one.
    const std::string src{mcpp::build::kMcppModuleSource};
    EXPECT_NE(src.find("@PROTOCOL@"), std::string::npos)
        << "the bundled module no longer carries the substitution point";

    const std::string hardcoded =
        "mcpp:protocol=" + std::to_string(pp::kProtocolVersion);
    EXPECT_EQ(src.find(hardcoded), std::string::npos)
        << "the protocol number is spelled into the bundled module; "
           "substitute @PROTOCOL@ instead so the two cannot diverge";
}

TEST(SubsystemContracts, TheModuleDeclarationIsAlsoSubstituted) {
    // `export module mcpp;` inside a string literal would be read by mcpp's
    // own regex scanner as this file exporting a second module. The engine
    // writes `@MODULE@` and substitutes it when it emits the source.
    const std::string src{mcpp::build::kMcppModuleSource};
    EXPECT_NE(src.find("@MODULE@"), std::string::npos);
    EXPECT_EQ(src.find("export module mcpp;"), std::string::npos)
        << "a literal module declaration here makes the scanner believe this "
           "translation unit exports it";
}

TEST(SubsystemContracts, TheBinaryVersionMatchesTheRootManifest) {
    // Two files, one fact. `.github/tools/check_version_pins.sh` enforces this
    // as text before anything is built; this asserts it on the value the
    // COMPILED binary carries, which is what `--version`, the BMI fingerprint
    // and the index floor comparison all read.
    //
    // The version module moved into `modules/versioning/` and this test did
    // not have to move with it: the fact belongs to neither side alone.
    namespace fs = std::filesystem;
    fs::path root = fs::current_path();
    for (int i = 0; i < 6 && !fs::exists(root / "mcpp.toml"); ++i)
        root = root.parent_path();
    ASSERT_TRUE(fs::exists(root / "mcpp.toml"))
        << "could not find the root manifest from " << fs::current_path();

    std::ifstream in(root / "mcpp.toml");
    ASSERT_TRUE(in.is_open());
    std::string line, fromToml;
    while (std::getline(in, line)) {
        if (!line.starts_with("version")) continue;
        auto a = line.find('"');
        auto b = line.find('"', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        fromToml = line.substr(a + 1, b - a - 1);
        break;
    }
    ASSERT_FALSE(fromToml.empty()) << "no [package] version in the root manifest";
    EXPECT_EQ(fromToml, std::string(mcpp::MCPP_VERSION));
}
