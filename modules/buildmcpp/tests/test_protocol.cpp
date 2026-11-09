
import std;
import mcpplibs.mtest;
import mcpp.build.program_protocol;

using namespace mcpplibs::mtest;

// A SUBSYSTEM-LEVEL test: it knows only this package, and it is the only place
// the protocol's own invariants can be stated without an engine around them.
//
// The engine-level counterpart lives in the root `tests/unit/`, where the same
// values are checked as the build actually uses them. Two tests, two failure
// meanings: one says "the contract changed", the other says "a consumer broke".

namespace pp = mcpp::build::program_protocol;

UTest the_version_is_positive_and_monotone{"ProgramProtocol/TheVersionIsPositiveAndMonotone", [] {
    // A protocol version of zero would make "declared nothing" and "declared
    // version zero" the same observation, which is the distinction the whole
    // unknown-directive rule rests on.
    expect(gt(pp::kProtocolVersion, 0));
}};

UTest the_cache_epoch_is_positive{"ProgramProtocol/TheCacheEpochIsPositive", [] {
    expect(gt(pp::kCacheEpoch, 0));
}};
