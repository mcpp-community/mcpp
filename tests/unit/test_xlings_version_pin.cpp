// The vendored xlings is replaced when it is BEHIND the pin, and only then.
//
// acquire_xlings_binary used to return on mere existence, so a home kept
// whatever xlings it first acquired forever -- measured at 2026.8.2.1 against
// a pin of 2026.8.6.3, with `mcpp self env` printing both numbers next to each
// other and saying nothing. That is not cosmetic: the subos_info block arrived
// in xlings 2026.8.5.1, so on that machine the graphics packages' declarations
// were discarded by a client too old to have the API, and the fix for
// mcpp#352 could never take effect no matter how current mcpp itself was.
//
// The comparison is STRICTLY older, not "not equal". A user who deliberately
// put a newer xlings there must not be downgraded by an mcpp pinning an older
// one, and an unparseable version is not evidence of being behind.

#include <gtest/gtest.h>

import std;
import mcpp.fallback.xlings_binary;

namespace fb = mcpp::fallback;

namespace {

TEST(XlingsVersionPin, OlderIsBehind) {
    EXPECT_TRUE(fb::version_is_older("2026.8.2.1", "2026.8.6.3"));
    EXPECT_TRUE(fb::version_is_older("2026.7.31.3", "2026.8.1.1"));
    EXPECT_TRUE(fb::version_is_older("2026.8.6", "2026.8.6.1"));
}

TEST(XlingsVersionPin, EqualIsNotBehind) {
    EXPECT_FALSE(fb::version_is_older("2026.8.6.3", "2026.8.6.3"));
    // Trailing zeros compare equal, not older.
    EXPECT_FALSE(fb::version_is_older("2026.8.6.0", "2026.8.6"));
}

// The case that decides whether this is safe to run automatically: a user who
// put a NEWER xlings in place keeps it.
TEST(XlingsVersionPin, NewerIsNotDowngraded) {
    EXPECT_FALSE(fb::version_is_older("2026.9.1.1", "2026.8.6.3"));
    EXPECT_FALSE(fb::version_is_older("2027.1.1.1", "2026.12.31.9"));
}

// A version this code cannot parse says nothing, and "says nothing" must not
// be read as "is behind" -- that would delete a working binary on a guess.
TEST(XlingsVersionPin, UnparseableIsNotBehind) {
    EXPECT_FALSE(fb::version_is_older("dev", "2026.8.6.3"));
    EXPECT_FALSE(fb::version_is_older("2026.8.6.3", ""));
    EXPECT_FALSE(fb::version_is_older("", "2026.8.6.3"));
    EXPECT_FALSE(fb::version_is_older("2026.8.x", "2026.8.6.3"));
}

// The version scheme changed epochs: xlings went 0.4.x -> YYYY.M.D.N. Both
// live on the same disk, and a home's system xlings may still be a 0.4.x while
// its vendored one is already dated. Getting this backwards is not academic --
// the first cut of the replace-when-behind logic deleted the vendored binary
// and re-acquired from `which xlings`, which on this developer machine turned
// 2026.8.2.1 into 0.4.51: older still, and equally missing the very feature
// the replacement existed to restore. Being behind the pin justifies looking
// for a replacement; it does not justify accepting whatever turns up.
TEST(XlingsVersionPin, DatedSchemeIsNewerThanTheOldOne) {
    EXPECT_FALSE(fb::version_is_older("2026.8.2.1", "0.4.51"));
    EXPECT_TRUE(fb::version_is_older("0.4.51", "2026.8.2.1"));
    EXPECT_TRUE(fb::version_is_older("0.4.51", "0.4.54"));
}

}  // namespace
