// The run-time search-path contract: rank, provenance, machine-locality.
//
// This module is pure policy — it never touches the filesystem and knows
// nothing about ELF — so it is the one part of the closure that can be pinned
// exactly, on every platform, including the ones where none of it applies.
// Everything downstream (the DT_RPATH mcpp emits, what `mcpp pack` strips,
// what the closure resolver walks) is a consequence of these four answers.


import std;
import mcpplibs.mtest;
import mcpp.runtime.search;

using namespace mcpplibs::mtest;

namespace search = mcpp::runtime::search;
using search::Origin;

namespace {

// THE invariant. Payload before farm is not a preference: `<subos>/lib` is a
// symlink view rewritten by every `xlings install`, a payload directory is
// written once, and an already-linked artifact must not have its libc changed
// out from under it by a later install.
UTest payload_outranks_farm{"RuntimeSearch/PayloadOutranksFarm", [] {
    expect(lt(search::rank(Origin::Payload), search::rank(Origin::SubosFarm)));
    expect(lt(search::rank(Origin::Package), search::rank(Origin::SubosFarm)));
    expect(lt(search::rank(Origin::SubosFarm), search::rank(Origin::HostDefault)));

    // #415 — the artifact's own directory (`$ORIGIN`) sits between the packages
    // and the farm. This is the ordering #414 was about: with the farm ahead of
    // `$ORIGIN` an artifact linked one libX11 and loaded another. A pinned
    // payload still wins, because that is the one thing nothing re-points.
    expect(lt(search::rank(Origin::Package), search::rank(Origin::Artifact)));
    expect(lt(search::rank(Origin::Artifact), search::rank(Origin::SubosFarm)));
    expect(lt(search::rank(Origin::Payload), search::rank(Origin::Artifact)));
}};

// `mcpp pack` asks this to decide what may not be baked into a distributable.
// The host's own /usr/lib is the only one that is NOT this machine's private
// state — relying on it is an ordinary host requirement, not a dependency on
// the build box.
UTest machine_local_is_everything_but_the_host_defaults{"RuntimeSearch/MachineLocalIsEverythingButTheHostDefaults", [] {
    expect(search::is_machine_local(Origin::Payload));
    expect(search::is_machine_local(Origin::Package));
    expect(search::is_machine_local(Origin::SubosFarm));
    expect(!search::is_machine_local(Origin::HostDefault));

    // `$ORIGIN` is NOT machine-local, and getting this backwards would be
    // worse than the gap it closes: it is resolved by the loader relative to
    // the artifact, so it means the same thing wherever that artifact is
    // copied — `pack` REWRITES everything else into this form. Marking it
    // local would make pack reject the one entry it is trying to produce.
    expect(!search::is_machine_local(Origin::Artifact));
}};

// These strings are PUBLISHED — they are the `origin` field of every entry in
// `resolution.json`'s `runtime.search.closure`, which CI, `mcpp why runtime` and
// e2e 219 all read. Renaming one is a wire-format change, so it is pinned here
// rather than left to whatever `to_string` happens to say.
UTest origin_names_are_published_and_stable{"RuntimeSearch/OriginNamesArePublishedAndStable", [] {
    expect(eq(search::to_string(Origin::Payload), "payload"));
    expect(eq(search::to_string(Origin::Package), "package"));
    expect(eq(search::to_string(Origin::Artifact), "artifact"));
    expect(eq(search::to_string(Origin::SubosFarm), "subos_farm"));
    expect(eq(search::to_string(Origin::HostDefault), "host_default"));
}};

UTest ordered_sorts_by_rank_not_by_insertion{"RuntimeSearch/OrderedSortsByRankNotByInsertion", [] {
    std::vector<search::Dir> input{
        {"/farm", Origin::SubosFarm},
        {"/pkg",  Origin::Package},
        {"/pay",  Origin::Payload},
    };
    auto out = search::ordered(input);
    expect(fatal(eq(out.size(), 3u)));
    expect(eq(out[0].path, std::filesystem::path("/pay")));
    expect(eq(out[1].path, std::filesystem::path("/pkg")));
    expect(eq(out[2].path, std::filesystem::path("/farm")));
}};

// Stable within a rank, because insertion order still decides among peers —
// libglvnd resolves GL vendors by exactly that order, so reordering equals
// would change which driver a program gets.
UTest ordered_is_stable_within_a_rank{"RuntimeSearch/OrderedIsStableWithinARank", [] {
    std::vector<search::Dir> input{
        {"/pay/b", Origin::Payload},
        {"/pay/a", Origin::Payload},
        {"/pay/c", Origin::Payload},
    };
    auto out = search::ordered(input);
    expect(fatal(eq(out.size(), 3u)));
    expect(eq(out[0].path, std::filesystem::path("/pay/b")));
    expect(eq(out[1].path, std::filesystem::path("/pay/a")));
    expect(eq(out[2].path, std::filesystem::path("/pay/c")));
}};

// The same directory reachable two ways is ONE entry, and it takes the
// stronger origin. A payload that is also visible through the farm view is a
// payload; ranking it as farm would push the real libc behind everything else
// the farm holds — the exact failure the ordering exists to prevent.
UTest duplicate_path_keeps_the_strongest_origin{"RuntimeSearch/DuplicatePathKeepsTheStrongestOrigin", [] {
    std::vector<search::Dir> input{
        {"/shared", Origin::SubosFarm},
        {"/shared", Origin::Payload},
        {"/other",  Origin::SubosFarm},
    };
    auto out = search::ordered(input);
    expect(fatal(eq(out.size(), 2u)));
    expect(eq(out[0].path, std::filesystem::path("/shared")));
    expect(eq(out[0].origin, Origin::Payload));
    expect(eq(out[1].origin, Origin::SubosFarm));
}};

UTest ordered_normalizes_and_drops_empty{"RuntimeSearch/OrderedNormalizesAndDropsEmpty", [] {
    std::vector<search::Dir> input{
        {"", Origin::Payload},
        {"/a/./b/", Origin::Payload},
        {"/a/b", Origin::Payload},
    };
    auto out = search::ordered(input);
    expect(fatal(eq(out.size(), 1u)));
    expect(eq(out[0].path, std::filesystem::path("/a/b")));
}};

// The opt-out is a CROSS-REPO name (openxlings/xlings#540). Pinning the
// spelling here means a rename shows up as a failed test rather than as a
// silently ineffective declaration — the failure mode of an env var nobody
// reads is that everything looks fine.
UTest linker_opt_out_is_spelled_once{"RuntimeSearch/LinkerOptOutIsSpelledOnce", [] {
    expect(eq(search::kLinkerPathInjectionOptOut, "XLINGS_SUBOS_LD_PATHS"));
    expect(eq(search::kLinkerPathInjectionOptOutValue, "0"));
}};

} // namespace
