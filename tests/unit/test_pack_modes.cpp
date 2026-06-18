#include <gtest/gtest.h>

import std;
import mcpp.pack;

using mcpp::pack::Mode;
using mcpp::pack::parse_mode;
using mcpp::pack::mode_cli_name;

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
