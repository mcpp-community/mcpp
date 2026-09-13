#include <gtest/gtest.h>

import std;
import mcpp.pack.route;

using namespace mcpp::pack;

// #630 A9: the route is chosen by the artifact's FORM, not by the target's
// kind. `accepts_several_targets` is the pure predicate `cmd_pack` asks
// before it builds anything — a program route serves more than one
// `--target` only when EVERY requested row resolves to a shared-object form,
// which today means every row is an Android row and the target is a
// `kind = "app"` (`PackRoute::isApplication`).

namespace {

PackRoute app_route()     { return PackRoute{ "myapp", /*library=*/false, /*isApplication=*/true }; }
PackRoute bin_route()     { return PackRoute{ "myapp", /*library=*/false, /*isApplication=*/false }; }
PackRoute library_route() { return PackRoute{ "mylib", /*library=*/true,  /*isApplication=*/false }; }

} // namespace

TEST(AcceptsSeveralTargets, AnAppOnSeveralAndroidRowsIsAccepted) {
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-android"};
    EXPECT_TRUE(accepts_several_targets(app_route(), triples));
}

TEST(AcceptsSeveralTargets, AnAppMixedWithAnExecutableRowIsRefused) {
    // The negative direction inside "an app": one Android row and one row
    // whose form is an executable must not slip through as "every row is a
    // shared object" — a mixed request is not a request this route can
    // satisfy either.
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-gnu"};
    EXPECT_FALSE(accepts_several_targets(app_route(), triples));
}

TEST(AcceptsSeveralTargets, AnAppOnExecutableRowsOnlyIsRefused) {
    std::vector<std::string> triples{"x86_64-linux-gnu", "aarch64-macos"};
    EXPECT_FALSE(accepts_several_targets(app_route(), triples));
}

TEST(AcceptsSeveralTargets, ABinTargetIsAlwaysRefusedEvenOnAndroidRows) {
    // `kind = "bin"` never resolves to a shared-object form on any row
    // (`toolchain::triple::application_form` only answers `SharedObject` for
    // `kind = "app"`), so the several-triple route is not this target's,
    // whatever triples are named.
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-android"};
    EXPECT_FALSE(accepts_several_targets(bin_route(), triples));
}

TEST(AcceptsSeveralTargets, AnUnparseableTripleIsRefusedNotIgnored) {
    std::vector<std::string> triples{"aarch64-linux-android", "not a triple"};
    EXPECT_FALSE(accepts_several_targets(app_route(), triples));
}

TEST(AcceptsSeveralTargets, ALibraryRouteIsUnaffected) {
    // `accepts_several_targets` is never consulted for a library route in
    // `cmd_pack` (libraries already take several triples unconditionally);
    // asked anyway, it answers false because `isApplication` is false for a
    // library `PackRoute`, which keeps the predicate honest about what it
    // actually decides.
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-android"};
    EXPECT_FALSE(accepts_several_targets(library_route(), triples));
}
