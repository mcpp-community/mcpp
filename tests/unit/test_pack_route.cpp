
import std;
import mcpplibs.mtest;
import mcpp.pack.route;

using namespace mcpplibs::mtest;

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

UTest an_app_on_several_android_rows_is_accepted{"AcceptsSeveralTargets/AnAppOnSeveralAndroidRowsIsAccepted", [] {
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-android"};
    expect(bool(accepts_several_targets(app_route(), triples)));
}};

UTest an_app_mixed_with_an_executable_row_is_refused{"AcceptsSeveralTargets/AnAppMixedWithAnExecutableRowIsRefused", [] {
    // The negative direction inside "an app": one Android row and one row
    // whose form is an executable must not slip through as "every row is a
    // shared object" — a mixed request is not a request this route can
    // satisfy either.
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-gnu"};
    expect(!(accepts_several_targets(app_route(), triples)));
}};

UTest an_app_on_executable_rows_only_is_refused{"AcceptsSeveralTargets/AnAppOnExecutableRowsOnlyIsRefused", [] {
    std::vector<std::string> triples{"x86_64-linux-gnu", "aarch64-macos"};
    expect(!(accepts_several_targets(app_route(), triples)));
}};

UTest a_bin_target_is_always_refused_even_on_android_rows{"AcceptsSeveralTargets/ABinTargetIsAlwaysRefusedEvenOnAndroidRows", [] {
    // `kind = "bin"` never resolves to a shared-object form on any row
    // (`toolchain::triple::application_form` only answers `SharedObject` for
    // `kind = "app"`), so the several-triple route is not this target's,
    // whatever triples are named.
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-android"};
    expect(!(accepts_several_targets(bin_route(), triples)));
}};

UTest an_unparseable_triple_is_refused_not_ignored{"AcceptsSeveralTargets/AnUnparseableTripleIsRefusedNotIgnored", [] {
    std::vector<std::string> triples{"aarch64-linux-android", "not a triple"};
    expect(!(accepts_several_targets(app_route(), triples)));
}};

UTest a_library_route_is_unaffected{"AcceptsSeveralTargets/ALibraryRouteIsUnaffected", [] {
    // `accepts_several_targets` is never consulted for a library route in
    // `cmd_pack` (libraries already take several triples unconditionally);
    // asked anyway, it answers false because `isApplication` is false for a
    // library `PackRoute`, which keeps the predicate honest about what it
    // actually decides.
    std::vector<std::string> triples{"aarch64-linux-android", "x86_64-linux-android"};
    expect(!(accepts_several_targets(library_route(), triples)));
}};
