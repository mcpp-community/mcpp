#include <gtest/gtest.h>

import std;
import mcpp.version;
import mcpp.version_req;

// SUBSYSTEM-LEVEL: the two modules here answer different questions and are
// tested for that difference. `mcpp.version` is this binary's identity;
// `mcpp.version_req` is the grammar every OTHER package's version is read
// with. Nothing outside this package is involved.

TEST(Versioning, ThisBinaryDeclaresAFourSegmentDateVersion) {
    // ⚠️ The same string lives in the root mcpp.toml, and
    // .github/tools/check_version_pins.sh keeps the two equal. This states the
    // SHAPE, which that checker cannot: a version that lost a segment would
    // still match the manifest and still be wrong.
    const std::string v{mcpp::MCPP_VERSION};
    EXPECT_FALSE(v.empty());
    EXPECT_EQ(std::ranges::count(v, '.'), 3) << "expected YYYY.M.D.N, got " << v;
}

TEST(Versioning, TheFourthSegmentSurvivesAParseAndPrint) {
    // Round-tripping is load-bearing: the resolver rebuilds dependency version
    // strings from this, and they flow into the lock file and the xlings wire
    // address. A `.0` collapsing to three segments would rename a package.
    auto v = mcpp::version_req::parse_version("2026.8.29.1");
    ASSERT_TRUE(v.has_value()) << v.error();
    EXPECT_EQ(v->str(), "2026.8.29.1");

    auto z = mcpp::version_req::parse_version("2026.8.29.0");
    ASSERT_TRUE(z.has_value()) << z.error();
    EXPECT_EQ(z->str(), "2026.8.29.0");
}

TEST(Versioning, TheDateSchemeSortsAboveTheOldThreeSegmentOne) {
    // `0.0.109` < `2026.7.27.1`: the first segment goes from 0 to 2026 and
    // never comes back. Stated here because the ordering is what stops a
    // released mcpp from resolving backwards into the pre-date scheme.
    auto older = mcpp::version_req::parse_version("0.0.109");
    auto newer = mcpp::version_req::parse_version("2026.7.27.1");
    ASSERT_TRUE(older.has_value());
    ASSERT_TRUE(newer.has_value());
    EXPECT_TRUE(*older < *newer);
}
