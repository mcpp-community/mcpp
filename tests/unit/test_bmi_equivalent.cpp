// mcpp.build.stage::bmi_equivalent — the comparison that decides whether an
// importer must be rebuilt.
//
// Asserted from BOTH sides on purpose. A function that always returned `true`
// would pass every "equivalent BMIs compare equal" test while silently
// suppressing every legitimate cascade — which is a far worse bug than the one
// it replaces. Half of these cases exist to catch that.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

import mcpp.build.stage;

namespace {

std::filesystem::path tmpdir() {
    auto d = std::filesystem::temp_directory_path() /
             ("mcpp-bmi-eq-" + std::to_string(::getpid()));
    std::filesystem::create_directories(d);
    return d;
}

std::filesystem::path write(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}

// A stand-in for the shape GCC actually emits, verified against a real .gcm:
//   ...export.repository: gcm.cache\0buildtime: 2026/08/12 02:25:01 UTC\0...
std::string bmi_like(const std::string& stamp, const std::string& payload = "PAYLOAD") {
    return "GCM\x01" + payload + std::string("\0", 1) +
           "buildtime: " + stamp + std::string("\0", 1) +
           "localtime: " + stamp + std::string("\0", 1) + "TAIL";
}

}  // namespace

TEST(BmiEquivalent, IdenticalFilesAreEquivalent) {
    const auto d = tmpdir();
    auto a = write(d / "a.gcm", bmi_like("2026/08/12 02:25:01 UTC"));
    auto b = write(d / "b.gcm", bmi_like("2026/08/12 02:25:01 UTC"));
    EXPECT_TRUE(mcpp::build::stage::bmi_equivalent(a, b));
}

// The whole reason this function exists: GCC stamps a wall clock into the BMI,
// so two compiles of identical source differ by a few bytes and `cmp` reports
// "changed" every single time.
TEST(BmiEquivalent, DifferingOnlyInTheEmbeddedTimestamp) {
    const auto d = tmpdir();
    auto a = write(d / "t1.gcm", bmi_like("2026/08/12 02:25:01 UTC"));
    auto b = write(d / "t2.gcm", bmi_like("2026/08/12 02:25:33 UTC"));
    EXPECT_TRUE(mcpp::build::stage::bmi_equivalent(a, b));
}

// The side that must NOT be lost: a real interface change still cascades.
TEST(BmiEquivalent, DifferingPayloadIsNotEquivalent) {
    const auto d = tmpdir();
    auto a = write(d / "p1.gcm", bmi_like("2026/08/12 02:25:01 UTC", "PAYLOAD"));
    auto b = write(d / "p2.gcm", bmi_like("2026/08/12 02:25:01 UTC", "PAYLOAX"));
    EXPECT_FALSE(mcpp::build::stage::bmi_equivalent(a, b));
}

// A payload difference must not be masked just because a timestamp is nearby.
TEST(BmiEquivalent, PayloadDifferenceIsNotHiddenByATimestampDifference) {
    const auto d = tmpdir();
    auto a = write(d / "m1.gcm", bmi_like("2026/08/12 02:25:01 UTC", "PAYLOAD"));
    auto b = write(d / "m2.gcm", bmi_like("2026/08/12 09:59:59 UTC", "PAYLOAX"));
    EXPECT_FALSE(mcpp::build::stage::bmi_equivalent(a, b));
}

TEST(BmiEquivalent, DifferentSizesAreNeverEquivalent) {
    const auto d = tmpdir();
    auto a = write(d / "s1.gcm", bmi_like("2026/08/12 02:25:01 UTC"));
    auto b = write(d / "s2.gcm", bmi_like("2026/08/12 02:25:01 UTC") + "EXTRA");
    EXPECT_FALSE(mcpp::build::stage::bmi_equivalent(a, b));
}

// No stamps at all → strict comparison, which is the old behaviour. This is the
// conservative fallback that keeps the function from ever being MORE permissive
// than a byte compare on inputs it does not understand.
TEST(BmiEquivalent, WithoutStampsItIsAStrictCompare) {
    const auto d = tmpdir();
    auto a = write(d / "n1.gcm", "no stamps here");
    auto b = write(d / "n2.gcm", "no stamps here");
    auto c = write(d / "n3.gcm", "no stamps HERE");
    EXPECT_TRUE(mcpp::build::stage::bmi_equivalent(a, b));
    EXPECT_FALSE(mcpp::build::stage::bmi_equivalent(a, c));
}

// A value that merely follows the prefix but is not a timestamp must not be
// masked — otherwise an attacker-shaped or simply unusual BMI could hide a real
// difference behind the literal text "buildtime: ".
TEST(BmiEquivalent, NonTimestampAfterThePrefixIsNotMasked) {
    const auto d = tmpdir();
    auto a = write(d / "f1.gcm", "buildtime: not-a-timestamp-xxxxxA");
    auto b = write(d / "f2.gcm", "buildtime: not-a-timestamp-xxxxxB");
    EXPECT_FALSE(mcpp::build::stage::bmi_equivalent(a, b));
}

TEST(BmiEquivalent, MissingFileIsNotEquivalent) {
    const auto d = tmpdir();
    auto a = write(d / "e1.gcm", bmi_like("2026/08/12 02:25:01 UTC"));
    EXPECT_FALSE(mcpp::build::stage::bmi_equivalent(a, d / "does-not-exist.gcm"));
}
