#include <gtest/gtest.h>

import std;
import mcpp.pack;

using mcpp::pack::Mode;
using mcpp::pack::parse_mode;
using mcpp::pack::mode_cli_name;
using mcpp::pack::Format;
using mcpp::pack::ClosureUnavailableOutcome;
using mcpp::pack::closure_unavailable_outcome;

TEST(PackModes, CanonicalNamesParse) {
    EXPECT_EQ(parse_mode("system"),         Mode::None);
    EXPECT_EQ(parse_mode("vendored"),       Mode::BundleProject);
    EXPECT_EQ(parse_mode("self-contained"), Mode::BundleAll);
    EXPECT_EQ(parse_mode("static"),         Mode::Static);
}

TEST(PackModes, OldNamesStayAsAliases) {
    EXPECT_EQ(parse_mode("bundle-project"), Mode::BundleProject);
    EXPECT_EQ(parse_mode("bundle-all"),     Mode::BundleAll);
}

TEST(PackModes, UnknownIsNullopt) {
    EXPECT_FALSE(parse_mode("nonsense").has_value());
}

TEST(PackModes, CliNamesAreCanonical) {
    EXPECT_EQ(mode_cli_name(Mode::None),          "system");
    EXPECT_EQ(mode_cli_name(Mode::BundleProject), "vendored");
    EXPECT_EQ(mode_cli_name(Mode::BundleAll),     "self-contained");
    EXPECT_EQ(mode_cli_name(Mode::Static),        "static");
}

// ── #630 §3: what happens when a format's dependency closure is unavailable,
// as a function of the format alone ──────────────────────────────────────
//
// The archive formats (`tar`, `dir`) ARE the closure -- an unavailable one is
// the command failing, exactly as before `pack::run` staged declared files
// ahead of the closure walk. A dispatched format receives the staged tree
// regardless: a provider may not need a closure at all.

TEST(PackClosureUnavailableOutcome, ArchiveFormatsFailTheCommand) {
    EXPECT_EQ(closure_unavailable_outcome(Format::Tar),
              ClosureUnavailableOutcome::CommandFails);
    EXPECT_EQ(closure_unavailable_outcome(Format::Dir),
              ClosureUnavailableOutcome::CommandFails);
}

TEST(PackClosureUnavailableOutcome, DispatchedFormatStagesWithoutIt) {
    EXPECT_EQ(closure_unavailable_outcome(Format::Dispatched),
              ClosureUnavailableOutcome::StageWithoutClosure);
}

// `--systems` (design 2026-10-29 §4.1): `<os>` for every architecture of the
// legs, `<os>-<arch>` for one, `all` for the three systems; unstated, this
// machine's.
TEST(PackSystems, TheWordsNameSystemsForAnArchitecture) {
    using V = std::vector<std::string>;
    EXPECT_EQ(mcpp::pack::systems_for({ "linux", "macos-aarch64" }, "x86_64"), V{ "linux" });
    EXPECT_EQ(mcpp::pack::systems_for({ "linux", "macos-aarch64" }, "aarch64"), (V{ "linux", "macos" }));
    EXPECT_EQ(mcpp::pack::systems_for({ "all", "linux" }, "x86_64"), (V{ "linux", "macos", "windows" }));
    EXPECT_EQ(mcpp::pack::systems_for({}, "x86_64").size(), 1u);
}

// `-o` names a directory when it ends in a separator or is one (design
// 2026-10-29 §4.2).
TEST(PackSystems, AnOutputEndingInASeparatorIsADirectory) {
    EXPECT_TRUE(mcpp::pack::output_names_directory("dist/"));
    EXPECT_FALSE(mcpp::pack::output_names_directory("dist/app.tar.gz"));
    EXPECT_FALSE(mcpp::pack::output_names_directory(""));
    EXPECT_TRUE(mcpp::pack::output_names_directory(std::filesystem::temp_directory_path()));
}
